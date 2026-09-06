CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
PREFIX ?= /usr/local
UDEVDIR ?= /usr/lib/udev/rules.d
CORE = sixpair_core.c mac.c store.c btsony.c
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 xapp)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 xapp)

all: sixaxis-ctrl sixaxis-tui sixaxis-gtk

sixaxis-ctrl: sixaxis-ctrl.c $(CORE) sixpair_core.h mac.h store.h btsony.h version.h
	$(CC) $(CFLAGS) -o $@ sixaxis-ctrl.c $(CORE) -lusb

sixaxis-tui: sixaxis-tui.c $(CORE) sixpair_core.h mac.h store.h btsony.h version.h
	$(CC) $(CFLAGS) -o $@ sixaxis-tui.c $(CORE) -lusb -lncursesw

sixaxis-gtk: sixaxis-gtk.c $(CORE) sixpair_core.h mac.h store.h btsony.h version.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ sixaxis-gtk.c $(CORE) -lusb $(GTK_LIBS)

clean:
	rm -f sixaxis-ctrl sixaxis-tui sixaxis-gtk

install: all
	install -Dm755 sixaxis-ctrl $(DESTDIR)$(PREFIX)/bin/sixaxis-ctrl
	install -Dm755 sixaxis-tui $(DESTDIR)$(PREFIX)/bin/sixaxis-tui
	install -Dm755 sixaxis-gtk $(DESTDIR)$(PREFIX)/bin/sixaxis-gtk
	install -Dm644 99-sixaxis.rules $(DESTDIR)$(UDEVDIR)/99-sixaxis.rules

.PHONY: all clean install
