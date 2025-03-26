CFLAGS = -Wall -Wextra -pedantic -O2 `pkg-config --cflags glfw3 poppler-glib cairo`
LIBS = `pkg-config --libs glfw3 poppler-glib cairo` -lGL -lm

all: beamview

beamview: beamview.c
	$(CC) $(CFLAGS) -o beamview beamview.c $(LIBS)

clean:
	rm -f beamview
