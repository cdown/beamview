CFLAGS = -Wall -Wextra -pedantic -O2 `pkg-config --cflags sdl2 poppler-glib cairo` -I/usr/X11R6/include -L/usr/X11R6/lib
LIBS = `pkg-config --libs sdl2 poppler-glib cairo` -lm -lX11

all: beamview

beamview: beamview.c
	$(CC) $(CFLAGS) -o beamview beamview.c $(LIBS)

clean:
	rm -f beamview
