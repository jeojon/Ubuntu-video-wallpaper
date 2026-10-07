CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11

# vidwall: 壁纸本体, 只要 X11
WALL_LIBS = -lX11 -lXext -lXrandr

# vidgui: 图形界面, 只要 GTK3 (系统自带 libgtk-3-dev)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

PREFIX   ?= /usr/local
BINS      = vidwall vidgui

all: $(BINS)

vidwall: src/vidwall.o
	$(CC) $(CFLAGS) -o $@ $< $(WALL_LIBS)

vidgui: src/vidgui.o
	$(CC) $(CFLAGS) -o $@ $< $(GTK_LIBS)

src/vidwall.o: src/vidwall.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/vidgui.o: src/vidgui.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c -o $@ $<

install: $(BINS)
	install -Dm755 vidwall $(DESTDIR)$(PREFIX)/bin/vidwall
	install -Dm755 vidgui  $(DESTDIR)$(PREFIX)/bin/vidgui

clean:
	rm -f $(BINS) src/*.o

.PHONY: all install clean
