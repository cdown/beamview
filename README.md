# beamview

A dual-screen PDF viewer for presenting Beamer presentations with notes.

## Features

- Dual-screen mode: allows viewing presentation notes while presenting slides
- Poppler/Cairo/SDL-based rendering for high performance
- Slide pre-rendering and caching for instantaneous navigation
- Simple keyboard navigation
- Minimal resource usage
- Clean, simple codebase (~500 lines)

## Usage

First, compile your beamer document to put notes at the side:

    \setbeameroption{show notes on second screen}

Then call beamview on the resulting PDF:

    beamview presentation.pdf

This will open two windows, one displaying the notes, and one displaying the
presentation. Navigate using:

| Key                             | Action          |
|---------------------------------|-----------------|
| Left Arrow, Up Arrow, Page Up   | Previous slide  |
| Right Arrow, Down Arrow, Page Down | Next slide  |
| Shift+Q                         | Quit           |
| Shift+F                         | Fullscreen     |

The windows will automatically scale content to fit, and you can resize them as
needed.

## Compilation

Run `make` to compile. You will need the following dependencies:

- poppler-glib
- SDL2
- Cairo

## Comparison with other tools

I used to use [pdfpc](https://github.com/pdfpc/pdfpc), and then
[dspdfviewer](https://github.com/dannyedel/dspdfviewer). The pdfpc toolbase is
too complex for my tastes and would basically be the only reason I need a Vala
compiler. dspdfviewer is, unfortunately, unmaintained for over two years and
somewhat crash-prone in my experience, which is obviously not good if you are
giving a presentation.
