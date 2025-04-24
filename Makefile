COMMON_CFLAGS = -Wall -Wextra -pedantic `pkg-config --cflags sdl2 poppler-glib cairo` -I/usr/X11R6/include -L/usr/X11R6/lib
LIBS = `pkg-config --libs sdl2 poppler-glib cairo` -lm -lX11

CFLAGS_RELEASE = -O2 $(COMMON_CFLAGS)
CFLAGS_DEBUG = -Og -ggdb -fno-omit-frame-pointer $(COMMON_CFLAGS)
CFLAGS_SANITISERS = $(CFLAGS_DEBUG) -fsanitize=address -fsanitize=undefined

all: release

release: CFLAGS = $(CFLAGS_RELEASE)
release: beamview

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: beamview

sanitisers: CFLAGS = $(CFLAGS_SANITISERS)
sanitisers: beamview

clang-tidy:
	clang-tidy beamview.c \
	  -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
	  -- $(COMMON_CFLAGS)

beamview: beamview.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f beamview

prefix ?= /usr/local
bindir = $(prefix)/bin
mandir = $(prefix)/share/man/man1
INSTALL = install

install: release
	mkdir -p $(DESTDIR)$(bindir)
	$(INSTALL) -m 755 beamview   $(DESTDIR)$(bindir)/beamview
	mkdir -p $(DESTDIR)$(mandir)
	$(INSTALL) -m 644 beamview.1 $(DESTDIR)$(mandir)/beamview.1

.PHONY: all release debug sanitisers clang-tidy install clean
