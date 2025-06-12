

show dwl tag hit when switch tag(base on dwl ipc)

if maybe useful if you don't want any bar.



https://github.com/user-attachments/assets/043fa47d-5926-463c-87cd-7f85aa75744c



# dependency

- wayland
- wayland-protocols
- wayland-scanner
- fontconfig
- freetype2

# build

```shell
sudo make install
```

# usage

```shell
dwl-tag-overlay [-b border_color] [-I inactive_bg] [-i inactive_fg]
                [-A active_bg] [-a active_fg] [-O occupied_bg] [-o occupied_fg]
                [-l "label_list"] [-t timeout]
```

# example

```shell
dwl-tag-overlay -b ffDDCA9E -A ffDDCA9E -a ff000000 -I ff201B14 -i ff835529  -o ffDDCA9E -O ff201B14  -l "123456789" -t 500
```
