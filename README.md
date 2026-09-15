you just need weechat-plugin.h from dev version of weechat and this Makefile: 
CC      = gcc
CFLAGS  = -Wall -Wextra -fPIC -O2 -I.
LDFLAGS = -shared

all: colorZ.so

colorZ.so: colorZ.c weechat-plugin.h
        $(CC) $(CFLAGS) -c colorZ.c -o colorZ.o
        $(CC) $(LDFLAGS) colorZ.o -o colorZ.so

install: colorZ.so
        mkdir -p ~/.local/share/weechat/plugins
        cp colorZ.so ~/.local/share/weechat/plugins/

clean:
        rm -f colorZ.o colorZ.so

.PHONY: all install clean
