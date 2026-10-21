// Platform seam: what the portable core (doc.cpp, search.cpp, sidecar.cpp,
// opendoc.cpp, surgery.cpp) is allowed to ask the platform layer for. The
// Windows original inlines these as GDI calls, straight into the "pure"
// functions; here they are one interface so the core never names a window, a DC
// or a widget. Not every entry is a core call, and each is named below: the
// core reaches ui_message, ui_save_file, ui_pick_jpeg, ui_copy_text and
// ui_windowRect; ui_scale, ui_invalidate, ui_set_title, ui_open_file,
// ui_prompt, ui_prompt_range, ui_color_box and refreshDrawers serve the GTK
// layer itself. The update check is not a seam: its blocking half lives in
// update.cpp beside the version rules, and it is called from the GTK layer on a
// worker thread.
#pragma once

#include <string>

double ui_scale();                       // device pixels per design pixel (GTK scale factor)

// saved window geometry: sidecar.cpp owns it (app.txt); the window layer consumes
// it at startup and updates it on configure/state changes
extern int gWinX, gWinY, gWinW, gWinH;
extern bool gWinMax;
// request a redraw of the current view
void ui_invalidate();

// the drawers (thumbnails/outline) follow any change to the page list: call
// this after anything that changes the pages - open, tab switch, delete, move,
// rotate, undo/redo, a merge or a split
void refreshDrawers();
void ui_set_title(const std::string& t); // window/tab title

// modal-ish message box (the quit prompt asks Yes/No/Cancel as Windows does;
// save failures and page-surgery refusals use the info form)
int ui_message(const std::string& text, const std::string& caption, bool yesno);
enum { UI_MSGBOX_NO = 0, UI_MSGBOX_YES = 1, UI_MSGBOX_CANCEL = 2 };

// file pickers; return "" when cancelled
std::string ui_open_file();
std::string ui_save_file(const std::string& suggested);
std::string ui_pick_jpeg();

// a text prompt with a default value; cancel leaves value untouched and returns false
bool ui_prompt(const std::string& label, std::string& value);
bool ui_prompt_range(std::string& range);             // "Split pages" range prompt
bool ui_color_box(int& paletteIndex);                  // custom #rrggbb entry

// clip text as UTF-8
void ui_copy_text(const std::string& utf8);
