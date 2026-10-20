// Platform seam: what the portable core (doc.cpp/search.cpp/sidecar.cpp) is
// allowed to ask the platform layer for. The Windows original inlines these as
// GDI calls, straight into the "pure" functions; here they are one interface so
// core.cpp never names a window, a DC or a widget.
#pragma once

#include <string>

double ui_scale();                       // device pixels per design pixel (GTK scale factor)

// saved window geometry: sidecar.cpp owns it (app.txt); the window layer consumes
// it at startup and updates it on configure/state changes
extern int gWinX, gWinY, gWinW, gWinH;
extern bool gWinMax;
// request a redraw of the current view
void ui_invalidate();

// the drawers (thumbnails/outline) follow any change to the page list
void refreshDrawers();
void ui_set_title(const std::string& t); // window/tab title
void ui_toast(const std::string& msg);   // transient user-visible status line

// modal-ish message box (used by save failures and page-surgery refusals)
int ui_message(const std::string& text, const std::string& caption, bool yesno);
enum { UI_MSGBOX_NO = 0, UI_MSGBOX_YES = 1 };

// file pickers; return "" when cancelled
std::string ui_open_file();
std::string ui_save_file(const std::string& suggested);
std::string ui_pick_jpeg();

// a text prompt with a default value; nullopt/cancel returns false
bool ui_prompt(const std::string& caption, const std::string& label, std::string& value);
bool ui_prompt_range(std::string& range);             // "Split pages" range prompt
bool ui_color_box(int& paletteIndex);                  // custom #rrggbb entry

// pin note editing: the editor runs over the view; it reports back through
// prompt_note_finished when done (empty text = the pin was cancelled and is
// dropped by the caller)
void ui_note_editor(int pinIndex, int cx, int cy, const std::wstring& initial);

// clip text as UTF-8
void ui_copy_text(const std::string& utf8);
