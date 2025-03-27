# beamview

A dual-screen PDF viewer for presenting Beamer presentations with notes.

## Features

- Dual-screen mode: allows viewing presentation notes while presenting slides
- OpenGL/GLFW-based rendering for high performance
- Slide pre-rendering and caching for instantaneous navigation
- Simple keyboard navigation
- Minimal resource usage
- Clean, simple codebase (~400 lines)

## Usage

beamview has a simple interface focused on presenting:

    beamview presentation.pdf

This will open two windows, each displaying half of the PDF. Navigate using:

| Key                             | Action          |
|---------------------------------|-----------------|
| Left Arrow, Up Arrow, Page Up   | Previous slide  |
| Right Arrow, Down Arrow, Page Down | Next slide  |
| Shift+Q                         | Quit           |

The windows will automatically scale content to fit, and you can resize them as needed.

## Compilation

Run `make` to compile. You will need the following dependencies:

- GLFW3
- OpenGL
- Cairo
- Poppler
- GLib
