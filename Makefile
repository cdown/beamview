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

beamview: beamview.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f beamview

.PHONY: all release debug sanitisers clean
