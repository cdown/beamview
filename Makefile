CFLAGS = -Wall -Wextra -pedantic -O2 `pkg-config --cflags glfw3 mupdf`
LIBS = `pkg-config --libs glfw3 mupdf` -lGL -lm

all: beamview

beamview: beamview.c
	$(CC) $(CFLAGS) -o beamview beamview.c $(LIBS)

clean:
	rm -f beamview
