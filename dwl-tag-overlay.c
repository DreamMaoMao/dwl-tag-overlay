#include "dwl-ipc-unstable-v2.h"
#include "wlr-layer-shell-unstable-v1.h"
#include <fcntl.h>
#include <freetype2/ft2build.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

// FreeType 全局变量
FT_Library ft_library;
FT_Face ft_face;
const char *font_path = NULL;
int font_size = 30;

// 新增函数：获取系统默认字体路径
const char *get_system_font() {
	static char font_path_buf[4096] = {0};
	if (font_path_buf[0])
		return font_path_buf;

	FcInit();
	FcPattern *pattern = FcPatternCreate();
	FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)"sans-serif");
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);

	FcConfig *config = FcInitLoadConfigAndFonts();
	FcResult result;
	FcPattern *match = FcFontMatch(config, pattern, &result);

	if (match) {
		FcChar8 *path;
		if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch) {
			strncpy(font_path_buf, (char *)path, sizeof(font_path_buf) - 1);
			font_path_buf[sizeof(font_path_buf) - 1] = '\0';
		}
		FcPatternDestroy(match);
	}

	FcPatternDestroy(pattern);
	FcConfigDestroy(config);

	if (!font_path_buf[0]) {
		fprintf(stderr, "无法找到系统字体\n");
		exit(EXIT_FAILURE);
	}

	return font_path_buf;
}

// 可配置样式参数
uint32_t color_border = 0xffDDCA9E;
uint32_t color_bg_inactive = 0xff201B14;
uint32_t color_fg_inactive = 0xffDDCA9E;
uint32_t color_bg_active = 0xffDDCA9E;
uint32_t color_fg_active = 0x00000000;
uint32_t color_bg_occupied = 0xff201B14;
uint32_t color_fg_occupied = 0xffDDCA9E;

struct wl_compositor *comp;
struct wl_surface *srfc;
struct wl_buffer *bfr;
struct wl_shm *shm;
struct wl_display *disp;
struct wl_output *wl_output = NULL;
struct zwlr_layer_shell_v1 *layer_shell = NULL;
struct zwlr_layer_surface_v1 *layer_surface = NULL;
struct zdwl_ipc_manager_v2 *ipc_manager = NULL;
struct zdwl_ipc_output_v2 *ipc_output = NULL;
uint8_t need_to_draw = 0;
uint8_t tag_count = 9;
uint8_t tag_states[9] = {0};
uint8_t new_tag_states[9] = {0};
uint32_t timeout = 500;
struct timespec last_frame;
uint8_t *pixl;
uint16_t w = 500;
uint16_t h = 30;
uint8_t visible = 0; // 新增：跟踪可见状态
char *custom_tags = NULL;

void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
							 uint32_t serial, uint32_t width, uint32_t height);

void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface);

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

// 新增：surface创建函数
void create_surface() {
	srfc = wl_compositor_create_surface(comp);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, srfc, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "overlay");
	zwlr_layer_surface_v1_set_size(layer_surface, w, h);
	zwlr_layer_surface_v1_set_anchor(layer_surface,
									 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);
	zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, 20, 0);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener,
									   0);
	wl_surface_commit(srfc);
	visible = 1;
}

// 新增：surface销毁函数
void destroy_surface() {
	if (layer_surface) {
		zwlr_layer_surface_v1_destroy(layer_surface);
		layer_surface = NULL;
	}
	if (srfc) {
		wl_surface_destroy(srfc);
		srfc = NULL;
	}
	if (bfr) {
		wl_buffer_destroy(bfr);
		bfr = NULL;
	}
	if (pixl) {
		munmap(pixl, (size_t)(w * h * 4));
		pixl = NULL;
	}
	visible = 0;
}

void usage(char *name) {
	fprintf(stderr,
			"Usage: %s [-b border_color] [-I inactive_bg] [-i inactive_fg]\n"
			"          [-A active_bg] [-a active_fg] [-O occupied_bg] [-o "
			"occupied_fg] [-l \"tags\"] [-t timeout]\n",
			name);
	exit(EXIT_FAILURE);
}

int32_t alc_shm(uint64_t sz) {
	char name[8];
	name[0] = '/';
	name[7] = 0;
	for (uint8_t i = 1; i < 6; i++) {
		name[i] = (rand() & 23) + 97;
	}
	shm_unlink(name);
	int32_t fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	ftruncate(fd, (off_t)sz);
	return fd;
}

void resz() {
	uint64_t size = (uint64_t)w * (uint64_t)h * 4;
	int32_t fd = alc_shm(size);
	pixl = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, (int)fd, (int32_t)size);
	bfr =
		wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
}

void fill_rect(int x, int y, int width, int height, uint32_t color) {
	uint32_t *pixels = (uint32_t *)pixl;
	for (int py = y; py < y + height && py < h; py++) {
		for (int px = x; px < x + width && px < w; px++) {
			pixels[py * w + px] = color;
		}
	}
}

void draw_char(int x, int y, char c, uint32_t color) {
	if (FT_Load_Char(ft_face, (FT_ULong)(unsigned char)c,
					 FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) {
		fprintf(stderr, "无法加载字符 '%c'\n", c);
		return;
	}
	FT_GlyphSlot glyph = ft_face->glyph;
	FT_Bitmap *bitmap = &glyph->bitmap;

	int ox = x + glyph->bitmap_left;
	int oy =
		y + (int)(ft_face->size->metrics.ascender / 64) - glyph->bitmap_top;

	for (int row = 0; row < (int)bitmap->rows; row++) {
		for (int col = 0; col < (int)bitmap->width; col++) {
			ptrdiff_t px = (ptrdiff_t)ox + (ptrdiff_t)col;
			ptrdiff_t py = (ptrdiff_t)oy + (ptrdiff_t)row;

			if (px >= 0 && py >= 0 && px < w && py < h) {
				size_t index =
					(size_t)row * (size_t)bitmap->pitch + (size_t)col;
				uint8_t alpha = bitmap->buffer[index];

				if (alpha > 0) {
					uint32_t *dst = (uint32_t *)pixl + py * w + px;

					uint8_t r = (color >> 16) & 0xFF;
					uint8_t g = ((color >> 8) & 0xFF);
					uint8_t b = color & 0xFF;

					uint8_t dst_r = ((*dst) >> 16) & 0xFF;
					uint8_t dst_g = ((*dst) >> 8) & 0xFF;
					uint8_t dst_b = (*dst) & 0xFF;

					uint8_t src_a = alpha;

					uint8_t out_r =
						(uint8_t)((r * src_a + dst_r * (255U - src_a)) / 255U);
					uint8_t out_g =
						(uint8_t)((g * src_a + dst_g * (255U - src_a)) / 255U);
					uint8_t out_b =
						(uint8_t)((b * src_a + dst_b * (255U - src_a)) / 255U);

					*dst = (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
				}
			}
		}
	}
}

void draw_border() {
	const int border_width = 2;
	for (int y = 0; y < border_width; y++) {
		fill_rect(0, y, w, 1, color_border);
		fill_rect(0, h - y - 1, w, 1, color_border);
	}
	for (int x = 0; x < border_width; x++) {
		for (int y = border_width; y < h - border_width; y++) {
			((uint32_t *)pixl)[y * w + x] = color_border;
			((uint32_t *)pixl)[y * w + (w - x - 1)] = color_border;
		}
	}
}

void draw_tag_bar() {
	draw_border();

	int pad = 4;
	int spacing = (w - 2 * pad) / tag_count;
	const char *tags = custom_tags ? custom_tags : "123456789";

	for (uint32_t i = 0; i < tag_count; i++) {
		int x = pad + (int)i * spacing;
		int width = spacing;
		if (i == (uint32_t)(tag_count - 1)) {
			width = w - 2 * pad - (int)i * spacing;
		}

		uint32_t bg;
		uint32_t fg;

		switch (tag_states[i]) {
		case 0: // 未激活
			bg = color_bg_inactive;
			fg = color_fg_inactive;
			break;
		case 1: // 激活
			bg = color_bg_active;
			fg = color_fg_active;
			break;
		case 2: // 被占用但未激活
			bg = color_bg_occupied;
			fg = color_fg_occupied;
			break;
		default:
			bg = color_bg_inactive;
			fg = color_fg_inactive;
			break;
		}

		fill_rect(x, 2, width, h - 4, bg);

		// 文字绘制
		char c = tags[i];
		int text_y = (int)(h - ft_face->size->metrics.height / 64) / 2;
		draw_char(x + (width - 8) / 2, text_y, c, fg);

		// 分隔线
		if (i > 0) {
			fill_rect(x, 0, 1, h, color_border);
		}
	}
}

void draw() {

	if (visible && !need_to_draw) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - last_frame.tv_sec) * 1000 +
						  (now.tv_nsec - last_frame.tv_nsec) / 1000000;

		if (elapsed_ms >= timeout) {
			// 超时后销毁surface
			destroy_surface();
			need_to_draw = 0;
			return;
		}
	}

	if (need_to_draw) {
		draw_tag_bar();
		clock_gettime(CLOCK_MONOTONIC, &last_frame);
		need_to_draw = 0;
	}

	if (!srfc || !bfr)
		return;

	wl_surface_attach(srfc, bfr, 0, 0);
	wl_surface_damage_buffer(srfc, 0, 0, w, h);
	wl_surface_commit(srfc);
}

struct wl_callback_listener cb_list;
void frame_new(void *data, struct wl_callback *cb, uint32_t a) {
	wl_callback_destroy(cb);
	cb = wl_surface_frame(srfc);
	wl_callback_add_listener(cb, &cb_list, 0);
	draw();
}
struct wl_callback_listener cb_list = {.done = frame_new};

void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
							 uint32_t serial, uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	if ((uint16_t)width != w || (uint16_t)height != h) {
		if (pixl) {
			munmap(pixl, (size_t)(w * h * 4));
			pixl = NULL;
		}
		w = (uint16_t)width;
		h = (uint16_t)height;
		resz();
	}
	if (!pixl) {
		resz();
	}
	draw();
}

void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	destroy_surface();
}

void ipc_manager_tags(void *data, struct zdwl_ipc_manager_v2 *manager,
					  uint32_t tags) {
	tag_count = (uint8_t)tags;
}
void ipc_manager_layout(void *data, struct zdwl_ipc_manager_v2 *manager,
						const char *layout) {}
struct zdwl_ipc_manager_v2_listener ipc_manager_listener = {
	.tags = ipc_manager_tags,
	.layout = ipc_manager_layout,
};

void ipc_output_tag(void *data, struct zdwl_ipc_output_v2 *output, uint32_t tag,
					uint32_t state, uint32_t clients, uint32_t focused) {

	if (tag >= tag_count)
		return;

	if (state == 0 && clients > 0) {
		new_tag_states[tag] = 2;
	} else {
		new_tag_states[tag] = (uint8_t)state;
	}

	if (tag == (uint32_t)(tag_count - 1) &&
		memcmp(tag_states, new_tag_states, tag_count)) {
		memcpy(tag_states, new_tag_states, tag_count);
		need_to_draw = 1;

		// 如果surface已销毁，重新创建
		if (!visible) {
			if (srfc)
				wl_surface_destroy(srfc);
			if (layer_surface)
				zwlr_layer_surface_v1_destroy(layer_surface);
			create_surface();
			struct wl_callback *cb = wl_surface_frame(srfc);
			wl_callback_add_listener(cb, &cb_list, 0);
			resz();
		}
	}
}

void ipc_output_active(void *data, struct zdwl_ipc_output_v2 *output,
					   uint32_t active) {}
void ipc_output_layout(void *data, struct zdwl_ipc_output_v2 *output,
					   uint32_t layout) {}
void ipc_output_title(void *data, struct zdwl_ipc_output_v2 *output,
					  const char *title) {}
void ipc_output_appid(void *data, struct zdwl_ipc_output_v2 *output,
					  const char *appid) {}
void ipc_output_layout_symbol(void *data, struct zdwl_ipc_output_v2 *output,
							  const char *layout) {}
void ipc_output_frame(void *data, struct zdwl_ipc_output_v2 *output) {}
void ipc_output_fullscreen(void *data, struct zdwl_ipc_output_v2 *output,
						   uint32_t is_fullscreen) {}
void ipc_output_floating(void *data, struct zdwl_ipc_output_v2 *output,
						 uint32_t is_floating) {}
void ipc_output_x(void *data, struct zdwl_ipc_output_v2 *output, int x) {}
void ipc_output_y(void *data, struct zdwl_ipc_output_v2 *output, int y) {}
void ipc_output_width(void *data, struct zdwl_ipc_output_v2 *output,
					  int width) {}
void ipc_output_height(void *data, struct zdwl_ipc_output_v2 *output,
					   int height) {}
struct zdwl_ipc_output_v2_listener ipc_output_listener = {
	.tag = ipc_output_tag,
	.active = ipc_output_active,
	.layout = ipc_output_layout,
	.title = ipc_output_title,
	.appid = ipc_output_appid,
	.layout_symbol = ipc_output_layout_symbol,
	.frame = ipc_output_frame,
	.fullscreen = ipc_output_fullscreen,
	.floating = ipc_output_floating,
	.x = ipc_output_x,
	.y = ipc_output_y,
	.width = ipc_output_width,
	.height = ipc_output_height,
};

void reg_glob(void *data, struct wl_registry *reg, uint32_t name,
			  const char *intf, uint32_t v) {
	if (!strcmp(intf, wl_compositor_interface.name)) {
		comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(intf, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(intf, zwlr_layer_shell_v1_interface.name)) {
		layer_shell =
			wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(intf, zdwl_ipc_manager_v2_interface.name)) {
		ipc_manager =
			wl_registry_bind(reg, name, &zdwl_ipc_manager_v2_interface, 2);
		zdwl_ipc_manager_v2_add_listener(ipc_manager, &ipc_manager_listener,
										 NULL);
	} else if (!strcmp(intf, wl_output_interface.name)) {
		if (!wl_output) {
			wl_output = wl_registry_bind(reg, name, &wl_output_interface, 1);
		}
	};
}
void reg_glob_rem(void *data, struct wl_registry *reg, uint32_t name) {}
struct wl_registry_listener reg_list = {.global = reg_glob,
										.global_remove = reg_glob_rem};

int main(int argc, char *argv[]) {
	int opt;
	while ((opt = getopt(argc, argv, "o:O:I:i:A:a:b:l:t:")) != -1) {
		switch (opt) {
		case 'b':
			sscanf(optarg, "%x", &color_border);
			break;
		case 'I':
			sscanf(optarg, "%x", &color_bg_inactive);
			break;
		case 'i':
			sscanf(optarg, "%x", &color_fg_inactive);
			break;
		case 'o':
			sscanf(optarg, "%x", &color_fg_occupied);
			break;
		case 'O':
			sscanf(optarg, "%x", &color_bg_occupied);
			break;
		case 'A':
			sscanf(optarg, "%x", &color_bg_active);
			break;
		case 'a':
			sscanf(optarg, "%x", &color_fg_active);
			break;
		case 't':
			sscanf(optarg, "%u", &timeout);
			break;
		case 'l':
			if (strlen(optarg) >= 9) {
				custom_tags = strdup(optarg);
			} else {
				fprintf(stderr, "自定义标签必须至少包含9个字符\n");
				usage(argv[0]);
			}
			break;
		default:
			usage(argv[0]);
		}
	}

	// 初始化 FreeType
	if (FT_Init_FreeType(&ft_library)) {
		fprintf(stderr, "无法初始化 FreeType\n");
		return -1;
	}

	font_path = get_system_font();

	if (FT_New_Face(ft_library, font_path, 0, &ft_face)) {
		fprintf(stderr, "无法加载字体: %s\n", font_path);
		return -1;
	}

	FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)font_size);

	disp = wl_display_connect(NULL);
	struct wl_registry *reg = wl_display_get_registry(disp);
	wl_registry_add_listener(reg, &reg_list, 0);
	wl_display_roundtrip(disp);

	// 初始创建surface
	create_surface();
	clock_gettime(CLOCK_MONOTONIC, &last_frame);
	struct wl_callback *cb = wl_surface_frame(srfc);
	wl_callback_add_listener(cb, &cb_list, 0);
	wl_surface_commit(srfc);

	if (ipc_manager && wl_output) {
		ipc_output = zdwl_ipc_manager_v2_get_output(ipc_manager, wl_output);
		zdwl_ipc_output_v2_add_listener(ipc_output, &ipc_output_listener, disp);
	}

	while (wl_display_dispatch(disp)) {
		if (!srfc && !layer_surface) {
			// 等待事件重新创建surface
		}
	}

	destroy_surface();
	if (layer_shell)
		zwlr_layer_shell_v1_destroy(layer_shell);
	if (ipc_output)
		zdwl_ipc_output_v2_destroy(ipc_output);
	if (ipc_manager)
		zdwl_ipc_manager_v2_destroy(ipc_manager);
	if (wl_output)
		wl_output_destroy(wl_output);
	wl_display_disconnect(disp);
	if (custom_tags)
		free(custom_tags);
	FT_Done_Face(ft_face);
	FT_Done_FreeType(ft_library);
	return 0;
}