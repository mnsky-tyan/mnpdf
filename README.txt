mnpdf v2.1.3 - Windows x64 portable release

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

PAGES
The right-click menu opens two drawers beside the reader. Thumbnails shows
a strip of page previews: click one to jump there, Ctrl+Up and Ctrl+Down to
move the selected page, Delete to remove it, R to turn it. Outline lists the
document's own bookmarks, indented, and a click goes to the page it names.

Merge PDFs adds other files to the end of the document you are reading.
Split pages exports a range - 2-5 or 1,3,8 - into a new PDF and leaves the
original alone.

Insert signature takes a JPEG and asks where to put it: the next click on a
page drops the stamp there. Clear signatures takes them all away again. A
stamp reaches the PDF when you save, and sits in the sidecar until then,
exactly like a highlight. A signature has to be a JPEG: the picker refuses
anything else rather than handing it to the image decoder.

Night mode inverts the page for a dark screen and comes back that way after
a restart.

Advanced, at the bottom of the right-click menu, lists the heavy options -
OCR, Word export, editing text in place, form creation, encryption, tabs.
They are greyed on purpose: they need a different kind of program, and this
one stays a reader you can copy onto a USB stick.

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
This release explains portable updates in the app and never replaces files automatically. Check for updates asks GitHub for the newest release at most once an hour and only tells you when a newer version exists; it downloads nothing and changes nothing by itself. To move to a newer version, extract the new ZIP over this folder. The file you download always contains a README.txt like this one.

