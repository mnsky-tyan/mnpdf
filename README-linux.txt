mnpdf for Linux

Run ./mnpdf from this directory. Everything it needs is here: the binary, the
PDFium shared library it links against, and the licences. No installation, no
root, and no system packages beyond GTK3 and libcurl, which every desktop
already has.

ADD IT TO THE MENU (recommended)
A reader you cannot find in the application menu is a reader you will not use.
Run once, from this directory:

    ./install-menu-entry.sh

It writes a desktop entry pointing at this copy (wherever you unpacked it) plus
the icon, into ~/.local/share - no root. After that, searching the menu for
"mnpdf" finds it, right-clicking a PDF offers it under Open With, and pinning it
to the taskbar works: the pinned icon re-launches the reader instead of going
blank. `./install-menu-entry.sh --remove` undoes it. If you move this directory,
run the installer again so the entry points at the new place.

WHAT IT DOES
A small native PDF reader: continuous scrolling, text selection and copy,
search with Ctrl+F and F3, zoom with Ctrl+wheel or +/- and Ctrl+0 to fit the
width, highlights in six preset colours plus your own, pins with notes, page
rotation, undo/redo, and printing. Thumbnails and an outline panel open as side
drawers, the titlebar can be hidden, and there is a night mode. All of it works
offline.

SCREEN SCALE
The reader takes its scale from the desktop and paints at that density. If it
guesses wrong on yours, name the factor yourself - 1 is unscaled, 2 doubles
everything, and 1 to 4 is accepted:

    MNPDF_SCALE=1.6 ./mnpdf

SHARED STATE WITH WINDOWS
It reads and writes the same per-document sidecar files and the same app.txt as
the Windows build, so marks, pins, rotations and notes made in either build open
in the other. On Linux those live in ${XDG_CONFIG_HOME:-~/.config}/mnpdf; point
XDG_CONFIG_HOME at a directory shared with the Windows machine to line the two
readers up on the same documents. The single-writer guard is per platform (a
flock on Linux, a named mutex on Windows), so keep one platform's reader closed
while the other's is open: two Linux readers or two Windows readers protect the
file from each other, one of each does not.

DIFFERENCES FROM THE WINDOWS BUILD
Printing uses the GTK print dialog instead of the Windows one. The note editor
is a modal dialog rather than a popup beside the pin. A custom colour is typed
into a dialog rather than into the little popup box that opens beside the
reader. The update check tells you when a newer release exists; it does not
download or install it, and it has no "open the release page" button yet - the
Windows build has one.
