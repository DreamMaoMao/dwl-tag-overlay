PKG_CONFIG ?= pkg-config
SCANNER := wayland-scanner

PREFIX=/usr
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

# 强制启用 FreeType 支持
CFLAGS += -g -Wall -Werror -Wextra -Wpedantic -Wno-unused-parameter \
          -Wconversion -Wformat-security -Wformat -Wsign-conversion \
          -Wfloat-conversion -Wunused-result \
          -DFORCE_FREETYPE \
          $(shell $(PKG_CONFIG) --cflags pixman-1 freetype2 fontconfig)

# 添加 wlroots 和 fontconfig 到链接库
LIBS += -lwayland-client $(shell $(PKG_CONFIG) --libs pixman-1 freetype2 fontconfig) -lrt -lm

OBJ=dwl-ipc-unstable-v2.o dwl-tag-overlay.o wlr-layer-shell-unstable-v1.o xdg-shell.o
GEN=dwl-ipc-unstable-v2.c dwl-ipc-unstable-v2.h wlr-layer-shell-unstable-v1.c wlr-layer-shell-unstable-v1.h xdg-shell.c xdg-shell.h

dwl-tag-overlay: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install: dwl-tag-overlay
	install        -D dwl-tag-overlay   $(DESTDIR)$(BINDIR)/dwl-tag-overlay

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/dwl-tag-overlay

clean:
	$(RM) dwl-tag-overlay $(GEN) $(OBJ)

.PHONY: clean install

