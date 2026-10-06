mnpdf v2.3.0 - Windows x64 portable release

Run mnpdf.exe. Keep pdfium.dll in the same folder.

WHAT IT IS
A small native Windows PDF reader: no installer, no runtime to install, no
framework. The folder above is the whole program, and it can be run from a
USB stick or moved anywhere without setup.

READ AND MARK
Pages scroll continuously in one column. Select text with the mouse and copy
it with Ctrl+C. Search with Ctrl+F, jump between matches with F3 and
Shift+F3. Zoom with Ctrl+wheel or the + and - keys, fit the page width with
Ctrl+0, and drag the scroll bars to move in both directions.

Highlight selected text (right click, Highlight, then a colour) and drop
pins with a note attached (right click, Add pin here). Six preset colours
are shared by highlights and pins, plus up to three of your own #rrggbb
values - the box opens with the # already in it, a wrong entry is explained
at once, and Clear custom colours hands the slots back. Undo anything with
Ctrl+Z and Ctrl+Y.

Annotations never stack on top of each other and never block the page: a
pin's editor stays inside the window, and clicking anywhere else closes it.

ADVANCED
Advanced, in the right-click menu, holds everything past plain reading.

The two drawers live there. Thumbnails shows a strip of page previews:
click one to jump there, Ctrl+Up and Ctrl+Down to move the selected page,
Delete to remove it, R to turn it. Outline panel lists the document's own
bookmarks, indented, and a click goes to the page it names.

Night mode inverts the page for a dark screen and comes back that way after
a restart.

Merge PDFs adds other files to the end of the document you are reading.
Split pages exports a range - 2-5 or 1,3,8 - into a new PDF and leaves the
original alone.

Insert signature takes a JPEG and asks where to put it: the next click on a
page drops the stamp there. Clear signatures takes them all away again. A
stamp reaches the PDF when you save, and sits in the sidecar until then,
exactly like a highlight. A signature has to be a JPEG: the picker refuses
anything else rather than handing it to the image decoder.

Reopen last document goes back to the document the current one replaced, so
opening the wrong file by mistake costs one click to undo. It spans this
session only.

Below those, the same menu lists what is still to come - OCR, Word export,
editing text in place, form creation, encryption, tabs - greyed on purpose:
they need a different kind of program, and this one stays a reader you can
copy onto a USB stick.

SAVING
With autosave on (the default) every change is written to a sidecar file
under %APPDATA%\mnpdf named doc-<hash>.txt, and the PDF on disk is left
untouched. Ctrl+S writes the highlights, pins, signature stamps and page
rotations into the PDF itself with Save As under Ctrl+Shift+S. Settings,
palette, night mode, window position and the last document you had open are
remembered in the same folder, so the next start resumes exactly where you
left off.

HIDING THE TITLEBAR
Hide titlebar removes the caption entirely. The window still moves when you
drag its top edge, and resizes from the left, right and bottom edges and
from the corners, with a band exactly one window frame thick so edge clicks
still reach the page. It does not grow a plain Windows caption back when
another window takes focus, and its size, position and maximised state come
back after a restart - including after quitting while minimised.

PRINTING
Ctrl+P (or Print in the menu) opens the standard Windows print dialog on
the document and prints clean pages: annotations, highlights and pins stay
in the file and on screen, they are simply not printed.

UPDATES
Check for updates asks GitHub for the newest release at most once an hour. When it finds one, the check button becomes an update button: clicking it downloads the official ZIP, replaces mnpdf.exe and pdfium.dll in this folder and restarts the reader, which opens your last document again. A document with unsaved changes is not touched until you save it, and if anything fails before the swap - no network, a blocked file - nothing is changed and the update page stays one click away. The swap is the point where the change stands: once mnpdf.exe and pdfium.dll are both replaced the update has happened, and if the new reader then refuses to start the previous copies are still in the folder beside them as mnpdf.exe.old and pdfium.dll.old - rename them back. The old copies are removed by the restarted reader. To update by hand instead, extract the new ZIP over this folder; the ZIP always contains a README.txt like this one.

