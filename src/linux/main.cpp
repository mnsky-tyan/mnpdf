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
void refreshDrawers() {}

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
        yesno ? GTK_BUTTONS_YES_NO : GTK_BUTTONS_OK,
        "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(d), caption.c_str());
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return (r == GTK_RESPONSE_YES) ? UI_MSGBOX_YES : UI_MSGBOX_NO;
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
    *maxed = (gWinMax || gtk_window_is_maximized(GTK_WINDOW(gWin))) ? 1 : 0;
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
    case 113: doSave(); break;
    case 114: doSaveAs(); break;
    case 115: undoOp(); break;
    case 116: redoOp(); break;
    case 118: deletePage(gPageIndex); break;
    case 119: if (gPageIndex > 0) { movePage(gPageIndex, gPageIndex - 1); } break;
    case 120: if (gPageIndex + 1 < gDocPages) { movePage(gPageIndex, gPageIndex + 1); } break;
    case 130: rotatePage(gPageIndex, +1); break;
    case 131: rotatePage(gPageIndex, -1); break;
    case 200: gNight = !gNight; writeAppPref(); renderPage(); break;
    case 202: toggleSearch(true); break;
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
    case 400: {   // highlight the current selection with the default colour
        applyHighlightFromSelection(gHlDefault);
        break;
    }
    case 401: {   // highlight with a chosen palette entry
        int idx = -1;
        if (ui_color_box(idx) && idx >= 0) applyHighlightFromSelection(idx);
        break;
    }
    case 402: {   // add pin here (point captured when the menu opened)
        if (gDoc && gMenuPinPage >= 0 && gMenuPinPage < gDocPages) {
            int idx = (int)gPins.size();
            addPinAt(gMenuPinPage, gMenuPinX, gMenuPinY);
            openNoteEditor(idx, true);
        }
        break;
    }
    case 410: if (gMenuPinIdx >= 0) openNoteEditor(gMenuPinIdx, false); break;
    case 411: deletePin(gMenuPinIdx); break;
    case 412: deleteHighlight(gMenuHlIdx); break;
    case 700: doPrint(); break;
    }
}

static void onContextMenu(GtkWidget* w, GdkEventButton* ev) {
    GtkWidget* menu = gtk_menu_new();
    int x = (int)ev->x, y = (int)ev->y;
    int pg;
    int ci = charIndexAtDoc(x, y, &pg);
    gMenuHlIdx = hlAt(pg, ci);
    gMenuPinIdx = pinAt(x, y);
    gMenuPinPage = -1;

    auto add = [&](const char* label, int id) {
        GtkWidget* it = gtk_menu_item_new_with_label(label);
        g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    };
    auto sep = [&]() { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };

    if (gMenuPinIdx >= 0) {
        add("Edit note", 410);
        add("Delete pin", 411);
        sep();
    } else if (gMenuHlIdx >= 0) {
        add("Copy text", 101);
        add("Delete highlight", 412);
        sep();
    } else if (hasSelection()) {
        add("Highlight", 400);
        add("Highlight colour...", 401);
        sep();
    } else if (gDoc && pg >= 0) {
        double px, py;
        pagePxToPt(pg, x - docLeft() - pageLeftPx(pg), y - docTop() - yTopPx(pg), &px, &py);
        gMenuPinPage = pg; gMenuPinX = px; gMenuPinY = py;
        add("Add pin here", 402);
        sep();
    }
    add("Open...", 107);
    add("Open in new tab...", 108);
    add("New tab", 300);
    add("Close tab", 301);
    sep();
    add("Save", 113);
    add("Save as...", 114);
    sep();
    add("Undo", 115);
    add("Redo", 116);
    sep();
    add("Find...", 103);
    sep();
    add("Fit width", 104);
    add("Zoom in", 105);
    add("Zoom out", 106);
    sep();
    add("Rotate clockwise", 130);
    add("Rotate counter-clockwise", 131);
    sep();
    add("Delete this page", 118);
    add("Move page up", 119);
    add("Move page down", 120);
    sep();
    add(gNight ? "Day mode" : "Night mode", 200);
    add("Insert signature...", 203);
    add("Clear signatures", 204);
    add("Merge PDFs...", 205);
    add("Split pages...", 206);
    sep();
    add(gAutosave ? "Autosave: on" : "Autosave: off", 109);
    add(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(gNotebook)) ? "Hide tab strip" : "Show tab strip", 302);
    sep();
    add("Minimize", 110);
    add("Maximize / restore", 111);
    add("Quit", 112);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
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
    double dy = (ev->direction == GDK_SCROLL_UP) ? -1 : (ev->direction == GDK_SCROLL_DOWN) ? 1 : 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) dy = -(ev->delta_y + ev->delta_x);   // Wayland: deltas
    if (dy == 0) return;
    // smooth deltas arrive in small steps, so accumulate until a step is worth
    // acting on; a discrete UP/DOWN event is already a whole step
    static double sSmoothAcc = 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        sSmoothAcc += dy;
        if (fabs(sSmoothAcc) < 1.0) return;
        dy = sSmoothAcc > 0 ? 1 : -1;
        sSmoothAcc = 0;
    }
    if (ctrl) {
        zoomAt(dy < 0 ? 1.1 : 1 / 1.1, (int)ev->x, (int)ev->y);
    } else {
        scrollByDy((int)(dy * 3 * 40));
        markSave();
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
    const char* path = getenv("MNPDF_SHOT");
    if (path && *path && ui_backbuf_bits() && ui_backbuf_w() > 0) {
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
    renderPage();
}

static gboolean onWindowDelete(GtkWidget*, GdkEvent*, gpointer) {
    settleActiveDoc();
    if (gAutosave) writeAllSidecars();
    else if (gDirty) {
        int r = ui_message("Save changes?", "mnpdf", true);
        if (r == UI_MSGBOX_YES && !doSave()) return TRUE;
        writeAllSidecars();
    } else writeAllSidecars();
    writeAppPref();
    return FALSE;
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
// only partly fits keeps its width and height with its top-left moved inside
// that monitor's work area, so the caption strip and a resize border stay
// reachable.
static int clampBox(int pos, int size, int lo, int hi) {
    if (pos < lo) pos = lo;
    else if (pos + size > hi) pos = hi - size;
    if (pos < lo) pos = lo;
    return pos;
}

static void restoreWindowGeometry() {
    // -1,-1 is the unsaved default loadAppPref leaves alone; any other pair is a
    // position the user actually left the window at, negative ones included
    // (a monitor can sit left of or above the primary).
    if (!(gWinX == -1 && gWinY == -1)) {
        GdkDisplay* dpy = gdk_display_get_default();
        GdkMonitor* land = nullptr;
        int best = 0;
        int n = dpy ? gdk_display_get_n_monitors(dpy) : 0;
        for (int i = 0; i < n; i++) {
            GdkMonitor* m = gdk_display_get_monitor(dpy, i);
            if (!m) continue;
            GdkRectangle g;
            gdk_monitor_get_geometry(m, &g);
            int l = gWinX > g.x ? gWinX : g.x;
            int t = gWinY > g.y ? gWinY : g.y;
            int r = (gWinX + gWinW < g.x + g.width) ? gWinX + gWinW : g.x + g.width;
            int b = (gWinY + gWinH < g.y + g.height) ? gWinY + gWinH : g.y + g.height;
            int area = (r > l && b > t) ? (r - l) * (b - t) : 0;
            if (area > best) { best = area; land = m; }
        }
        if (land) {                              // off every monitor: default placement
            GdkRectangle wa;
            gdk_monitor_get_workarea(land, &wa);
            gtk_window_move(GTK_WINDOW(gWin), clampBox(gWinX, gWinW, wa.x, wa.x + wa.width),
                                               clampBox(gWinY, gWinH, wa.y, wa.y + wa.height));
        }
    }
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
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Save", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(113)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("Save _as...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(114)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Print...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(700)));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Quit", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(112)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);

    GtkWidget* view = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget* viewMenu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Find...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(103)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Fit width", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(104)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _in", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(105)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _out", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(106)));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Night mode", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(200)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), viewMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view);

    GtkWidget* tools = gtk_menu_item_new_with_mnemonic("_Tools");
    GtkWidget* toolsMenu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(toolsMenu), mi("_Merge PDFs...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(205)));
    gtk_menu_shell_append(GTK_MENU_SHELL(toolsMenu), mi("_Split pages...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(206)));
    gtk_menu_shell_append(GTK_MENU_SHELL(toolsMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(toolsMenu), mi("Insert _signature...", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(203)));
    gtk_menu_shell_append(GTK_MENU_SHELL(toolsMenu), mi("_Clear signatures", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(204)));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tools), toolsMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), tools);

    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
}

// print: render each page clean (no annotations) and hand it to GtkPrintOperation
static void printDrawPage(GtkPrintOperation*, GtkPrintContext* ctx, int pageNr, gpointer) {
    cairo_t* cr = gtk_print_context_get_cairo_context(ctx);
    double pw = gtk_print_context_get_width(ctx);
    double ph = gtk_print_context_get_height(ctx);
    int w = (int)(pw * 2), h = (int)(ph * 2);
    std::vector<unsigned char> buf;
    int stride = 0;
    if (!renderPageToBuffer(pageNr, w, h, buf, &stride)) return;
    cairo_surface_t* s = cairo_image_surface_create_for_data(buf.data(), CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_scale(cr, pw / w, ph / h);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(s);
}

static void doPrint() {
    if (!gDoc) return;
    GtkPrintOperation* op = gtk_print_operation_new();
    gtk_print_operation_set_n_pages(op, gDocPages);
    g_signal_connect(op, "draw-page", G_CALLBACK(printDrawPage), nullptr);
    GtkPrintOperationResult res = gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, GTK_WINDOW(gWin), nullptr);
    if (res == GTK_PRINT_OPERATION_RESULT_ERROR) {
        GError* err = nullptr;
        gtk_print_operation_get_error(op, &err);
        if (err) g_error_free(err);
    }
    g_object_unref(op);
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    FPDF_InitLibrary();

    loadAppPref();
    gDocs[0] = new Doc();

    gWin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(gWin), gWinW > 0 ? gWinW : 1100, gWinH > 0 ? gWinH : 800);
    restoreWindowGeometry();
    gtk_window_set_title(GTK_WINDOW(gWin), "mnpdf");
    gtk_window_set_icon_name(GTK_WINDOW(gWin), "application-pdf");
    g_signal_connect(gWin, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(gWin, "delete-event", G_CALLBACK(onWindowDelete), nullptr);
    g_signal_connect(gWin, "configure-event", G_CALLBACK(onWindowConfigure), nullptr);
    g_signal_connect(gWin, "window-state-event", G_CALLBACK(onWindowState), nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(gWin), vbox);
    installMenuBar(vbox);

    GtkWidget* overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(vbox), overlay, TRUE, TRUE, 0);
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
    if (getenv("MNPDF_SHOT")) g_idle_add(onShotIdle, nullptr);
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
    FPDF_DestroyLibrary();
    return 0;
}
