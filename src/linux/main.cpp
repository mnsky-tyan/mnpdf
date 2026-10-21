// mnpdf for Linux - the GTK3 platform layer: window, tabs, event handling,
// dialogs and the ui_* seam the portable core calls. The document model,
// geometry, marks and search live in doc.cpp/render.cpp/search.cpp/sidecar.cpp/
// opendoc.cpp/surgery.cpp, ported from the Windows src/main.cpp.
//
// The Windows app scrolls the document itself (gScrollX/gScrollY) and paints a
// back buffer; this does the same, so the geometry, hit-testing and search code
// ported over unchanged. GtkNotebook supplies the tab strip (the Windows app
// draws its own), and cairo replaces GDI for the overlays.
#include "doc.h"
#include "ui.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <cairo.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- externs from the core modules ----
extern void writeSidecarNow();
extern void writeAllSidecars();
extern void writeLastDocument(const std::wstring&);
extern std::wstring lastDocumentPath();
extern void restoreSidecar(const std::wstring&);
extern void loadAppPref();
extern void writeAppPref();
extern void runSearch();
extern void nextMatch(int dir);
extern int searchCount();
extern void ui_matchReset();
extern void ui_paint(cairo_t*);
extern void copyTextRange(int p0, int i0, int p1, int i1);
extern void deletePage(int idx);
extern void movePage(int from, int to);
extern void mergeFiles(const std::vector<std::wstring>&);
extern void splitExport(const std::wstring& target, const std::string& range, bool quiet);
extern void pickSignatureImage();
extern void clearSignatures();
extern void commitSigPlacement(int page, double sx, double sy);
extern void refreshDrawers();
extern std::wstring gSigPlacing;
extern bool looksLikeJpeg(const std::wstring&);
extern bool sidecarPending();
extern void commitPinText(int idx, bool fresh, const std::wstring& text);
extern bool renderPageToBuffer(int page, int w, int h, std::vector<unsigned char>& out, int* stride);
extern const unsigned char* ui_backbuf_bits();
extern int ui_backbuf_w();
extern int ui_backbuf_h();
// the update check (update.cpp): the blocking half, run on a worker thread
extern const char* gAppVersion;
bool updateCheckBlocking(std::string& latestTag, bool& newer, std::string& error);
bool updateCheckNow(std::string& latestTag, bool& newer, std::string& error);

// ---- platform state ----
static GtkWidget* gWin = nullptr;
static GtkWidget* gNotebook = nullptr;
static GtkWidget* gCurArea = nullptr;
static GtkWidget* gSearchBar = nullptr;
static GtkWidget* gSearchEntry = nullptr;
static GtkWidget* gSearchLabel = nullptr;
static double gScale = 1.0;
static guint gSaveTimer = 0;
static bool gDragging = false;         // a selection drag is live
static bool gSbDragging = false;       // a scrollbar thumb drag is live
static int gSbWhich = 0;               // 1 = vertical, 2 = horizontal
static int gSbGrab = 0;                // pointer offset inside the thumb
static GdkCursor* gIbeam = nullptr;
static GdkCursor* gArrow = nullptr;

double ui_scale() { return gScale; }
void ui_invalidate() { if (gCurArea) gtk_widget_queue_draw(gCurArea); }

static std::wstring basenameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"/\\");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// the tab labels live in the notebook, so refresh them there
static void refreshTabLabels() {
    for (int i = 0; i < gDocCount; i++) {
        GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(gNotebook), i);
        if (!page) continue;
        std::wstring nm = gDocs[i]->path.empty() ? L"New tab" : basenameOf(gDocs[i]->path);
        std::string label = wideToUtf8(nm);
        if (gDocs[i]->dirty) label = "\xE2\x97\x8F " + label;   // a filled dot marks unsaved work
        gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(gNotebook), page, label.c_str());
    }
}

void ui_set_title(const std::string&) {
    std::string base = "mnpdf";
    if (!gPath.empty()) {
        base = wideToUtf8(basenameOf(gPath)) + " - mnpdf";
        char pg[48];
        snprintf(pg, sizeof(pg), " - page %d/%d", gPageIndex + 1, gDocPages);
        base += pg;
        if (gDirty) base = "*" + base;
    }
    gtk_window_set_title(GTK_WINDOW(gWin), base.c_str());
    refreshTabLabels();
}

// ---- message box ----
int ui_message(const std::string& text, const std::string& caption, bool yesno) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(gWin),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        yesno ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO,
        GTK_BUTTONS_NONE, "%s", text.c_str());
    if (yesno) {
        // the three Windows MB_YESNOCANCEL answers: No still writes the sidecar,
        // Cancel keeps the window open and decides later
        gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL,
                               "_No", GTK_RESPONSE_NO, "_Yes", GTK_RESPONSE_YES, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_YES);
    } else {
        gtk_dialog_add_button(GTK_DIALOG(d), "_OK", GTK_RESPONSE_OK);
    }
    gtk_window_set_title(GTK_WINDOW(d), caption.c_str());
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_YES) return UI_MSGBOX_YES;
    // Escape and the window-manager close are Cancel, not No: dismissing a
    // "Save changes?" must not quit, exactly as IDCANCEL on Windows
    if (r == GTK_RESPONSE_CANCEL || r == GTK_RESPONSE_DELETE_EVENT || r == GTK_RESPONSE_NONE)
        return UI_MSGBOX_CANCEL;
    return UI_MSGBOX_NO;
}

// ---- file pickers ----
static std::string runChooser(const char* title, GtkFileChooserAction act, bool save, const std::string& suggested) {
    GtkWidget* d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(gWin), act,
        "_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (act == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
        if (!suggested.empty())
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), suggested.c_str());
    }
    std::string out;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (fn) { out = fn; g_free(fn); }
    }
    gtk_widget_destroy(d);
    return out;
}

std::string ui_open_file() { return runChooser("Open PDF", GTK_FILE_CHOOSER_ACTION_OPEN, false, ""); }
std::string ui_save_file(const std::string& suggested) {
    std::string name = suggested;
    size_t s = name.find_last_of('/');
    if (s != std::string::npos) name = name.substr(s + 1);
    return runChooser("Save as", GTK_FILE_CHOOSER_ACTION_SAVE, true, name);
}
std::string ui_pick_jpeg() {
    GtkWidget* d = gtk_file_chooser_dialog_new("Choose a signature image", GTK_WINDOW(gWin),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "JPEG image");
    gtk_file_filter_add_pattern(f, "*.jpg");
    gtk_file_filter_add_pattern(f, "*.jpeg");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    std::string out;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (fn) { out = fn; g_free(fn); }
    }
    gtk_widget_destroy(d);
    return out;
}

bool ui_prompt(const std::string& label, std::string& value) {
    GtkWidget* d = gtk_dialog_new_with_buttons("mnpdf", GTK_WINDOW(gWin), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, nullptr);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label.c_str()), FALSE, FALSE, 6);
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), value.c_str());
    gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 6);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    if (ok) value = gtk_entry_get_text(GTK_ENTRY(e));
    gtk_widget_destroy(d);
    return ok;
}

bool ui_prompt_range(std::string& range) {
    std::string v = range.empty() ? "1" : range;
    bool ok = ui_prompt("Pages to export (e.g. 2-5 or 1,3,8):", v);
    if (ok) range = v;
    return ok;
}

// ---- clipboard ----
void ui_copy_text(const std::string& utf8) {
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, utf8.c_str(), -1);
    gtk_clipboard_store(cb);
}

// ---- custom colour entry ----
bool ui_color_box(int& paletteIndex) {
    std::string v = "#";
    if (!ui_prompt("Custom colour (#rrggbb):", v)) return false;
    unsigned r, g, b;
    if (v.size() == 7 && v[0] == '#' && sscanf(v.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
        int slot = gPalNext;
        gPalCustom[slot] = RGB(r, g, b);
        gPalNext = (slot + 1) % kPalCustom;
        paletteIndex = kPalPreset + slot;
        writeAppPref();
        return true;
    }
    ui_message("That is not a colour. Use #rrggbb, for example #ff8800.", "mnpdf", false);
    return false;
}

void ui_windowRect(int* x, int* y, int* w, int* h, int* maxed) {
    *x = gWinX; *y = gWinY; *w = gWinW; *h = gWinH;
    *maxed = gWinMax ? 1 : 0;
}

// ---- the colour submenu ----------------------------------------------------
// The menu handler lives further down, so it is declared here: the submenu
// builder connects the same handler the top-level menus use.
static void onMenuActivate(GtkMenuItem* mi, gpointer data);
// Windows offers the same menu wherever a colour is chosen: the six presets,
// the reader's own custom slots, a way to hand the slots back, and a way to
// type a new one (src/main.cpp:3952 colorSubmenu). The ids differ only in
// their base, so one builder serves all four places: the preset entries start
// at presetBase, the custom slots at customBase, and customId opens the
// #rrgggb box for that kind of mark.
                                              // 0 = a highlight, 1 = a pin,
                                              // 2 = the default highlight,
                                              // 3 = the default pin

static GtkWidget* buildColorSubmenu(int current, int presetBase, int customBase, int customId) {
    GtkWidget* sub = gtk_menu_new();
    auto entry = [&](const char* label, int id) {
        GtkWidget* it = gtk_menu_item_new_with_label(label);
        g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
        // recorded so the menu dump can see the colour ids too, which are the
        // families the parity claim is most easily wrong about
        g_object_set_data(G_OBJECT(it), "cmd-id", GINT_TO_POINTER(id));
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
    };
    for (int c = 0; c < kPalPreset; c++) {
        // the current colour carries a tick, like the Windows MF_CHECKED entry
        char lab[64];
        snprintf(lab, sizeof(lab), "%s%s", gPalName[c], current == c ? " ✓" : "");
        entry(lab, presetBase + c);
    }
    bool any = false;
    for (int c = 0; c < kPalCustom; c++) {
        if (gPalCustom[c] == CLR_INVALID) continue;
        std::wstring wl = palLabel(kPalPreset + c);
        std::string sl = wideToUtf8(wl);
        char lab[80];
        snprintf(lab, sizeof(lab), "%s%s", sl.c_str(), current == kPalPreset + c ? " ✓" : "");
        entry(lab, customBase + c);
        any = true;
    }
    if (any) {
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
        entry("Clear custom colours", 180);
    }
    entry("Custom...", customId);
    return sub;
}

// the id of a colour entry: presets at base, custom slots at base + kPalPreset
static int menuColorIndex(int id, int presetBase, int customBase) {
    if (id >= presetBase && id < presetBase + kPalPreset) return id - presetBase;
    if (id >= customBase && id < customBase + kPalCustom) return kPalPreset + (id - customBase);
    return -1;
}

// ---- the update check ------------------------------------------------------
// The Windows build offers the update in a dialog with a Download button
// (src/main.cpp:3020). The Linux check asks the releases API for the newest
// release that carries a Linux archive and compares it with this build's
// version; the report is a dialog of the same shape, so the reader sees the
// same thing on both platforms.
//
// The request runs on its own thread, like the Windows one: a network that
// takes its time must not freeze the window. GTK is not thread-safe, so the
// result is handed back to the main loop with g_idle_add.
struct UpdateOutcome {
    bool ok = false;
    std::string latestTag;
    bool newer = false;
    std::string error;
};

static gboolean showUpdateResult(gpointer data) {
    UpdateOutcome* o = (UpdateOutcome*)data;
    std::string current = gAppVersion;
    if (!o->ok) {
        ui_message(o->error.empty() ? "The update check failed." : o->error, "mnpdf", false);
    } else if (o->newer) {
        ui_message("mnpdf " + o->latestTag + " is available.\n\nYou are running " + current + ".",
                   "mnpdf update available", false);
    } else {
        ui_message("You are running the newest version (" + current + ").", "mnpdf update", false);
    }
    delete o;
    return G_SOURCE_REMOVE;
}

static void runUpdateCheck() {
    std::thread([]() {
        UpdateOutcome* o = new UpdateOutcome();
        std::string latest, error;
        bool newer = false;
        if (updateCheckNow(latest, newer, error)) {
            o->ok = true;
            o->latestTag = latest;
            o->newer = newer;
        } else {
            o->ok = false;
            o->error = error;
        }
        g_idle_add(showUpdateResult, o);
    }).detach();
}

// ---- note editor: a dialog with a text view, committed on OK ----
static void openNoteEditor(int idx, bool fresh) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    GtkWidget* d = gtk_dialog_new_with_buttons("Note", GTK_WINDOW(gWin), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(sw, 360, 150);
    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    std::string init = wideToUtf8(gPins[idx].text);
    GtkTextBuffer* tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(tb, init.c_str(), -1);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 6);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    std::wstring text;
    if (ok) {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(tb, &a, &b);
        char* t = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
        if (t) { text = utf8ToWide(t); g_free(t); }
        if ((int)text.size() > kNoteMaxChars) text.resize(kNoteMaxChars);
    }
    gtk_widget_destroy(d);
    if (!ok) { if (fresh) commitPinText(idx, true, L""); return; }   // cancel a new pin: drop it
    commitPinText(idx, fresh, text);
}

// ---- tab lifecycle ----
static void requestQuit();
static void makeArea(GtkWidget** out);
static void onAreaSize(GtkWidget*, GtkAllocation*, gpointer);
static void settleActiveDoc();
static void toggleTitlebar();
static void toggleThumbDrawer();
static void toggleOutlineDrawer();
static void reopenLastDocument();
static void clearCustomColours();

static void addTab(const std::wstring& path) {
    if (gDocCount >= kMaxTabs) return;
    settleActiveDoc();                // the outgoing tab keeps its marks before the switch
    flushPageCache();
    gDocs[gDocCount] = new Doc();
    int idx = gDocCount++;
    GtkWidget* area = nullptr;
    makeArea(&area);
    gtk_notebook_append_page(GTK_NOTEBOOK(gNotebook), area, gtk_label_new("New tab"));
    gtk_widget_show_all(area);
    gActiveDoc = idx;
    gCurArea = area;
    if (!path.empty()) openPath(path);
    else { gClientW = gtk_widget_get_allocated_width(area); gClientH = gtk_widget_get_allocated_height(area); renderPage(); }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(gNotebook), idx);
    refreshTabLabels();
}

static void settleActiveDoc() {
    writeSidecarNow();                 // the outgoing tab keeps its marks
    if (!gSigPlacing.empty()) gSigPlacing.clear();
    gDragging = false;
    if (gSearchOpen) {
        gSearchOpen = false;
        if (gSearchBar) gtk_widget_hide(gSearchBar);
        ui_matchReset();
    }
    clearSelection();
}

static void closeTab() {
    if (gDocCount <= 1) { requestQuit(); return; }   // the last tab quits like closing the window
    settleActiveDoc();
    flushPageCache();
    int cur = gActiveDoc;
    Doc* doomed = gDocs[cur];
    // the last tab's bundle fills the slot; its notebook page moves into place
    gDocs[cur] = gDocs[gDocCount - 1];
    gDocs[gDocCount - 1] = nullptr;
    gDocCount--;
    if (gActiveDoc >= gDocCount) gActiveDoc = gDocCount - 1;
    if (doomed->doc) FPDF_CloseDocument(doomed->doc);
    delete doomed;
    // drop the closed document's page widget, then pull the last page's widget
    // into its slot so the notebook's page order keeps matching gDocs. The
    // moved bundle keeps its own widget: only the Doc pointer moved, and the
    // drawing area carries no per-document state of its own.
    gtk_notebook_remove_page(GTK_NOTEBOOK(gNotebook), cur);
    if (gDocCount > 1) {
        GtkWidget* lastPage = gtk_notebook_get_nth_page(GTK_NOTEBOOK(gNotebook), gDocCount - 1);
        if (lastPage) gtk_notebook_reorder_child(GTK_NOTEBOOK(gNotebook), lastPage, cur);
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(gNotebook), gActiveDoc);
    gCurArea = gtk_notebook_get_nth_page(GTK_NOTEBOOK(gNotebook), gActiveDoc);
    loadPage(gPageIndex);
    relayoutPages();
    clampScroll();
    renderPage();
    refreshTabLabels();
}

// ---- scrollbars ----
static const int kSbThick = 12;
static void sbRects(int* vx, int* vy, int* vw, int* vh, int* hx, int* hy, int* hw, int* hh) {
    int vhgt = gClientH - kSbThick;
    *vx = gClientW - kSbThick; *vy = 0; *vw = kSbThick; *vh = vhgt;
    *hx = 0; *hy = gClientH - kSbThick; *hw = gClientW - kSbThick; *hh = kSbThick;
}
static bool vThumb(int* ty, int* th) {
    int total = docHpx(), view = docViewH();
    if (total <= view || total <= 0) return false;
    int h = gClientH - kSbThick;
    *th = (int)((double)view / total * h);
    if (*th < 24) *th = 24;
    int maxScroll = total - view;
    *ty = (int)((double)gScrollY / maxScroll * (h - *th));
    return true;
}
static bool hThumb(int* tx, int* tw) {
    int total = docWpx() + 2 * MARGIN, view = gClientW;
    if (total <= view || total <= 0) return false;
    int w = gClientW - kSbThick;
    *tw = (int)((double)view / total * w);
    if (*tw < 24) *tw = 24;
    int maxScroll = total - view;
    *tx = (int)((double)gScrollX / maxScroll * (w - *tw));
    return true;
}

// ---- drawing area ----
static void onDraw(GtkWidget* w, cairo_t* cr, gpointer) {
    ui_paint(cr);
    (void)w;
    // scrollbars painted over the frame
    cairo_set_source_rgb(cr, 0.14, 0.14, 0.15);
    cairo_rectangle(cr, gClientW - kSbThick, 0, kSbThick, gClientH);
    cairo_rectangle(cr, 0, gClientH - kSbThick, gClientW, kSbThick);
    cairo_fill(cr);
    int ty, th, tx, tw;
    if (vThumb(&ty, &th)) {
        cairo_set_source_rgb(cr, 0.42, 0.42, 0.45);
        cairo_rectangle(cr, gClientW - kSbThick + 2, ty, kSbThick - 4, th);
        cairo_fill(cr);
    }
    if (hThumb(&tx, &tw)) {
        cairo_set_source_rgb(cr, 0.42, 0.42, 0.45);
        cairo_rectangle(cr, tx, gClientH - kSbThick + 2, tw, kSbThick - 4);
        cairo_fill(cr);
    }
}

static void onAreaSize(GtkWidget* w, GtkAllocation* a, gpointer) {
    gClientW = a->width;
    gClientH = a->height;
    if (gFitWidth) applyFitWidth();
    clampScroll();
    ensureActivePage();
    renderPage();
}

static void scrollByDy(int dy) {
    gScrollY += dy;
    gRestoredPage = false;
    clampScroll();
    ensureActivePage();
}

static void toggleSearch(bool show) {
    if (!gSearchBar) return;
    gSearchOpen = show;
    if (show) { gtk_widget_show(gSearchBar); gtk_widget_grab_focus(gSearchEntry); }
    else { gtk_widget_hide(gSearchBar); ui_matchReset(); }
    renderPage();
}

// ---- forward declarations for the context-menu actions ----
static void doPrint();
static void openNoteEditor(int idx, bool fresh);
static int gMenuPinPage = -1;
static double gMenuPinX = 0, gMenuPinY = 0;
static int gMenuHlIdx = -1;
static int gMenuPinIdx = -1;

// ---- context menu ----
static void onMenuActivate(GtkMenuItem* mi, gpointer data) {
    int id = GPOINTER_TO_INT(data);
    switch (id) {
    case 101: copySelection(); break;
    case 103: toggleSearch(true); break;
    case 104: fitWidth(); break;
    case 105: zoomAt(1.2, gClientW / 2, gClientH / 2); break;
    case 106: zoomAt(1 / 1.2, gClientW / 2, gClientH / 2); break;
    case 107: { std::string f = ui_open_file(); if (!f.empty()) openPath(utf8ToWide(f)); } break;
    case 108: { std::string f = ui_open_file(); if (!f.empty()) addTab(utf8ToWide(f)); } break;
    case 109: gAutosave = !gAutosave; writeAppPref(); break;
    case 110: gtk_window_iconify(GTK_WINDOW(gWin)); break;
    case 111: if (gtk_window_is_maximized(GTK_WINDOW(gWin))) gtk_window_unmaximize(GTK_WINDOW(gWin)); else gtk_window_maximize(GTK_WINDOW(gWin)); break;
    case 112: requestQuit(); break;
    case 180: clearCustomColours(); break;
    // ---- the four colour menus. Each submenu answers two id families that
    // differ only in base - presets at 120/140/150/160, custom slots at
    // 126/146/156/166 - so one arm per menu replaces one case group per
    // family. Same scheme as the Windows build (src/main.cpp:5659).
    case 120: case 121: case 122: case 123: case 124: case 125:
    case 126: case 127: case 128: {             // recolour the menued highlight
        int c = menuColorIndex(id, 120, 126);
        if (c >= 0 && gMenuHlIdx >= 0) recolorHighlight(gMenuHlIdx, c);
        break;
    }
    case 136: {                                  // Custom... on a highlight
        int idx = -1;
        if (ui_color_box(idx) && idx >= 0 && gMenuHlIdx >= 0) recolorHighlight(gMenuHlIdx, idx);
        break;
    }
    case 140: case 141: case 142: case 143: case 144: case 145:
    case 146: case 147: case 148: {             // set the default highlight colour
        int c = menuColorIndex(id, 140, 146);
        if (c >= 0) {
            gHlDefault = c;
            writeAppPref();
            if (hasSelection()) applyHighlightFromSelection(gHlDefault);
        }
        break;
    }
    case 149: {                                  // Custom... on the default highlight
        int idx = -1;
        if (ui_color_box(idx) && idx >= 0) {
            gHlDefault = idx;
            writeAppPref();
            if (hasSelection()) applyHighlightFromSelection(gHlDefault);
        }
        break;
    }
    case 150: case 151: case 152: case 153: case 154: case 155:
    case 156: case 157: case 158: {             // recolour the menued pin
        int c = menuColorIndex(id, 150, 156);
        if (c >= 0 && gMenuPinIdx >= 0) recolorPin(gMenuPinIdx, c);
        break;
    }
    case 159: {                                  // Custom... on a pin
        int idx = -1;
        if (ui_color_box(idx) && idx >= 0 && gMenuPinIdx >= 0) recolorPin(gMenuPinIdx, idx);
        break;
    }
    case 160: case 161: case 162: case 163: case 164: case 165:
    case 166: case 167: case 168: {             // set the default pin colour
        int c = menuColorIndex(id, 160, 166);
        if (c >= 0) {
            gPinDefault = c;
            writeAppPref();
        }
        break;
    }
    case 169: {                                  // Custom... on the default pin
        int idx = -1;
        if (ui_color_box(idx) && idx >= 0) {
            gPinDefault = idx;
            writeAppPref();
        }
        break;
    }
    case 201: toggleOutlineDrawer(); break;
    case 202: toggleThumbDrawer(); break;
    case 207: reopenLastDocument(); break;
    case 118: doSave(); break;
    case 119: doSaveAs(); break;
    case 114: undoOp(); refreshDrawers(); break;
    case 115: redoOp(); refreshDrawers(); break;
    case 310: deletePage(gPageIndex); break;
    case 311: if (gPageIndex > 0) { movePage(gPageIndex, gPageIndex - 1); } break;
    case 312: if (gPageIndex + 1 < gDocPages) { movePage(gPageIndex, gPageIndex + 1); } break;
    case 133: rotatePage(gPageIndex, +1); refreshDrawers(); break;
    case 134: rotatePage(gPageIndex, -1); refreshDrawers(); break;
    case 200: gNight = !gNight; writeAppPref(); renderPage(); break;
    case 203: if (gDoc) pickSignatureImage(); break;
    case 204: clearSignatures(); break;
    case 205: {
        std::string f = ui_open_file();
        if (!f.empty()) mergeFiles({ utf8ToWide(f) });
        break;
    }
    case 206: {
        std::string range = "";
        if (ui_prompt_range(range) && !range.empty()) {
            std::string out = ui_save_file("split.pdf");
            if (!out.empty()) splitExport(utf8ToWide(out), range, false);
        }
        break;
    }
    case 300: addTab(L""); break;
    case 301: closeTab(); break;
    case 302: { bool vis = gtk_notebook_get_show_tabs(GTK_NOTEBOOK(gNotebook));
                gtk_notebook_set_show_tabs(GTK_NOTEBOOK(gNotebook), !vis); } break;
    case 113: toggleTitlebar(); break;
    case 135: {   // highlight the current selection with the default colour
        applyHighlightFromSelection(gHlDefault);
        break;
    }
    case 130: {   // add pin here (point captured when the menu opened)
        if (gDoc && gMenuPinPage >= 0 && gMenuPinPage < gDocPages) {
            int idx = (int)gPins.size();
            addPinAt(gMenuPinPage, gMenuPinX, gMenuPinY);
            openNoteEditor(idx, true);
        }
        break;
    }
    case 131: if (gMenuPinIdx >= 0) openNoteEditor(gMenuPinIdx, false); break;
    case 132: deletePin(gMenuPinIdx); break;
    case 116: {   // copy the text of the menued highlight (Windows id 116)
        if (gMenuHlIdx >= 0 && gMenuHlIdx < (int)gHls.size()) {
            const Hl& h2 = gHls[gMenuHlIdx];
            copyTextRange(h2.page, h2.start, h2.page, h2.start + h2.count - 1);
        }
        break;
    }
    case 117: deleteHighlight(gMenuHlIdx); break;
    case 137: doPrint(); break;
    case 170: runUpdateCheck(); break;
    }
}

// The menu structure, as text, for the parity suite. The Windows build's
// context menu is the reference (src/main.cpp:5510): the same entries, in the
// same order, with the same meaning. Printing the tree lets a test compare the
// two without clicking anything, which is what the Windows suites do with
// WM_COMMAND ids.
static void dumpMenuTree(GtkWidget* item, int depth) {
    GtkWidget* sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(item));
    GtkWidget* label = gtk_bin_get_child(GTK_BIN(item));
    const char* text = label ? gtk_label_get_text(GTK_LABEL(label)) : "";
    for (int i = 0; i < depth; i++) printf("  ");
    // the id is what the parity claim is about, so print it beside the label:
    // "Hide titlebar: 113" is checkable, "Hide titlebar" is not
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "cmd-id"));
    if (id) printf("%s: %d\n", text, id);
    else printf("%s\n", text);
    if (sub) {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(sub));
        for (GList* k = kids; k; k = k->next)
            if (GTK_IS_MENU_ITEM(k->data)) dumpMenuTree(GTK_WIDGET(k->data), depth + 1);
        g_list_free(kids);
    }
}

// Build the right-click menu for the current gMenuHlIdx / gMenuPinIdx /
// selection state. Split out from onContextMenu so the menu dump can build the
// same menu in each of its shapes.
static GtkWidget* buildContextMenu(int x, int y) {
    GtkWidget* menu = gtk_menu_new();
    int pg = pageAtDocY(y);

    auto add = [&](const char* label, int id) {
        GtkWidget* it = gtk_menu_item_new_with_label(label);
        g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
        g_object_set_data(G_OBJECT(it), "cmd-id", GINT_TO_POINTER(id));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    };
    auto sep = [&]() { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };

    if (gMenuPinIdx >= 0) {
        add("Edit note", 131);
        {
            GtkWidget* it = gtk_menu_item_new_with_label("Color");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(it),
                buildColorSubmenu(gPins[gMenuPinIdx].color, 150, 156, 159));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        add("Delete pin", 132);
        sep();
    } else if (gMenuHlIdx >= 0) {
        {
            GtkWidget* it = gtk_menu_item_new_with_label("Color");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(it),
                buildColorSubmenu(gHls[gMenuHlIdx].color, 120, 126, 136));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        add("Copy text", 116);
        add("Delete highlight", 117);
        sep();
    } else if (hasSelection()) {
        add("Highlight", 135);
        {
            GtkWidget* it = gtk_menu_item_new_with_label("Set default color");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(it),
                buildColorSubmenu(gHlDefault, 140, 146, 149));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        add("Copy", 101);
        sep();
    } else if (gDoc && pg >= 0) {
        double px, py;
        pagePxToPt(pg, x - docLeft() - pageLeftPx(pg), y - docTop() - yTopPx(pg), &px, &py);
        gMenuPinPage = pg; gMenuPinX = px; gMenuPinY = py;
        add("Add pin here", 130);
        {
            GtkWidget* it = gtk_menu_item_new_with_label("Set pin color");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(it),
                buildColorSubmenu(gPinDefault, 160, 166, 169));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        sep();
    }
    add("Open...", 107);
    add("Open in new tab...", 108);
    add("New tab", 300);
    add("Close tab", 301);
    sep();
    add("Save", 118);
    add("Save as...", 119);
    sep();
    add("Print...", 137);
    {
        // Windows draws a tick next to Autosave when it is on (MF_CHECKED). A
        // GtkCheckMenuItem flips itself on activate, so the handler would undo
        // it again - the tick is set here and the handler only records the
        // state, which is why it is a plain item with a drawn tick instead.
        GtkWidget* it = gtk_menu_item_new_with_label(gAutosave ? "Autosave ✓" : "Autosave");
        g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(109));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    sep();
    add("Undo", 114);
    add("Redo", 115);
    sep();
    add("Find...", 103);
    sep();
    add("Fit width", 104);
    add("Zoom in", 105);
    add("Zoom out", 106);
    sep();
    add("Rotate clockwise", 133);
    add("Rotate counter-clockwise", 134);
    sep();
    add("Delete this page", 310);
    add("Move page up", 311);
    add("Move page down", 312);
    sep();
    // The Advanced cascade, the same entries the Windows context menu carries
    // under it (src/main.cpp:5562): everything past plain reading.
    {
        GtkWidget* advItem = gtk_menu_item_new_with_label("Advanced");
        GtkWidget* advSub = gtk_menu_new();
        auto addAdv = [&](const char* label, int id) {
            GtkWidget* it = gtk_menu_item_new_with_label(label);
            g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
            g_object_set_data(G_OBJECT(it), "cmd-id", GINT_TO_POINTER(id));
            gtk_menu_shell_append(GTK_MENU_SHELL(advSub), it);
        };
        addAdv("Thumbnails", 202);
        addAdv("Outline panel", 201);
        // the same static label and tick the Windows build draws (MF_CHECKED,
        // src/main.cpp:5456): the label must not flip to "Day mode", which
        // would make the two readers show different words for one command
        addAdv(gNight ? "Night mode ✓" : "Night mode", 200);
        gtk_menu_shell_append(GTK_MENU_SHELL(advSub), gtk_separator_menu_item_new());
        addAdv("Merge PDFs...", 205);
        addAdv("Split pages...", 206);
        addAdv("Insert signature...", 203);
        addAdv("Clear signatures", 204);
        gtk_menu_shell_append(GTK_MENU_SHELL(advSub), gtk_separator_menu_item_new());
        addAdv("Reopen last document", 207);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(advItem), advSub);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), advItem);
    }
    sep();
    add(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(gNotebook)) ? "Hide tab strip" : "Show tab strip", 302);
    add(gTitlebar ? "Hide titlebar" : "Show titlebar", 113);
    sep();
    add("Minimize", 110);
    add("Maximize / restore", 111);
    sep();
    add("Check for updates", 170);
    add("Quit", 112);
    return menu;
}

static void onContextMenu(GtkWidget* w, GdkEventButton* ev) {
    int x = (int)ev->x, y = (int)ev->y;
    int pg;
    int ci = charIndexAtDoc(x, y, &pg);
    gMenuHlIdx = hlAt(pg, ci);
    gMenuPinIdx = pinAt(x, y);
    gMenuPinPage = -1;
    GtkWidget* menu = buildContextMenu(x, y);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
}

// The menu has four shapes: a pin under the cursor, a highlight, a live
// selection, and neither. The mark-specific commands exist only in their own
// shape, so a dump that opens one menu can never see them all. This builds
// each shape in turn and prints it.
static void dumpEveryMenuShape() {
    int savedHl = gMenuHlIdx, savedPin = gMenuPinIdx, savedPinPage = gMenuPinPage;
    for (int shape = 0; shape < 4; shape++) {
        gMenuHlIdx = -1;
        gMenuPinIdx = -1;
        gMenuPinPage = -1;
        if (shape == 1 && !gHls.empty()) gMenuHlIdx = 0;
        else if (shape == 2 && !gPins.empty()) gMenuPinIdx = 0;
        else if (shape == 3) gMenuHlIdx = -1;
        if (shape == 1 && gMenuHlIdx < 0) continue;   // no highlights to show
        if (shape == 2 && gMenuPinIdx < 0) continue;  // no pins to show
        GtkWidget* menu = buildContextMenu(gClientW / 2, gClientH / 2);
        GList* kids = gtk_container_get_children(GTK_CONTAINER(menu));
        for (GList* k = kids; k; k = k->next)
            if (GTK_IS_MENU_ITEM(k->data)) dumpMenuTree(GTK_WIDGET(k->data), 0);
        g_list_free(kids);
        gtk_widget_destroy(menu);
    }
    gMenuHlIdx = savedHl;
    gMenuPinIdx = savedPin;
    gMenuPinPage = savedPinPage;
}

// ---- mouse ----
static void onButtonPress(GtkWidget* w, GdkEventButton* ev, gpointer) {
    gtk_widget_grab_focus(w);
    if (ev->button == GDK_BUTTON_SECONDARY) { onContextMenu(w, ev); return; }
    if (ev->button != GDK_BUTTON_PRIMARY) return;

    // scrollbars first
    int vx, vy, vw, vh, hx, hy, hw, hh;
    sbRects(&vx, &vy, &vw, &vh, &hx, &hy, &hw, &hh);
    int ty, th, tx, tw;
    if (ev->x >= vx && vThumb(&ty, &th)) {
        if (ev->y >= ty && ev->y < ty + th) { gSbDragging = true; gSbWhich = 1; gSbGrab = (int)ev->y - ty; }
        else {   // page up/down in the track
            int total = docHpx(), view = docViewH();
            if (total > view) {
                int step = (ev->y < ty ? -1 : 1) * view * 9 / 10;
                scrollByDy(step); renderPage();
            }
        }
        return;
    }
    if (ev->y >= hy && hThumb(&tx, &tw)) {
        if (ev->x >= tx && ev->x < tx + tw) { gSbDragging = true; gSbWhich = 2; gSbGrab = (int)ev->x - tx; }
        else {
            int total = docWpx() + 2 * MARGIN, view = gClientW;
            if (total > view) {
                gScrollX += (ev->x < tx ? -1 : 1) * view * 9 / 10;
                clampScroll(); renderPage();
            }
        }
        return;
    }

    if (!gSigPlacing.empty()) {
        for (int i = 0; i < gDocPages; i++) {
            int px = pageVx(i), py = pageVy(i), pw = pageWpx(i), ph = pageHpx(i);
            if (ev->x >= px && ev->x < px + pw && ev->y >= py && ev->y < py + ph) {
                commitSigPlacement(i, ev->x - px, ev->y - py);
                return;
            }
        }
        return;
    }
    if (ev->type == GDK_2BUTTON_PRESS) {
        int pg;
        int idx = charIndexClamped((int)ev->x, (int)ev->y, &pg);
        if (idx < 0 || pg < 0) return;
        gSelAnchorPage = gSelHeadPage = pg;
        gSelAnchor = gSelHead = idx;
        selectWord(pg, idx);
        gDragging = true;
        renderPage();
        return;
    }
    if (ev->type == GDK_3BUTTON_PRESS) {
        int pg;
        int idx = charIndexClamped((int)ev->x, (int)ev->y, &pg);
        if (idx < 0 || pg < 0) return;
        selectLine(pg, idx);
        gDragging = true;
        renderPage();
        return;
    }
    int pg;
    int idx = charIndexClamped((int)ev->x, (int)ev->y, &pg);
    gSelAnchorPage = gSelHeadPage = pg;
    gSelAnchor = gSelHead = idx;
    gDragging = true;
    renderPage();
}

static void onButtonRelease(GtkWidget*, GdkEventButton* ev, gpointer) {
    if (ev->button != GDK_BUTTON_PRIMARY) return;
    gDragging = false;
    gSbDragging = false;
}

static void onMotion(GtkWidget* w, GdkEventMotion* ev, gpointer) {
    if (gSbDragging) {
        int vx, vy, vw, vh, hx, hy, hw, hh;
        sbRects(&vx, &vy, &vw, &vh, &hx, &hy, &hw, &hh);
        (void)vy; (void)vw; (void)vh; (void)hx; (void)hy; (void)hw; (void)hh;
        int ty, th, tx, tw;
        if (gSbWhich == 1 && vThumb(&ty, &th)) {
            int h = gClientH - kSbThick;
            int span = h - th;
            double frac = span > 0 ? ((double)ev->y - gSbGrab) / span : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int total = docHpx(), view = docViewH();
            gScrollY = (int)(frac * (total - view));
            gRestoredPage = false;
            clampScroll(); ensureActivePage(); renderPage();
        } else if (gSbWhich == 2 && hThumb(&tx, &tw)) {
            int ww = gClientW - kSbThick;
            int span = ww - tw;
            double frac = span > 0 ? ((double)ev->x - gSbGrab) / span : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int total = docWpx() + 2 * MARGIN, view = gClientW;
            gScrollX = (int)(frac * (total - view));
            clampScroll(); renderPage();
        }
        return;
    }
    // cursor shape over text
    int pg;
    int idx = charIndexClamped((int)ev->x, (int)ev->y, &pg);
    gdk_window_set_cursor(gtk_widget_get_window(w), idx >= 0 ? gIbeam : gArrow);
    if (gDragging) {
        int i2 = charIndexClamped((int)ev->x, (int)ev->y, &pg);
        if (i2 >= 0 && (pg != gSelHeadPage || i2 != gSelHead)) {
            gSelHeadPage = pg; gSelHead = i2;
            renderPage();
        }
    }
}

static void onScroll(GtkWidget*, GdkEventScroll* ev, gpointer) {
    bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    // GTK's scroll deltas read as content motion: a positive delta_y scrolls
    // down, just as a discrete GDK_SCROLL_DOWN does (measured here: smooth
    // delta_y = +1 raises a GtkScrolledWindow's vadjustment by the same step as
    // DOWN). So the smooth path takes the deltas with no negation and that
    // matches the discrete sign below; delta_x is a horizontal pan. The gate's
    // Xvfb session only ever delivers discrete UP/DOWN events, so it cannot
    // catch a sign error in this smooth path.
    double dy = (ev->direction == GDK_SCROLL_UP) ? -1 : (ev->direction == GDK_SCROLL_DOWN) ? 1 : 0;
    double dx = 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) { dx = ev->delta_x; dy = ev->delta_y; }
    if (dy == 0 && dx == 0) return;
    // smooth deltas arrive in small steps, so accumulate until a step is worth
    // acting on; a discrete UP/DOWN event is already a whole step
    static double sSmoothAccX = 0, sSmoothAccY = 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        sSmoothAccX += dx; sSmoothAccY += dy;
        if (fabs(sSmoothAccX) < 1.0 && fabs(sSmoothAccY) < 1.0) return;
        dx = fabs(sSmoothAccX) >= 1.0 ? (sSmoothAccX > 0 ? 1 : -1) : 0;
        dy = fabs(sSmoothAccY) >= 1.0 ? (sSmoothAccY > 0 ? 1 : -1) : 0;
        if (dx) sSmoothAccX = 0;
        if (dy) sSmoothAccY = 0;
    }
    if (ctrl) {
        if (dy) zoomAt(dy < 0 ? 1.1 : 1 / 1.1, (int)ev->x, (int)ev->y);
    } else {
        if (dy) { scrollByDy((int)(dy * 3 * 40)); markSave(); }
        if (dx) { gScrollX += (int)(dx * 3 * 40); clampScroll(); }
        renderPage();
    }
}

// ---- keyboard ----
static gboolean onKey(GtkWidget*, GdkEventKey* ev, gpointer) {
    bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    bool shift = (ev->state & GDK_SHIFT_MASK) != 0;
    int page = gClientH * 9 / 10;
    bool scrolled = false;
    if (!gSigPlacing.empty()) {
        if (ev->keyval == GDK_KEY_Escape) gSigPlacing.clear();
        return TRUE;
    }
    switch (ev->keyval) {
    case GDK_KEY_Page_Down: case GDK_KEY_space: scrollByDy(page); scrolled = true; break;
    case GDK_KEY_Page_Up: scrollByDy(-page); scrolled = true; break;
    case GDK_KEY_Down: scrollByDy(60); scrolled = true; break;
    case GDK_KEY_Up: scrollByDy(-60); scrolled = true; break;
    case GDK_KEY_Left: gScrollX -= 60; clampScroll(); scrolled = true; break;
    case GDK_KEY_Right: gScrollX += 60; clampScroll(); scrolled = true; break;
    case GDK_KEY_0: if (ctrl) fitWidth(); break;
    case GDK_KEY_f: if (ctrl) toggleSearch(true); break;
    case GDK_KEY_o: if (ctrl) { std::string f = ui_open_file(); if (!f.empty()) openPath(utf8ToWide(f)); } break;
    case GDK_KEY_t: if (ctrl) addTab(L""); break;
    case GDK_KEY_w: if (ctrl) closeTab(); break;
    case GDK_KEY_Tab:
        if (ctrl) gtk_notebook_set_current_page(GTK_NOTEBOOK(gNotebook),
                                                (gActiveDoc + (shift ? gDocCount - 1 : 1)) % gDocCount);
        break;
    case GDK_KEY_c: if (ctrl) copySelection(); break;
    case GDK_KEY_F3: if (!gSearchOpen) toggleSearch(true); else nextMatch(shift ? -1 : 1); break;
    case GDK_KEY_s: if (ctrl) { if (shift) doSaveAs(); else doSave(); } break;
    case GDK_KEY_z: if (ctrl) { if (shift) redoOp(); else undoOp(); } break;
    case GDK_KEY_y: if (ctrl) redoOp(); break;
    case GDK_KEY_p: if (ctrl && gDoc) doPrint(); break;
    case GDK_KEY_plus: case GDK_KEY_equal: if (ctrl || ev->keyval == GDK_KEY_plus) zoomAt(1.2, gClientW / 2, gClientH / 2); break;
    case GDK_KEY_minus: zoomAt(1 / 1.2, gClientW / 2, gClientH / 2); break;
    case GDK_KEY_Escape:
        if (gSearchOpen) toggleSearch(false);
        else requestQuit();
        break;
    default: return FALSE;
    }
    if (scrolled) { markSave(); renderPage(); }
    return TRUE;
}

// ---- the drawing area factory ----
static void makeArea(GtkWidget** out) {
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(area, TRUE);
    gtk_widget_add_events(area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
        GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(area, "draw", G_CALLBACK(onDraw), nullptr);
    g_signal_connect(area, "size-allocate", G_CALLBACK(onAreaSize), nullptr);
    g_signal_connect(area, "button-press-event", G_CALLBACK(onButtonPress), nullptr);
    g_signal_connect(area, "button-release-event", G_CALLBACK(onButtonRelease), nullptr);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(onMotion), nullptr);
    g_signal_connect(area, "scroll-event", G_CALLBACK(onScroll), nullptr);
    g_signal_connect(area, "key-press-event", G_CALLBACK(onKey), nullptr);
    *out = area;
}

// ---- find bar ----
static void onSearchChanged(GtkEditable* e, gpointer) {
    gQuery = utf8ToWide(gtk_entry_get_text(GTK_ENTRY(e)));
    runSearch();
    char buf[64];
    int n = searchCount();
    if (n > 0) snprintf(buf, sizeof(buf), "%d matches", n);
    else snprintf(buf, sizeof(buf), "%s", gQuery.empty() ? "" : "no matches");
    gtk_label_set_text(GTK_LABEL(gSearchLabel), buf);
}
static gboolean onSearchKey(GtkWidget*, GdkEventKey* ev, gpointer) {
    if (ev->keyval == GDK_KEY_Escape) { toggleSearch(false); return TRUE; }
    if (ev->keyval == GDK_KEY_Return) { nextMatch((ev->state & GDK_SHIFT_MASK) ? -1 : 1); return TRUE; }
    if (ev->keyval == GDK_KEY_F3) { nextMatch((ev->state & GDK_SHIFT_MASK) ? -1 : 1); return TRUE; }
    return FALSE;
}

static void buildSearchBar(GtkWidget* overlay) {
    gSearchBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(gSearchBar, GTK_ALIGN_END);
    gtk_widget_set_valign(gSearchBar, GTK_ALIGN_START);
    gtk_widget_set_margin_top(gSearchBar, 6);
    gtk_widget_set_margin_end(gSearchBar, 18);
    gtk_style_context_add_class(gtk_widget_get_style_context(gSearchBar), "linked");
    gSearchEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(gSearchEntry), "Find");
    gtk_widget_set_size_request(gSearchEntry, 240, -1);
    gSearchLabel = gtk_label_new("");
    gtk_widget_set_margin_start(gSearchLabel, 6);
    gtk_widget_set_margin_end(gSearchLabel, 6);
    gtk_box_pack_start(GTK_BOX(gSearchBar), gSearchEntry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gSearchBar), gSearchLabel, FALSE, FALSE, 0);
    g_signal_connect(gSearchEntry, "changed", G_CALLBACK(onSearchChanged), nullptr);
    g_signal_connect(gSearchEntry, "key-press-event", G_CALLBACK(onSearchKey), nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), gSearchBar);
    gtk_widget_hide(gSearchBar);
}

// ---- main window ----
static gboolean onSaveTick(gpointer) {
    if (sidecarPending()) writeSidecarNow();
    return G_SOURCE_CONTINUE;
}

// test seam (the Windows app has the same hooks): after the first render, write
// the back buffer to a PNG when MNPDF_SHOT names a path, and quit when
// MNPDF_SHOT_EXIT is set - so a suite can assert on the pixels the app paints
// with no display and no harness clicking.
static gboolean onShotIdle(gpointer) {
    // MNPDF_CMD drives one command by its menu id before the shot is taken, the
    // same way the Windows suites post WM_COMMAND: the drawers, the titlebar
    // toggle and the night mode all change what is painted, so a suite that can
    // only take a picture of the start state cannot assert on them at all.
    // The command runs once. It may have added a drawer, which keeps a 1-px
    // allocation until a layout pass runs, so the rest of this seam (the shot
    // and the quit) waits one pass before it runs. Draining the loop inside the
    // command handler did the same thing, but from inside a handler, where it
    // re-enters other pending callbacks - the update check's reply opens a
    // modal, i.e. a nested main loop inside the drain.
    static bool ranCommand = false;
    const char* cmd = getenv("MNPDF_CMD");
    if (cmd && *cmd && !ranCommand) {
        ranCommand = true;
        int id = atoi(cmd);
        if (id > 0) onMenuActivate(nullptr, GINT_TO_POINTER(id));
        g_timeout_add(150, onShotIdle, nullptr);
        return G_SOURCE_REMOVE;
    }
    // MNPDF_MENU_DUMP prints the menu tree as "label: id" and exits. The menu
    // has several shapes - nothing under the cursor, a highlight, a pin, a
    // selection - and the mark-specific commands only exist in their own shape,
    // so every shape is built and printed. Opening one menu at a point with
    // nothing under it hid exactly the ids the parity check needs to see.
    if (getenv("MNPDF_MENU_DUMP")) {
        dumpEveryMenuShape();
        fflush(stdout);
        exit(0);
    }
    const char* path = getenv("MNPDF_SHOT");
    // MNPDF_SHOT_WINDOW captures the whole window (menu bar, drawers and all)
    // instead of only the page back buffer, so a suite can assert on chrome the
    // page buffer never contains.
    if (path && *path && getenv("MNPDF_SHOT_WINDOW")) {
        GdkWindow* gw = gtk_widget_get_window(gWin);
        if (gw) {
            GdkWindow* target = gdk_window_get_toplevel(gw);
            int w = gdk_window_get_width(target), h = gdk_window_get_height(target);
            if (w > 0 && h > 0) {
                cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                cairo_t* cr = cairo_create(s);
                gdk_cairo_set_source_window(cr, target, 0, 0);
                cairo_paint(cr);
                cairo_destroy(cr);
                cairo_surface_write_to_png(s, path);
                cairo_surface_destroy(s);
                printf("shot: %s (%dx%d window)\n", path, w, h);
            }
        }
    } else if (path && *path && ui_backbuf_bits() && ui_backbuf_w() > 0) {
        cairo_surface_t* s = cairo_image_surface_create_for_data(
            (unsigned char*)ui_backbuf_bits(), CAIRO_FORMAT_ARGB32,
            ui_backbuf_w(), ui_backbuf_h(), ui_backbuf_w() * 4);
        cairo_surface_write_to_png(s, path);
        cairo_surface_destroy(s);
        printf("shot: %s (%dx%d)\n", path, ui_backbuf_w(), ui_backbuf_h());
    }
    if (getenv("MNPDF_SHOT_EXIT")) {
        // through the REAL quit path, so a suite that sets this exercises the
        // same save logic the Esc key, the menu Quit and the window close use -
        // a seam that saved on its own would hide a broken quit path
        requestQuit();
    }
    return G_SOURCE_REMOVE;
}

static void onNotebookSwitch(GtkNotebook*, GtkWidget*, guint page, gpointer) {
    if ((int)page == gActiveDoc) { gCurArea = gtk_notebook_get_nth_page(GTK_NOTEBOOK(gNotebook), page); return; }
    settleActiveDoc();
    flushPageCache();
    gActiveDoc = (int)page;
    gCurArea = gtk_notebook_get_nth_page(GTK_NOTEBOOK(gNotebook), page);
    loadPage(gPageIndex);
    relayoutPages();
    clampScroll();
    // an open drawer follows the new tab, the same place the Windows build
    // refreshes it (src/main.cpp:5296)
    refreshDrawers();
    renderPage();
}

static gboolean onWindowDelete(GtkWidget*, GdkEvent*, gpointer) {
    settleActiveDoc();
    if (gAutosave) writeAllSidecars();
    else if (gDirty) {
        int r = ui_message("Save changes?", "mnpdf", true);
        if (r == UI_MSGBOX_CANCEL) return TRUE;   // keep working, decide later
        if (r == UI_MSGBOX_YES && !doSave()) return TRUE;
        writeAllSidecars();
    } else writeAllSidecars();
    writeAppPref();
    return FALSE;
}

// ---- titlebar toggle -------------------------------------------------------
// Windows clears WS_CAPTION and then re-implements WM_NCCALCSIZE and
// WM_NCHITTEST (src/main.cpp:5825, 6014) because removing the caption there also
// removes the frame's own resize and drag behaviour. GTK keeps the window
// manager in charge of all of that, so the honest equivalent is one call: the
// WM still resizes, snaps and maximises the window, and only the caption strip
// disappears. That is also why no hit-test override exists here - there is
// nothing to re-implement.
static void toggleTitlebar() {
    gTitlebar = !gTitlebar;
    gtk_window_set_decorated(GTK_WINDOW(gWin), gTitlebar);
    writeAppPref();
    renderPage();
}

// ---- thumbnails and outline drawers ----------------------------------------
// The Windows app hosts each drawer in its own WS_POPUP window placed beside the
// reader (src/main.cpp:4886 placeDrawer). GTK does the same job with a
// GtkPaned-free side box inside the window: a fixed-width child packed beside
// the notebook, so the drawer moves and resizes with the reader instead of
// needing to be repositioned after every move. The drawn content is the same
// code path - a cairo surface built from renderPageToBuffer for the thumbnail,
// and the bookmark list for the outline.
static GtkWidget* gDrawerBox = nullptr;      // the hbox holding [thumb][outline][reader]
static GtkWidget* gThumbDrawer = nullptr;
static GtkWidget* gThumbArea = nullptr;
static GtkWidget* gOutlineDrawer = nullptr;
static GtkWidget* gOutlineArea = nullptr;

// the same numbers the Windows drawers use, so the two readers look alike
static const int kThumbW = 108;
static const int kThumbGap = 6;
// the drawer's design height, used before the widget has an allocation of its
// own (the first refresh can run before the first layout pass)
static const int kThumbListPad = 400;
static const int kOutlineW = 240;
static const int kBmRow = 18;

static std::vector<unsigned char> gThumbPix;   // one page's BGRA, drawn on demand
static std::vector<int> gThumbHs;
static int gThumbSel = -1;
static int gThumbScroll = 0;
static double gThumbSmoothAcc = 0;   // fractional trackpad deltas, accumulated
static int gThumbDrawnFor = -1;               // which page gThumbPix holds

struct BmEntry { std::wstring title; int page; int depth; };
static std::vector<BmEntry> gBmList;
static int gBmSel = -1;

// Recompute the row heights. The cached rasters are kept unless the page count
// changed, which is the Windows shape (row heights rebuilt, each row's picture
// kept - refreshThumbDrawer, src/main.cpp:4813-4957): a refresh after a merge or
// a reopen must not blank the drawer, and re-rasterizing every row on each
// refresh costs a page render per row on the UI thread.
static void layoutThumbs() {
    gThumbHs.clear();
    if (!gDoc) { gThumbPix.clear(); gThumbDrawnFor = -1; return; }
    if ((int)gThumbPix.size() != gDocPages) { gThumbPix.clear(); gThumbDrawnFor = -1; }
    for (int i = 0; i < gDocPages; i++) {
        double ar = (gPageH[i] > 0 && gPageW[i] > 0) ? gPageH[i] / gPageW[i] : 1.3;
        int h = (int)(kThumbW * ar + 0.5);
        if (h < 8) h = 8;
        if (h > 400) h = 400;
        gThumbHs.push_back(h);
    }
}

// the height the list is drawn into: the widget's allocation once it has one,
// and the drawer's design height before the first layout pass
static int thumbAreaHeight() {
    GtkAllocation al;
    if (gThumbArea && gtk_widget_get_visible(gThumbArea)) {
        gtk_widget_get_allocation(gThumbArea, &al);
        if (al.height > 0) return al.height;
    }
    return kThumbListPad;
}

static int thumbListHeight() {
    int total = 0;
    for (int hh : gThumbHs) total += hh + kThumbGap;
    return total;
}

// client y -> page index under it, or -1 (same arithmetic as thumbAt)
static int thumbAt(int y) {
    int cy = 4 - gThumbScroll;
    for (size_t i = 0; i < gThumbHs.size(); i++) {
        if (y >= cy && y < cy + gThumbHs[i]) return (int)i;
        cy += gThumbHs[i] + kThumbGap;
    }
    return -1;
}

static void openPageAt(int idx) {
    if (!gDoc || idx < 0 || idx >= gDocPages) return;
    gRestoredPage = false;
    loadPage(idx);
    gScrollY = yTopPx(idx) - MARGIN;
    clampScroll();
    renderPage();
    ui_invalidate();
}

static gboolean onThumbDraw(GtkWidget*, cairo_t* cr, gpointer) {
    GtkAllocation al;
    gtk_widget_get_allocation(gThumbArea, &al);
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
    cairo_paint(cr);
    if (!gDoc || gThumbHs.empty()) return TRUE;
    int cy = 4 - gThumbScroll;
    for (size_t i = 0; i < gThumbHs.size(); i++) {
        int th = gThumbHs[i];
        if (cy + th >= 0 && cy <= al.height) {
            int x = (al.width - kThumbW) / 2;
            if ((int)i == gThumbSel) {
                cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
                cairo_rectangle(cr, x - 2, cy - 2, kThumbW + 4, th + 4);
                cairo_fill(cr);
            }
            // one page rasterized at a time, only when it reaches the screen:
            // drawing the whole document up front would parse every page before
            // the drawer appeared, exactly what the Windows drawer avoids
            if (gThumbDrawnFor != (int)i) {
                std::vector<unsigned char> buf;
                int stride = 0;
                if (renderPageToBuffer((int)i, kThumbW, th, buf, &stride)) {
                    gThumbPix.swap(buf);
                    gThumbDrawnFor = (int)i;
                }
            }
            if (gThumbDrawnFor == (int)i && !gThumbPix.empty()) {
                cairo_surface_t* s = cairo_image_surface_create_for_data(
                    gThumbPix.data(), CAIRO_FORMAT_ARGB32, kThumbW, th, kThumbW * 4);
                cairo_set_source_surface(cr, s, x, cy);
                cairo_paint(cr);
                cairo_surface_destroy(s);
            }
            char num[16];
            snprintf(num, sizeof(num), "%d", (int)i + 1);
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
            cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11);
            cairo_move_to(cr, x, cy + th + 12);
            cairo_show_text(cr, num);
        }
        cy += th + kThumbGap;
    }
    return TRUE;
}

static gboolean onThumbPress(GtkWidget*, GdkEventButton* ev, gpointer) {
    if (ev->button != GDK_BUTTON_PRIMARY) return FALSE;
    int idx = thumbAt((int)ev->y);
    if (idx >= 0 && idx < gDocPages) { gThumbSel = idx; openPageAt(idx); }
    return TRUE;
}

static gboolean onThumbScrollEv(GtkWidget*, GdkEventScroll* ev, gpointer) {
    int dy = 0;
    if (ev->direction == GDK_SCROLL_UP) dy = -1;
    else if (ev->direction == GDK_SCROLL_DOWN) dy = 1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        // A touchpad sends delta_y of about +/-0.1 to 0.3 per event, and
        // (int)0.2 is 0, so truncating here swallowed every trackpad scroll and
        // the drawer never moved while the page beside it did. Accumulate the
        // fraction instead, exactly as the document's own scroll handler does.
        gThumbSmoothAcc += ev->delta_y;
        dy = (int)gThumbSmoothAcc;
        if (dy) gThumbSmoothAcc -= dy;
    }
    if (!dy) return TRUE;
    // the same positive-is-forward convention the document itself uses; see the
    // scroll probe note in docs/linux.md
    gThumbScroll += dy * 60;
    int maxScroll = thumbListHeight() - thumbAreaHeight();
    if (maxScroll < 0) maxScroll = 0;
    if (gThumbScroll < 0) gThumbScroll = 0;
    if (gThumbScroll > maxScroll) gThumbScroll = maxScroll;
    gtk_widget_queue_draw(gThumbArea);
    return TRUE;
}

static void collectBookmarks(FPDF_BOOKMARK bm, int depth) {
    for (; bm; bm = FPDFBookmark_GetNextSibling(gDoc, bm)) {
        unsigned int len = FPDFBookmark_GetTitle(bm, nullptr, 0);
        std::vector<unsigned short> buf(len + 1, 0);
        if (len) FPDFBookmark_GetTitle(bm, buf.data(), (int)buf.size());
        BmEntry e;
        e.title = u16ToWide(buf.data(), len);
        e.depth = depth;
        e.page = -1;
        FPDF_DEST dest = FPDFBookmark_GetDest(gDoc, bm);
        if (dest) {
            int pi = FPDFDest_GetDestPageIndex(gDoc, dest);
            if (pi >= 0) e.page = pi;
        }
        if (e.page < 0) {
            FPDF_ACTION act = FPDFBookmark_GetAction(bm);
            if (act && FPDFAction_GetType(act) == PDFACTION_GOTO) {
                FPDF_DEST ad = FPDFAction_GetDest(gDoc, act);
                int pi = ad ? FPDFDest_GetDestPageIndex(gDoc, ad) : -1;
                if (pi >= 0) e.page = pi;
            }
        }
        gBmList.push_back(e);
        collectBookmarks(FPDFBookmark_GetFirstChild(gDoc, bm), depth + 1);
    }
}

static void buildBmList() {
    gBmList.clear();
    gBmSel = -1;
    if (!gDoc) return;
    collectBookmarks(FPDFBookmark_GetFirstChild(gDoc, nullptr), 0);
}

static gboolean onOutlineDraw(GtkWidget*, cairo_t* cr, gpointer) {
    GtkAllocation al;
    gtk_widget_get_allocation(gOutlineArea, &al);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    if (gBmList.empty()) {
        cairo_set_source_rgb(cr, 0.47, 0.47, 0.47);
        cairo_move_to(cr, 8, 24);
        cairo_show_text(cr, "This document has no bookmarks.");
        return TRUE;
    }
    int y = 4;
    for (size_t i = 0; i < gBmList.size(); i++) {
        if (y > al.height) break;
        if ((int)i == gBmSel) {
            cairo_set_source_rgb(cr, 0.83, 0.83, 0.83);
            cairo_rectangle(cr, 0, y, al.width, kBmRow);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        std::string t = wideToUtf8(gBmList[i].title);
        cairo_move_to(cr, 4 + gBmList[i].depth * 12, y + 13);
        cairo_show_text(cr, t.c_str());
        y += kBmRow;
    }
    return TRUE;
}

static gboolean onOutlinePress(GtkWidget*, GdkEventButton* ev, gpointer) {
    if (ev->button != GDK_BUTTON_PRIMARY) return FALSE;
    int idx = ((int)ev->y - 4) / kBmRow;
    if (idx >= 0 && idx < (int)gBmList.size()) {
        gBmSel = idx;
        if (gBmList[idx].page >= 0) openPageAt(gBmList[idx].page);
        gtk_widget_queue_draw(gOutlineArea);
    }
    return TRUE;
}

static void toggleThumbDrawer() {
    if (!gDoc) return;
    if (gThumbDrawer) {
        gtk_widget_destroy(gThumbDrawer);
        gThumbDrawer = gThumbArea = nullptr;
        gThumbScroll = 0;
        return;
    }
    gThumbSel = gPageIndex;
    layoutThumbs();
    gThumbDrawer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(gThumbDrawer, kThumbW + 24, -1);
    gThumbArea = gtk_drawing_area_new();
    gtk_widget_add_events(gThumbArea, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    g_signal_connect(gThumbArea, "draw", G_CALLBACK(onThumbDraw), nullptr);
    g_signal_connect(gThumbArea, "button-press-event", G_CALLBACK(onThumbPress), nullptr);
    g_signal_connect(gThumbArea, "scroll-event", G_CALLBACK(onThumbScrollEv), nullptr);
    gtk_box_pack_start(GTK_BOX(gThumbDrawer), gThumbArea, TRUE, TRUE, 0);
    // The drawer is shown BEFORE it is packed into the strip. A widget packed
    // while still hidden is allocated on the next size negotiation the strip
    // does not ask for, so it stays at 1 px wide forever - measured with a
    // GtkBox/GtkNotebook probe, which is why this ordering is not cosmetic.
    gtk_widget_show_all(gThumbDrawer);
    gtk_box_pack_start(GTK_BOX(gDrawerBox), gThumbDrawer, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(gDrawerBox), gThumbDrawer, 0);
    // The strip has already negotiated a size once, so a child added afterwards
    // is not part of that negotiation and keeps a 1-px allocation until the strip
    // is told to lay itself out again. Measured with a GtkBox probe: without this
    // call the drawer stays invisible while the reader keeps the full width.
    // A child added to a box that has already negotiated its size keeps a 1-px
    // allocation until BOTH the strip and the window are told to lay themselves
    // out again, and the request is then drained. Measured with a GtkBox probe
    // (menu bar + strip + overlay, the app's own shape): queueing only the strip
    // leaves the drawer invisible while the reader keeps the full width.
    // Queueing is enough: the resize is serviced on the next layout pass, which
    // the shot seam waits for (it runs from an idle, 400 ms after a command).
    // Draining the loop here instead would re-enter pending callbacks from
    // inside a command handler, and the update check's reply is one of them -
    // its handler opens a modal, i.e. a nested main loop inside this drain.
    gtk_widget_queue_resize(gDrawerBox);
    gtk_widget_queue_resize(gWin);
}

static void toggleOutlineDrawer() {
    if (!gDoc) return;
    if (gOutlineDrawer) {
        gtk_widget_destroy(gOutlineDrawer);
        gOutlineDrawer = gOutlineArea = nullptr;
        return;
    }
    buildBmList();
    gOutlineDrawer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(gOutlineDrawer, kOutlineW, -1);
    gOutlineArea = gtk_drawing_area_new();
    gtk_widget_add_events(gOutlineArea, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(gOutlineArea, "draw", G_CALLBACK(onOutlineDraw), nullptr);
    g_signal_connect(gOutlineArea, "button-press-event", G_CALLBACK(onOutlinePress), nullptr);
    gtk_box_pack_start(GTK_BOX(gOutlineDrawer), gOutlineArea, TRUE, TRUE, 0);
    // shown before packing, same reason as the thumbnails drawer
    gtk_widget_show_all(gOutlineDrawer);
    gtk_box_pack_start(GTK_BOX(gDrawerBox), gOutlineDrawer, FALSE, FALSE, 0);
    // the outline sits to the RIGHT of the reader on Windows (side 1), so it is
    // packed after the notebook rather than before it
    gtk_box_reorder_child(GTK_BOX(gDrawerBox), gOutlineDrawer, 2);
    // same post-add relayout as the thumbnails drawer; see that comment
    gtk_widget_queue_resize(gDrawerBox);
    gtk_widget_queue_resize(gWin);
}

// the Windows app calls this after anything that changes the pages under the
// drawers (delete, move, rotate, a new document)
void refreshDrawers() {
    if (gThumbDrawer) {
        layoutThumbs();
        // F5: Windows keeps the ring on the page it named unless that page is
        // gone (src/main.cpp:4964-4967); re-pointing it at the document's page
        // on every refresh would silently move the page a later "Delete this
        // page" acts on.
        if (gThumbSel < 0 || gThumbSel >= gDocPages) gThumbSel = gPageIndex;
        if (gThumbSel < 0) gThumbSel = 0;
        // F9: a merge or a delete shrinks the list, and an unclamped offset then
        // paints an empty grey panel with no rows and no way to tell why
        int maxScroll = thumbListHeight() - thumbAreaHeight();
        if (maxScroll < 0) maxScroll = 0;
        if (gThumbScroll > maxScroll) gThumbScroll = maxScroll;
        if (gThumbScroll < 0) gThumbScroll = 0;
        gtk_widget_queue_draw(gThumbArea);
    }
    if (gOutlineDrawer) { buildBmList(); gtk_widget_queue_draw(gOutlineArea); }
}

// ---- Reopen last document --------------------------------------------------
// The document this one replaced. last.txt is no use: autosave keeps it pointing
// at whatever is open, so reading it would reopen the same file.
static void reopenLastDocument() {
    if (!gDoc) return;
    std::wstring want = gPrevPath;
    if (want.empty() || want == gPath) {
        ui_message("No earlier document to reopen.", "mnpdf", false);
        return;
    }
    if (access(wideToUtf8(want).c_str(), R_OK) != 0) {
        ui_message("That document has moved or been deleted since you left it.", "mnpdf", false);
        return;
    }
    openPath(want);
}

// ---- Clear custom colours --------------------------------------------------
// Hand the three custom slots back: with only three, a reader who wants a
// different set has to be able to start from empty. A mark still pointing at a
// cleared slot takes the preset for its kind, so it stays visible instead of
// turning into whatever colour the slot happens to hold.
static void clearCustomColours() {
    bool any = false;
    for (int c = 0; c < kPalCustom; c++) if (gPalCustom[c] != CLR_INVALID) { any = true; break; }
    if (!any) return;
    // ui_message answers YES=1 / NO=0 / CANCEL=2, so only YES may clear: with
    // a plain `!ui_message(...)` guard, No kept the colours and Cancel wiped
    // them, which is backwards for both answers
    if (ui_message("Clear your custom colours?\n\nMarks that use one fall back to the "
                   "preset for that kind - a highlight to yellow, a pin to red.",
                   "mnpdf", true) != UI_MSGBOX_YES) return;
    for (int c = 0; c < kPalCustom; c++) gPalCustom[c] = CLR_INVALID;
    // a default that named a cleared slot would draw the fallback colour anyway,
    // so point the defaults at the preset for their kind from the start
    if (gHlDefault >= kPalPreset) gHlDefault = 0;
    if (gPinDefault >= kPalPreset) gPinDefault = kPalPreset - 1;
    for (auto& h : gHls) if (h.color >= kPalPreset) h.color = gHlDefault;
    for (auto& p : gPins) if (p.color >= kPalPreset) p.color = gPinDefault;
    gPalNext = 0;
    writeAppPref();
    writeSidecarNow();
    renderPage();
}

// the one quit path for every in-app exit: the WM close runs this through the
// delete-event, and the menu Quit, Esc and the last tab's close must reach the
// SAME save logic, or a quit with autosave off drops unsaved marks. Written out
// as gtk_widget_destroy three times before it lived here.
static void requestQuit() {
    if (!onWindowDelete(nullptr, nullptr, nullptr)) gtk_widget_destroy(gWin);
}

static void onWindowConfigure(GtkWidget*, GdkEventConfigure* ev, gpointer) {
    gWinX = ev->x; gWinY = ev->y; gWinW = ev->width; gWinH = ev->height;
}

static void onWindowState(GtkWidget*, GdkEventWindowState* ev, gpointer) {
    gWinMax = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
}

// The saved frame comes back where the last session left it, bounded the way the
// Windows reader bounds it (src/main.cpp, clampBox and MonitorFromRect): a rect
// no monitor can see any more falls back to the default placement, and one that
// only partly fits is moved inside the landing monitor's work area with its size
// capped to that same area, so the caption strip and a resize border stay
// reachable.
static int clampBox(int pos, int size, int lo, int hi) {
    if (pos < lo) pos = lo;
    else if (pos + size > hi) pos = hi - size;
    if (pos < lo) pos = lo;
    return pos;
}

// The landing monitor is picked against the WORK area - the same area the frame
// is clamped into - so a saved rect cannot be shifted by a panel overlap the
// pick ignored. The size is honored only up to what that area shows (Windows
// caps a created window the same way), which also bounds the unsaved default.
static void restoreWindowGeometry() {
    GdkDisplay* dpy = gdk_display_get_default();
    GdkMonitor* land = nullptr;
    int best = 0;
    if (!(gWinX == -1 && gWinY == -1)) {
        // -1,-1 is the unsaved default loadAppPref leaves alone; any other pair
        // is a position the user actually left the window at, negative ones
        // included (a monitor can sit left of or above the primary).
        int n = dpy ? gdk_display_get_n_monitors(dpy) : 0;
        for (int i = 0; i < n; i++) {
            GdkMonitor* m = gdk_display_get_monitor(dpy, i);
            if (!m) continue;
            GdkRectangle wa;
            gdk_monitor_get_workarea(m, &wa);
            int l = gWinX > wa.x ? gWinX : wa.x;
            int t = gWinY > wa.y ? gWinY : wa.y;
            int r = (gWinX + gWinW < wa.x + wa.width) ? gWinX + gWinW : wa.x + wa.width;
            int b = (gWinY + gWinH < wa.y + wa.height) ? gWinY + gWinH : wa.y + wa.height;
            int area = (r > l && b > t) ? (r - l) * (b - t) : 0;
            if (area > best) { best = area; land = m; }
        }
    }
    GdkRectangle wa = { 0, 0, 0, 0 };
    if (!land && dpy) land = gdk_display_get_monitor(dpy, 0);
    if (land) {
        gdk_monitor_get_workarea(land, &wa);
        if (wa.width > 0 && gWinW > wa.width) gWinW = wa.width;
        if (wa.height > 0 && gWinH > wa.height) gWinH = wa.height;
    }
    gtk_window_set_default_size(GTK_WINDOW(gWin), gWinW > 0 ? gWinW : 1100, gWinH > 0 ? gWinH : 800);
    if (best > 0) gtk_window_move(GTK_WINDOW(gWin), clampBox(gWinX, gWinW, wa.x, wa.x + wa.width),
                                               clampBox(gWinY, gWinH, wa.y, wa.y + wa.height));
    if (gWinMax) gtk_window_maximize(GTK_WINDOW(gWin));
}

static void onOpenActivate(GtkMenuItem*, gpointer) {
    std::string f = ui_open_file();
    if (!f.empty()) openPath(utf8ToWide(f));
}

static void installMenuBar(GtkWidget* box) {
    GtkWidget* bar = gtk_menu_bar_new();
    GtkWidget* file = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget* fileMenu = gtk_menu_new();
    auto mi = [](const char* label, GCallback cb, gpointer data) {
        GtkWidget* it = gtk_menu_item_new_with_mnemonic(label);
        g_signal_connect(it, "activate", cb, data);
        return it;
    };
    GtkWidget* open = mi("_Open...", G_CALLBACK(onOpenActivate), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Save", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(118)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("Save _as...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(119)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Print...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(137)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Quit", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(112)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);

    GtkWidget* view = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget* viewMenu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Find...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(103)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Fit width", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(104)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _in", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(105)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _out", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(106)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Rotate clockwise", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(133)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Rotate c_ounter-clockwise", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(134)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), viewMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view);

    // "Advanced" is the Windows cascade (src/main.cpp:5449): everything past
    // plain reading lives here, and the ids are the Windows ones so the same
    // command means the same thing on both platforms.
    GtkWidget* adv = gtk_menu_item_new_with_mnemonic("_Advanced");
    GtkWidget* advMenu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("_Thumbnails", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(202)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("_Outline panel", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(201)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi(gNight ? "Night mode ✓" : "Night mode", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(200)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("_Merge PDFs...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(205)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("Split _pages...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(206)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("Insert _signature...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(203)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("Clear si_gnatures", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(204)));
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), mi("Reopen last document", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(207)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(adv), advMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), adv);

    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
}

// print: render each page clean (no annotations) and hand it to GtkPrintOperation.
// The one transient is a single page raster, bounded by the same pixel budget the
// Windows print path uses (src/main.cpp:2068): a page larger than the budget is
// shrunk to fit it, never refused, so it still prints.
static const double kPrintPixelBudget = 16.0 * 1000.0 * 1000.0;

static void printDrawPage(GtkPrintOperation* op, GtkPrintContext* ctx, int pageNr, gpointer data) {
    bool* failed = (bool*)data;
    cairo_t* cr = gtk_print_context_get_cairo_context(ctx);
    double pw = gtk_print_context_get_width(ctx);
    double ph = gtk_print_context_get_height(ctx);
    // a zero-area target would paint nothing and still report success, so it fails
    // the job like an unallocatable raster does (Windows bounds the same case with
    // resX > 0 ? resX : 1, src/main.cpp:2215)
    if (pw <= 0 || ph <= 0) {
        *failed = true;
        gtk_print_operation_cancel(op);
        return;
    }
    // The source box comes from the page's own size at a fixed 300 dpi, never
    // from the printer's device resolution: at 300 dpi a 2481x3508 A4 raster is
    // 1:1, at 600 dpi it is a 2x upscale, while sizing from the device would ask
    // for ~139 MP and ~540 MP. Same rule and same numbers as src/main.cpp:2248.
    int w = (int)(rotWpt(pageNr) * 300.0 / 72.0 + 0.5);
    int h = (int)(rotHpt(pageNr) * 300.0 / 72.0 + 0.5);
    double px = (double)w * h;
    if (px > kPrintPixelBudget) {
        double shrink = sqrt(kPrintPixelBudget / px);
        w = (int)(w * shrink); h = (int)(h * shrink);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    std::vector<unsigned char> buf;
    int stride = 0;
    // a page that could not be rasterized aborts the job instead of emitting a
    // blank sheet, so the print subsystem reports it as the Windows AbortDoc path
    if (!renderPageToBuffer(pageNr, w, h, buf, &stride)) {
        *failed = true;
        gtk_print_operation_cancel(op);
        return;
    }
    cairo_surface_t* s = cairo_image_surface_create_for_data(buf.data(), CAIRO_FORMAT_ARGB32, w, h, stride);
    // fit inside the printable area, preserve the page's own aspect, centre it:
    // a landscape page on portrait paper letterboxes rather than clipping
    double scale = pw / w;
    if (ph / h < scale) scale = ph / h;
    double dw = w * scale, dh = h * scale;
    cairo_save(cr);
    cairo_translate(cr, (pw - dw) / 2.0, (ph - dh) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(s);
}

static void doPrint() {
    if (!gDoc) return;
    GtkPrintOperation* op = gtk_print_operation_new();
    // ask GTK for the print area in PostScript points, the unit rotWpt/rotHpt and
    // cairo both work in, so the geometry above never sees device pixels
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_n_pages(op, gDocPages);
    bool printFailed = false;
    g_signal_connect(op, "draw-page", G_CALLBACK(printDrawPage), &printFailed);
    GtkPrintOperationResult res = gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, GTK_WINDOW(gWin), nullptr);
    if (res == GTK_PRINT_OPERATION_RESULT_ERROR) {
        GError* err = nullptr;
        gtk_print_operation_get_error(op, &err);
        if (err) g_error_free(err);
    }
    g_object_unref(op);
    // a cancelled job returns CANCEL, not ERROR, so the stopped print is told here
    if (printFailed) ui_message("A page could not be printed, so the job was stopped.", "mnpdf", false);
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    FPDF_InitLibrary();

    // Claim the shared-preference role before anything reads the preferences:
    // a secondary window must not write app.txt or last.txt, or a second
    // reader would overwrite the first one's settings. The Windows build does
    // the same with a named mutex (src/main.cpp:6138).
    gPrimaryInstance = claimPrimaryInstance();

    loadAppPref();
    gDocs[0] = new Doc();

    gWin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    // the saved caption state is applied here, the same way the tab strip's is:
    // a reader who hid the titlebar last session gets a captionless window
    // again, on either platform, because both read the same titlebar= key
    gtk_window_set_decorated(GTK_WINDOW(gWin), gTitlebar);
    restoreWindowGeometry();               // size, position and maximize from app.txt
    gtk_window_set_title(GTK_WINDOW(gWin), "mnpdf");
    gtk_window_set_icon_name(GTK_WINDOW(gWin), "application-pdf");
    g_signal_connect(gWin, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(gWin, "delete-event", G_CALLBACK(onWindowDelete), nullptr);
    g_signal_connect(gWin, "configure-event", G_CALLBACK(onWindowConfigure), nullptr);
    g_signal_connect(gWin, "window-state-event", G_CALLBACK(onWindowState), nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(gWin), vbox);
    installMenuBar(vbox);

    // The drawer strip flanks the reader: [thumbnails][outline][overlay]. The
    // drawers are created and destroyed by their toggles, and the overlay keeps
    // the search bar floating over the page as before.
    gDrawerBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gDrawerBox, TRUE, TRUE, 0);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(gDrawerBox), overlay, TRUE, TRUE, 0);
    gNotebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(gNotebook), TRUE);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(gNotebook), gTabStrip);
    g_signal_connect(gNotebook, "switch-page", G_CALLBACK(onNotebookSwitch), nullptr);
    gtk_container_add(GTK_CONTAINER(overlay), gNotebook);
    buildSearchBar(overlay);

    GtkWidget* area = nullptr;
    makeArea(&area);
    gtk_notebook_append_page(GTK_NOTEBOOK(gNotebook), area, gtk_label_new("New tab"));
    gCurArea = area;
    gActiveDoc = 0;

    gIbeam = gdk_cursor_new_for_display(gdk_display_get_default(), GDK_XTERM);
    gArrow = gdk_cursor_new_for_display(gdk_display_get_default(), GDK_LEFT_PTR);

    gtk_widget_show_all(gWin);
    // the overlay children must be re-hidden AFTER show_all: show_all walks every
    // widget including the overlay's, which would leave the find bar and the
    // open-on-first-use drawers visible before they are asked for.
    if (gSearchBar && !gSearchOpen) gtk_widget_hide(gSearchBar);

    int scale = gtk_widget_get_scale_factor(gWin);
    gScale = scale > 0 ? scale : 1.0;

    // open the file named on the command line, else the remembered last document
    std::wstring toOpen;
    if (argc > 1) toOpen = utf8ToWide(argv[1]);
    else toOpen = lastDocumentPath();
    if (!toOpen.empty()) openPath(toOpen);
    else renderPage();

    gSaveTimer = g_timeout_add(800, onSaveTick, nullptr);
    gtk_widget_grab_focus(gCurArea);
    // The shot has to wait for the window to be mapped AND for the drawers a
    // command opened to be allocated: an idle callback fires before the first
    // size negotiation, so a drawer created by MNPDF_CMD would be captured at
    // zero width. A short timeout runs after the layout pass instead.
    // The seams are independent: MNPDF_CMD drives a command, MNPDF_SHOT
    // writes a picture, MNPDF_SHOT_EXIT quits, MNPDF_MENU_DUMP prints the menu.
    // Registering only for MNPDF_SHOT (as this did) meant a run that asked for
    // a command and a quit but no picture did neither, which is how a broken
    // seam hides; MNPDF_MENU_DUMP was missing for the same reason and made the
    // dump silently print nothing.
    if (getenv("MNPDF_SHOT") || getenv("MNPDF_SHOT_EXIT") || getenv("MNPDF_CMD")
        || getenv("MNPDF_MENU_DUMP"))
        g_timeout_add(getenv("MNPDF_CMD") ? 400 : 0, onShotIdle, nullptr);
    gtk_main();

    if (gSaveTimer) g_source_remove(gSaveTimer);
    writeAllSidecars();
    writeAppPref();
    flushPageCache();
    for (int i = 0; i < gDocCount; i++) {
        if (gDocs[i]) { if (gDocs[i]->doc) FPDF_CloseDocument(gDocs[i]->doc); delete gDocs[i]; }
    }
    if (gIbeam) g_object_unref(gIbeam);
    if (gArrow) g_object_unref(gArrow);
    // the shared-preference role is released here; the kernel would release it
    // anyway when the process ends, but doing it explicitly keeps the window
    // from looking primary while it is shutting down
    releasePrimaryInstance();
    FPDF_DestroyLibrary();
    return 0;
}
