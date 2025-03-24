CFLAGS = -Wall -Wextra -pedantic -O2 `pkg-config --cflags sdl2 poppler-glib cairo`
LIBS = `pkg-config --libs sdl2 poppler-glib cairo` -lm

all: beamview

beamview: beamview.c
	$(CC) $(CFLAGS) -o beamview beamview.c $(LIBS)

clean:
	rm -f beamview
