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
#include "update.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#if defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>          // physicalDpi(): the X screen's millimetre size
#include <X11/Xlib.h>
#endif
#include <cairo.h>
#include <limits.h>          // PATH_MAX, for the /proc/self/exe icon lookup
#include <unistd.h>          // readlink(), same lookup
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
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

// ---- platform state ----
static GtkWidget* gWin = nullptr;
static GtkWidget* gMenuBar = nullptr;   // the menu bar, so its stateful labels can be refreshed
// The app's own main loop. gtk_main_quit() leaves the INNERMOST running loop,
// so it cannot end the outer one while a modal's nested loop is up - the
// window goes, the nested loop ends, and the outer one keeps running with
// nothing to click. A GMainLoop of the app's own is ended by name, so exactly
// that loop goes, whatever else is running.
static GMainLoop* gAppLoop = nullptr;
static bool gAppLoopRunning = false;
static std::atomic<bool> gUpdateCheckRunning{false};   // a check is in flight on the worker thread
// A click that arrived while a check was in flight. The Windows build keeps the
// same flag (gManualUpdateRequested, src/main.cpp:92): the click is recorded
// before the in-flight test, and the worker's tail takes it, so a click during a
// silent launch check is answered rather than dropped (src/main.cpp:3183, 3201).
static std::atomic<bool> gUpdateCheckWanted{false};
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

// ---- dialogs ---------------------------------------------------------------
// A modal dialog is destroyed with its parent, so a window that goes down
// while the modal is up leaves the dialog pointer dangling, and destroying it
// a second time is a use-after-free - GTK answers that with "assertion
// 'GTK_IS_WIDGET (widget)' failed". The update check's answer can cause
// exactly that: the modal opens from a callback, and the quit path tears the
// window down while it is up. The widget's own destroy signal records that it
// is gone already, so the second destroy is skipped.
//
// Measured: with the update check's answer on screen and the window closing
// under it, GTK logged the critical above, once per run.
struct Dialog {
    GtkWidget* w;
    bool gone = false;
    explicit Dialog(GtkWidget* widget) : w(widget) {
        g_signal_connect(w, "destroy", G_CALLBACK(onDestroyed), this);
    }
    ~Dialog() { if (!gone && w) gtk_widget_destroy(w); }
    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;
private:
    static void onDestroyed(GtkWidget*, gpointer data) {
        ((Dialog*)data)->gone = true;
    }
};

// ---- message box ----
int ui_message(const std::string& text, const std::string& caption, bool yesno) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(gWin),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        yesno ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO,
        GTK_BUTTONS_NONE, "%s", text.c_str());
    Dialog life(d);
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
    if (r == GTK_RESPONSE_YES) return UI_MSGBOX_YES;
    // Escape and the window-manager close are Cancel, not No: dismissing a
    // "Save changes?" must not quit, exactly as IDCANCEL on Windows
    if (r == GTK_RESPONSE_CANCEL || r == GTK_RESPONSE_DELETE_EVENT || r == GTK_RESPONSE_NONE)
        return UI_MSGBOX_CANCEL;
    return UI_MSGBOX_NO;
}

// ---- file pickers ----
static std::vector<std::string> runChooser(const char* title, GtkFileChooserAction act,
                                           bool save, const std::string& suggested,
                                           bool multi = false) {
    GtkWidget* d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(gWin), act,
        "_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(d), TRUE);
    if (act == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
        if (!suggested.empty())
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), suggested.c_str());
    }
    if (multi) gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d), TRUE);
    Dialog life(d);
    std::vector<std::string> out;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        if (multi) {
            GSList* l = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
            for (GSList* p = l; p; p = p->next) {
                if (p->data) out.push_back((const char*)p->data);
                g_free(p->data);
            }
            g_slist_free(l);
        } else {
            char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
            if (fn) { out.push_back(fn); g_free(fn); }
        }
    }
    return out;
}

std::string ui_open_file() {
    std::vector<std::string> f = runChooser("Open PDF", GTK_FILE_CHOOSER_ACTION_OPEN, false, "");
    return f.empty() ? std::string() : f.front();
}
std::string ui_save_file(const std::string& suggested) {
    std::string name = suggested;
    size_t s = name.find_last_of('/');
    if (s != std::string::npos) name = name.substr(s + 1);
    std::vector<std::string> f = runChooser("Save as", GTK_FILE_CHOOSER_ACTION_SAVE, true, name);
    return f.empty() ? std::string() : f.front();
}
std::string ui_pick_jpeg() {
    GtkWidget* d = gtk_file_chooser_dialog_new("Choose a signature image", GTK_WINDOW(gWin),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(d), TRUE);
    Dialog life(d);
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
    return out;
}

bool ui_prompt(const std::string& label, std::string& value) {
    GtkWidget* d = gtk_dialog_new_with_buttons("mnpdf", GTK_WINDOW(gWin),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, nullptr);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label.c_str()), FALSE, FALSE, 6);
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), value.c_str());
    gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 6);
    gtk_widget_show_all(d);
    Dialog life(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    if (ok) value = gtk_entry_get_text(GTK_ENTRY(e));
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
// The Windows colour entry takes the same three forms: "#a1b2c3", "a1b2c3"
// and "A1B2C3" all mean the same colour (src/main.cpp, parseHexColor), and a
// colour that is already in one of the reader's own slots lands in THAT slot
// instead of taking a new one (src/main.cpp, applyCustomColor).
static bool parseHexColor(const std::string& in, COLORREF& out) {
    unsigned int v = 0;
    int digits = 0;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '#' || c == ' ') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (unsigned int)d;
        if (++digits > 6) return false;
    }
    if (digits != 6) return false;
    out = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    return true;
}

bool ui_color_box(int& paletteIndex) {
    // the '#' is already in the box, the way the Windows box has it, so the
    // reader is told the shape of the answer
    std::string v = "#";
    if (!ui_prompt("Custom colour (six hex digits, with or without the #):", v)) return false;
    COLORREF c;
    if (parseHexColor(v, c)) {
        int slot = -1;
        for (int k = 0; k < kPalCustom; k++)
            if (gPalCustom[k] == c) { slot = k; break; }
        if (slot < 0) {                       // a new colour: round-robin the slots
            slot = gPalNext;
            gPalNext = (gPalNext + 1) % kPalCustom;
        }
        gPalCustom[slot] = c;
        paletteIndex = kPalPreset + slot;
        writeAppPref();
        return true;
    }
    ui_message("\"" + v + "\" is not a colour.\n\nType six hex digits, with or without the #:\n\n"
               "    #ff4d00\n    ff4d00\n    FF4D00", "mnpdf", false);
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

// Every entry that posts a command records the id it posts, so the menu dump
// sees the whole command set: the context menu, the colour submenus, the
// Advanced cascade and the menu bar. An entry that only holds a submenu posts
// nothing and carries no id.
static GtkWidget* menuCommand(const char* label, int id) {
    GtkWidget* it = gtk_menu_item_new_with_label(label);
    g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
    g_object_set_data(G_OBJECT(it), "cmd-id", GINT_TO_POINTER(id));
    return it;
}

static GtkWidget* buildColorSubmenu(int current, int presetBase, int customBase, int customId) {
    GtkWidget* sub = gtk_menu_new();
    auto entry = [&](const char* label, int id) {
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), menuCommand(label, id));
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
// (src/main.cpp:3020). The Linux check takes the release list from the
// release feed, confirms the Linux archive by asking for its download URL,
// and compares the tag with this build's version; the report is a dialog too,
// though with no download - the reader is told, not installed.
//
// The request runs on its own thread, like the Windows one: a network that
// takes its time must not freeze the window. GTK is not thread-safe, so the
// result is handed back to the main loop with g_idle_add.
struct UpdateOutcome {
    bool ok = false;
    // a request was really made, rather than the answer being taken from what
    // the last attempt learned: only a real request stamps the cooldown
    bool manual = true;           // a click asks; the launch check only reports news
    bool requested = false;
    std::string latestTag;
    bool newer = false;
    std::string error;
    std::string currentTag;      // the release GitHub calls current
    bool currentIsNewer = false;
};

// The window is gone: a late answer must not build a dialog on a window that
// is already destroyed. The Windows build keeps the same flag for the same
// reason (gShuttingDown, src/main.cpp:93 and 3149).
static std::atomic<bool> gShuttingDown = false;

// the check itself, declared here because the reply handler above re-runs the
// command when a click arrived while the check was in flight
static void runUpdateCheck(bool manual);

static gboolean showUpdateResult(gpointer data) {
    UpdateOutcome* o = (UpdateOutcome*)data;
    gUpdateCheckRunning = false;
    // The outcome goes to stdout as well as to a dialog. The dialog is the
    // reader's answer; stdout is the suite's, because a modal is a GTK window a
    // headless run can neither read nor dismiss. It is printed before the
    // silent filter below, so a launch check that found nothing - or could not
    // reach the host - still leaves the outcome it reached behind. The error
    // text is printed too, because it is what tells the three answers apart:
    // "could not reach GitHub" is a host that did not answer, and "No release
    // with a Linux build was found" is a host that answered and has none. A
    // suite that conflated the two would blame the code for the network.
    if (o->ok) {
        printf("update check: outcome %s %s\n", o->newer ? "newer" : "current",
               o->latestTag.c_str());
    } else {
        printf("update check: outcome error\nupdate check: error %s\n",
               o->error.empty() ? "the check failed" : o->error.c_str());
    }
    fflush(stdout);
    if (gShuttingDown) { delete o; return G_SOURCE_REMOVE; }
    std::string current = gAppVersion;
    // The attempt is stamped here, on the GTK main thread, not on the worker
    // thread that made the request: writeAppPref writes the shared app.txt,
    // and every other writer of it runs on this thread. The Windows build
    // splits it the same way - the worker posts its result and the window
    // procedure stamps the attempt (src/main.cpp:6085). A failed attempt
    // counts too, so a check that could not reach the host does not turn into
    // a request per click.
    //
    // Only an attempt that really asked counts. An answer taken from the
    // cache spends no request, so it must not push the cooldown's end away -
    // otherwise a reader who clicks once an hour never asks again and keeps
    // being told whatever the first attempt saw.
    if (o->requested) noteUpdateCheck(o->ok ? o->latestTag : std::string());
    // A click that arrived while this check ran is answered by running the
    // command again, not by recursing from here: the cooldown decision and its
    // dialog stay on the UI thread, the way the Windows result handler posts
    // WM_COMMAND 170 rather than calling into the check (src/main.cpp:6089).
    // The worker already took any click that was pending when it finished (that
    // is what `o->manual` carries), so what is left here is a click that
    // arrived after that - it must not be dropped.
    if (!gShuttingDown && gUpdateCheckWanted.exchange(false)) {
        g_timeout_add(0, +[](gpointer) -> gboolean { runUpdateCheck(true); return G_SOURCE_REMOVE; },
                      nullptr);
    }
    // The silent launch check only speaks when there is a newer release: a
    // check with nothing to say, or one that could not reach the host, says
    // nothing at all, the two ways the Windows result handler is silent
    // (src/main.cpp:3129-3130). A clicked check always answers - the click
    // is the reader asking.
    if (!o->manual && !o->newer) { delete o; return G_SOURCE_REMOVE; }
    if (!o->ok) {
        ui_message(o->error.empty() ? "The update check failed." : o->error, "mnpdf", false);
    } else if (o->newer) {
        std::string msg = "mnpdf " + o->latestTag + " is available.\n\nYou are running " + current + ".";
        // The release GitHub calls current is a different release from the
        // newest one with a Linux archive: a Linux-only release is a
        // prerelease until its Windows archive ships, and /releases/latest
        // skips prereleases. When the two differ, the reader is told about
        // both rather than left to wonder why the current release is not the
        // one on offer.
        if (o->currentIsNewer && o->currentTag != o->latestTag) {
            msg += "\n\nGitHub's current release is " + o->currentTag + ".";
        }
        ui_message(msg, "mnpdf update available", false);
    } else {
        // "Newest" is the newest release carrying a Linux archive, and that is
        // not always the release GitHub calls current: a Linux-only release is
        // a prerelease until its Windows archive ships, and /releases/latest
        // skips prereleases. So a reader who is behind the current release is
        // told about it here too - and must not be called the newest, which
        // would contradict the very next sentence.
        bool behindCurrent = o->currentIsNewer && o->currentTag != o->latestTag;
        std::string msg = behindCurrent
            ? "You are running " + current + ". No newer release with a Linux build was found."
            : "You are running the newest version (" + current + ").";
        if (behindCurrent) {
            msg += "\n\nGitHub's current release is " + o->currentTag + ".";
        }
        ui_message(msg, "mnpdf update", false);
    }
    delete o;
    return G_SOURCE_REMOVE;
}

static void runUpdateCheck(bool manual) {
    // A click is recorded BEFORE the in-flight test below, so a click that
    // arrives while a check is running is not lost: the check in flight answers
    // it, and the reply handler runs the command again for anything that
    // arrived after it. The Windows build sets its own flag in the same place
    // for the same reason (src/main.cpp:3183 before :3188).
    if (manual) gUpdateCheckWanted = true;
    // One request at a time. The Windows build gates the click on the UI
    // thread before it spawns anything (src/main.cpp:3188), so a second
    // click inside the first request's own window makes one request, not two.
    if (gUpdateCheckRunning.exchange(true)) return;
    // Nothing is in flight now, so a click still recorded is this check's to
    // answer. Consuming it here matters for the cooldown return below: a flag
    // left set there would be claimed by the NEXT check, which would then open
    // a dialog nobody asked for.
    if (gUpdateCheckWanted.exchange(false)) manual = true;
    // The silent launch check says nothing inside the cooldown: the Windows
    // build returns before it spawns anything, and the cached answer is only
    // surfaced by a click (src/main.cpp:3190-3195) - a launch that answered
    // from the cache would offer the same release at every start of a run
    // that already knows about it.
    if (!manual && updateCheckCooling()) { gUpdateCheckRunning = false; return; }
    std::thread([manual]() {
        UpdateOutcome* o = new UpdateOutcome();
        std::string latest, error, cur;
        bool newer = false, curNewer = false, requested = false;
        bool ok = updateCheckNow(latest, newer, error, cur, curNewer, requested);
        // The click this check answers: one that arrived while the request was
        // in flight is folded into THIS answer rather than costing a second
        // request, which is where the Windows worker takes its flag - at the
        // tail, after the request (src/main.cpp:3201). A click that lands after
        // this line is answered by the reply handler.
        bool clicked = gUpdateCheckWanted.exchange(false);
        o->manual = manual || clicked;
        if (ok) {
            o->ok = true;
            o->latestTag = latest;
            o->newer = newer;
            o->currentTag = cur;
            o->currentIsNewer = curNewer;
        } else {
            o->ok = false;
            o->error = error;
        }
        o->requested = requested;
        g_idle_add(showUpdateResult, o);
    }).detach();
}

// ---- note editor: a dialog with a text view, committed on OK ----
static void openNoteEditor(int idx, bool fresh) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    GtkWidget* d = gtk_dialog_new_with_buttons("Note", GTK_WINDOW(gWin),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
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
    Dialog life(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    std::wstring text;
    if (ok) {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(tb, &a, &b);
        char* t = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
        if (t) { text = utf8ToWide(t); g_free(t); }
        if ((int)text.size() > kNoteMaxChars) text.resize(kNoteMaxChars);
    }
    if (!ok) { if (fresh) commitPinText(idx, true, L""); return; }   // cancel a new pin: drop it
    commitPinText(idx, fresh, text);
}

// ---- tab lifecycle ----
static void requestQuit();
static void refreshMenuLabels();
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
    else {
        gClientW = gtk_widget_get_allocated_width(area);
        gClientH = gtk_widget_get_allocated_height(area);
        renderPage();
        // a new empty tab follows no document, so an open drawer must not keep
        // the previous tab's pages - the same place openPath refreshes it
        refreshDrawers();
    }
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
    // the drawers follow the document, and a closed tab leaves a different one
    // open: the thumbnail list and the outline are built from the active
    // document's pages and bookmarks
    refreshDrawers();
}

// ---- scrollbars ----
static const int kSbThick = 12;
// the strip's thickness in device pixels: sbRects, the thumb helpers, the drag
// mapping and the paint all read this one value, so the hit-test and the drawn
// band stay the same rectangle at any device scale
static int sbThick() { return designPx(kSbThick); }
static void sbRects(int* vx, int* vy, int* vw, int* vh, int* hx, int* hy, int* hw, int* hh) {
    int S = sbThick();
    int vhgt = gClientH - S;
    *vx = gClientW - S; *vy = 0; *vw = S; *vh = vhgt;
    *hx = 0; *hy = gClientH - S; *hw = gClientW - S; *hh = S;
}
static bool vThumb(int* ty, int* th) {
    int total = docHpx(), view = docViewH();
    if (total <= view || total <= 0) return false;
    int h = gClientH - sbThick();
    *th = (int)((double)view / total * h);
    if (*th < designPx(24)) *th = designPx(24);
    int maxScroll = total - view;
    *ty = (int)((double)gScrollY / maxScroll * (h - *th));
    return true;
}
static bool hThumb(int* tx, int* tw) {
    int total = docWpx() + designPx(2 * MARGIN), view = gClientW;
    if (total <= view || total <= 0) return false;
    int w = gClientW - sbThick();
    *tw = (int)((double)view / total * w);
    if (*tw < designPx(24)) *tw = designPx(24);
    int maxScroll = total - view;
    *tx = (int)((double)gScrollX / maxScroll * (w - *tw));
    return true;
}

// ---- device scale ----
// ui_scale() is "device pixels per design pixel". GTK only ever reports an
// INTEGER scale factor: the default desktop launch here is a native Wayland
// client, and KDE publishes wl_output scale 2 for its 1.6 desktop (measured with
// wayland-info), so the toolkit's own factor already renders the frame sharp -
// the compositor downsamples the 2x buffer onto the 1.6x panel. A backend with
// no useful factor still has a fractional source in the X screen's published
// DPI, which is what detectUiScale reaches for; MNPDF_SCALE overrides all of it.
// The screen's physical DPI, or 0 when nothing usable is published. On X11 the
// server's millimetre size is the honest measurement - except on XWayland,
// which reports a millimetre size that works out to exactly 96 dpi whatever the
// desktop scale is (measured here: 2880 px over 762 mm on the 1.6-scaled panel),
// so there the fraction lives in the desktop's Xft.dpi instead (153 here,
// 1.6 x 96, read through gdk_screen_get_resolution). The millimetre figure is
// therefore only trusted when it is not that default.
static double physicalDpi(GtkWidget* w) {
    if (!w) return 0;
    GdkScreen* scr = gtk_widget_get_screen(w);
    if (!scr) return 0;
    GdkDisplay* dpy = gdk_screen_get_display(scr);
    if (!dpy) return 0;
    double xdpi = 0;
    // X11 backend: DisplayWidthMM/DisplayHeightMM carry the panel's real size
    if (GDK_IS_X11_DISPLAY(dpy)) {
        Display* xd = GDK_DISPLAY_XDISPLAY(dpy);
        int px = DisplayWidth(xd, DefaultScreen(xd));
        int mm = DisplayWidthMM(xd, DefaultScreen(xd));
        if (mm > 0 && px > 0) {
            double dpi = px * 25.4 / mm;
            if (dpi >= 48 && dpi <= 480) xdpi = dpi;
        }
    }
    if (xdpi > 0 && fabs(xdpi - 96.0) >= 1.0) return xdpi;
    double dpi = gdk_screen_get_resolution(scr);
    if (dpi > 0) return dpi;
    return xdpi;
}

static double detectUiScale(GtkWidget* w) {
    // An explicit override wins: a fractional desktop scale is not always
    // discoverable, so MNPDF_SCALE=1.6 is the documented escape.
    if (const char* env = getenv("MNPDF_SCALE")) {
        // g_ascii_strtod, not atof: gtk_init has already run setlocale(LC_ALL),
        // and in a comma-decimal locale atof("1.6") reads 1.0 - which still
        // passes the range check and silently forces scale 1
        double v = g_ascii_strtod(env, nullptr);
        if (v >= 1.0 && v <= 4.0) return v;
    }
    double best = 0;
    GdkDisplay* dpy = gdk_display_get_default();
    // the integer scale factor the toolkit reports: right for a 1x or 2x
    // desktop, and rounded up on a fractional one (KDE's 1.6 publishes 2)
    if (dpy && w) {
        GdkWindow* gw = gtk_widget_get_window(w);
        GdkMonitor* mon = gw ? gdk_display_get_monitor_at_window(dpy, gw) : nullptr;
        if (mon) best = gdk_monitor_get_scale_factor(mon);
    }
    if (best <= 0) best = gtk_widget_get_scale_factor(w);
    if (best <= 0) best = 1.0;
    double dpi = physicalDpi(w);
    if (dpi > 0) {
        double frac = dpi / 96.0;
        // keep it sane: a bogus 72-dpi X default or a wild value must not resize
        // the whole UI. 1.0..3.0 covers every real desktop scale.
        if (frac >= 1.0 && frac <= 3.0 && frac > best) best = frac;
    }
    return best;
}

// ---- input coordinates ----
// Every handler below compares against geometry in DEVICE pixels, while GTK
// delivers pointer coordinates in logical widget pixels. Converting once, at the
// point the event is handled, keeps the hit-test code in one coordinate space.
static inline int devX(double x) { return (int)(x * ui_scale() + 0.5); }
static inline int devY(double y) { return (int)(y * ui_scale() + 0.5); }

// ---- drawing area ----
static gboolean onDraw(GtkWidget* w, cairo_t* cr, gpointer) {
    ui_paint(cr);
    (void)w;
    // Scrollbars painted over the frame. sbRects and the thumb helpers work in
    // device pixels; this context draws in the widget's logical ones, so the
    // band is the device geometry divided by the same ui_scale the buffer maps
    // with - the painted band and the hit-test stay one rectangle.
    double s = ui_scale() > 0 ? ui_scale() : 1.0;
    int S = sbThick();
    double bx = (gClientW - S) / s, by = (gClientH - S) / s;
    cairo_set_source_rgb(cr, 0.14, 0.14, 0.15);
    cairo_rectangle(cr, bx, 0, S / s, gClientH / s);
    cairo_rectangle(cr, 0, by, gClientW / s, S / s);
    cairo_fill(cr);
    int ty, th, tx, tw;
    if (vThumb(&ty, &th)) {
        cairo_set_source_rgb(cr, 0.42, 0.42, 0.45);
        cairo_rectangle(cr, bx + 2, ty / s, kSbThick - 4, th / s);
        cairo_fill(cr);
    }
    if (hThumb(&tx, &tw)) {
        cairo_set_source_rgb(cr, 0.42, 0.42, 0.45);
        cairo_rectangle(cr, tx / s, by + 2, tw / s, kSbThick - 4);
        cairo_fill(cr);
    }
    return FALSE;
}

static void onAreaSize(GtkWidget* w, GtkAllocation* a, gpointer) {
    // the allocation is logical; the frame is device pixels, so the buffer and
    // every geometry function work in the device space
    double s = ui_scale() > 0 ? ui_scale() : 1.0;
    gClientW = (int)(a->width * s + 0.5);
    gClientH = (int)(a->height * s + 0.5);
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
static int gMenuRotPage = -1;   // the page the context menu targets (rotate)
static double gMenuPinX = 0, gMenuPinY = 0;
static int gMenuHlIdx = -1;
static int gMenuPinIdx = -1;
// the drawer state the menu handler and the cascade builder need; the rest
// of the drawer state lives with the drawer code below
static GtkWidget* gThumbDrawer = nullptr;
static GtkWidget* gOutlineDrawer = nullptr;
int gThumbSel = -1;


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
    case 109:
        gAutosave = !gAutosave;
        writeAppPref();
        // the Windows toggle also re-arms or cancels the pending sidecar
        // write (src/main.cpp:5702-5711): a write already pending must not
        // fire after autosave was switched off, and anything that changed
        // while it was off deserves a flush when it comes back. The port's
        // tick runs for the whole session, so arming and cancelling is the
        // pending flag, not the timer.
        if (gAutosave) markSave();
        else sidecarClearPending();
        break;
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
    case 201: toggleOutlineDrawer(); refreshMenuLabels(); break;
    case 202: toggleThumbDrawer(); refreshMenuLabels(); break;
    case 207: reopenLastDocument(); break;
    case 118: doSave(); break;
    case 119: doSaveAs(); break;
    case 114: undoOp(); break;
    case 115: redoOp(); break;
    case 310: deletePage(gPageIndex); break;
    // a move re-points the drawer's ring at the page that moved
    // (src/main.cpp:4615-4620): the index it happens to hold now shows a
    // different page, so the next move would act on that one instead
    case 311: if (gPageIndex > 0) { movePage(gPageIndex, gPageIndex - 1); } break;
    case 312: if (gPageIndex + 1 < gDocPages) { movePage(gPageIndex, gPageIndex + 1); } break;
    case 133: { int rp = gMenuRotPage >= 0 ? gMenuRotPage : gPageIndex;
                rotatePage(rp, +1);
                // the capture is spent. Windows never carries it past the
                // context menu's own dispatch - its only rotate commands are
                // the popup's (src/main.cpp:5559), captured the instant that
                // menu is built - while this reader also has them in the menu
                // bar, where a stale capture would rotate a page the reader
                // is no longer looking at. The drawer refresh is the
                // primitive's own (rotatePage, doc.cpp:550), not this
                // handler's: a second call would rebuild every row height and
                // walk the whole bookmark tree again for nothing
                gMenuRotPage = -1; } break;
    case 134: { int rp = gMenuRotPage >= 0 ? gMenuRotPage : gPageIndex;
                rotatePage(rp, -1);
                gMenuRotPage = -1; } break;
    case 200: gNight = !gNight; writeAppPref(); renderPage(); refreshMenuLabels(); break;
    case 203: if (gDoc) pickSignatureImage(); break;
    case 204: clearSignatures(); refreshMenuLabels(); break;
    case 205: {
        if (!gDoc) break;
        // Windows' Merge PDFs dialog is multi-select and titled for the merge
        // (mergeDialog in src/main.cpp), and every picked file goes to
        // mergeFiles in one call
        std::vector<std::string> fs =
            runChooser("Add files to the end of this document",
                       GTK_FILE_CHOOSER_ACTION_OPEN, false, "", true);
        if (!fs.empty()) {
            std::vector<std::wstring> w;
            w.reserve(fs.size());
            for (const std::string& f : fs) w.push_back(utf8ToWide(f));
            mergeFiles(w);
        }
        break;
    }
    case 206: {
        if (!gDoc || gDocPages <= 1) break;
        std::string range = "";
        if (ui_prompt_range(range) && !range.empty()) {
            std::string out = ui_save_file("split.pdf");
            if (!out.empty()) splitExport(utf8ToWide(out), range, false);
        }
        break;
    }
    case 300: addTab(L""); break;
    case 301: closeTab(); break;
    case 302: {
        // The state belongs to the flag, not to the widget: the Windows
        // toggle flips gTabStrip (src/main.cpp:3434) and the flag is what
        // app.txt carries, so a window that hides the strip and quits comes
        // back with the strip hidden. Flipping the widget alone left the flag
        // saying the opposite, so the next launch undid the toggle.
        gTabStrip = !gTabStrip;
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(gNotebook), gTabStrip);
        writeAppPref();
        refreshMenuLabels();
    } break;
    case 113: toggleTitlebar(); refreshMenuLabels(); break;
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
    case 170: runUpdateCheck(true); break;
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
    // "Hide titlebar: 113" is checkable, "Hide titlebar" is not. An entry that
    // is greyed out is marked "[greyed]": a command that is present but
    // insensitive does not run, so a dump that printed only the label and the
    // id would call a dead entry live.
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "cmd-id"));
    const char* grey = gtk_widget_get_sensitive(item) ? "" : " [greyed]";
    if (id) printf("%s: %d%s\n", text, id, grey);
    else printf("%s\n", text);
    if (sub) {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(sub));
        for (GList* k = kids; k; k = k->next)
            if (GTK_IS_MENU_ITEM(k->data)) dumpMenuTree(GTK_WIDGET(k->data), depth + 1);
        g_list_free(kids);
    }
}

// The menu bar is built once at startup, so the dump walks its widgets the
// same way it walks the context menu. Its entries post the same commands the
// context menu carries, so the ids are verified there.
static void dumpMenuShell(GtkWidget* shell, const char* title) {
    printf("== %s ==\n", title);
    GList* kids = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList* k = kids; k; k = k->next)
        if (GTK_IS_MENU_ITEM(k->data)) dumpMenuTree(GTK_WIDGET(k->data), 0);
    g_list_free(kids);
}

// Which commands are usable right now, by the rules the Windows context menu
// draws its greying with (src/main.cpp:5543-5560, advancedMenu :5450-5461):
// New Tab needs room under kMaxTabs (canNewTab, :5311), Close Tab a second
// tab, Save and Save As unsaved work, Print/Rotate a document, Undo and Redo
// a stack that is not empty, and the Advanced cascade greys every document
// tool without a document, Split needs a second page and Clear signatures a
// signature.
//
// One predicate, because the port draws this state twice: the context menu is
// built per open (like Windows, which rebuilds its menu every time), and the
// menu bar is built once and refreshed (refreshMenuLabels). Two copies of the
// rules drifted before - the context menu greyed nothing at all while the
// cascade greyed its own entries.
static bool commandUsable(int id) {
    bool doc = gDoc != nullptr;
    switch (id) {
    case 300: return gDocCount < kMaxTabs;          // canNewTab()
    case 108: return gDocCount < kMaxTabs;          // addTab refuses at the cap
    case 301: return gDocCount > 1;
    case 118: case 119: return doc && gDirty;      // nothing to save when clean
    case 137: case 133: case 134: return doc;
    case 114: return !gUndo.empty();
    case 115: return !gRedo.empty();
    case 310: return doc && gDocPages > 1;
    case 311: return doc && gPageIndex > 0;
    case 312: return doc && gPageIndex + 1 < gDocPages;
    case 201: case 202: case 203: case 205: case 207: return doc;
    case 206: return doc && gDocPages > 1;
    case 204: return !gSigs.empty();
    default: return true;
    }
}

// Build the right-click menu for the current gMenuHlIdx / gMenuPinIdx /
// selection state. Split out from onContextMenu so the menu dump can build the
// same menu in each of its shapes.
//
// The caller owns the menu: a GtkMenu is created floating and a popup that is
// never attached to a widget is never finalized by GTK, so the reference is
// sunk here and the caller releases it with gtk_widget_destroy + g_object_unref.
static GtkWidget* buildContextMenu(int x, int y) {
    GtkWidget* menu = gtk_menu_new();
    g_object_ref_sink(menu);
    // the page under the cursor, in DOCUMENT space: pageAtDocY takes a
    // document-space y and every other caller subtracts docTop() first
    // (charIndexAtDoc, doc.cpp:629; pinAt, doc.cpp:557). The raw event y is
    // widget space, so without this the pin target was the page under the
    // DOCUMENT's top - silently the wrong page whenever the view is scrolled.
    // The rotate and highlight targets come from charIndexAtDoc, which is why
    // only the pin branch disagreed with them.
    int pg = pageAtDocY(y - docTop());

    auto add = [&](const char* label, int id) {
        // greying comes from the state, the way the Windows menu draws it
        // (src/main.cpp:5543-5560): a GTK item is insensitive unless the
        // command applies right now
        GtkWidget* it = menuCommand(label, id);
        if (!commandUsable(id)) gtk_widget_set_sensitive(it, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    };
    auto sep = [&]() { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };

    if (gMenuPinIdx >= 0) {
        add("Edit text", 131);
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
        add("Copy\tCtrl+C", 101);
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
    add("Open...\tCtrl+O", 107);
    add("Open in new tab...", 108);
    add("New Tab\tCtrl+T", 300);
    add("Close Tab\tCtrl+W", 301);
    sep();
    add("Save\tCtrl+S", 118);
    add("Save As...\tCtrl+Shift+S", 119);
    sep();
    add("Print...\tCtrl+P", 137);
    {
        // Windows draws a tick next to Autosave when it is on (MF_CHECKED). A
        // GtkCheckMenuItem flips itself on activate, so the handler would undo
        // it again - the tick is set here and the handler only records the
        // state, which is why it is a plain item with a drawn tick instead.
        GtkWidget* it = menuCommand(gAutosave ? "Autosave ✓" : "Autosave", 109);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    sep();
    add("Undo\tCtrl+Z", 114);
    add("Redo\tCtrl+Y", 115);
    sep();
    add("Find...\tCtrl+F", 103);
    sep();
    add("Fit Width\tCtrl+0", 104);
    add("Zoom In\t+", 105);
    add("Zoom Out\t-", 106);
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
            GtkWidget* it = menuCommand(label, id);
            if (!commandUsable(id)) gtk_widget_set_sensitive(it, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(advSub), it);
        };
        // the ticks and the greying the Windows cascade draws when it builds
        // its menu (advancedMenu, src/main.cpp:5450-5461): the two drawers
        // tick when open, night mode ticks when on, and commandUsable carries
        // the rest - every document tool greys out without a document, Split
        // needs a second page, and Clear signatures needs a signature
        char tickLab[48];
        snprintf(tickLab, sizeof tickLab, "Thumbnails%s", gThumbDrawer ? " ✓" : "");
        addAdv(tickLab, 202);
        snprintf(tickLab, sizeof tickLab, "Outline panel%s", gOutlineDrawer ? " ✓" : "");
        addAdv(tickLab, 201);
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
    add("Minimize", 110);
    // the label carries the window's state, like the Windows entry
    // (src/main.cpp:5565: IsZoomed(h) ? "Restore" : "Maximize")
    add(gWinMax ? "Restore" : "Maximize", 111);
    add(gTitlebar ? "Hide titlebar" : "Show titlebar", 113);
    add(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(gNotebook)) ? "Hide tab strip" : "Show tab strip", 302);
    sep();
    add("Check for updates", 170);
    add("Quit", 112);
    return menu;
}

// the popup currently on screen, if any: a scroll or a re-render must not
// dismiss it (the "menu collapses itself" report), and a suite asserts that.
static GtkWidget* gOpenMenu = nullptr;

static void onContextMenu(GtkWidget* w, GdkEventButton* ev) {
    int x = devX(ev->x), y = devY(ev->y);
    int pg;
    int ci = charIndexAtDoc(x, y, &pg);
    gMenuHlIdx = hlAt(pg, ci);
    gMenuPinIdx = pinAt(x, y);
    gMenuPinPage = -1;
    // The rotation commands act on the page the menu was opened on, not on
    // the page the document happens to be showing. The Windows build keeps
    // that page in gMenuRotPage (src/main.cpp:5515) and the rotation commands
    // read it (src/main.cpp:5748), so a right-click on page 3 rotates page 3
    // even when page 7 is on screen.
    //
    // The capture is taken here, NOT in buildContextMenu: the menu bar posts
    // 133/134 with no right-click behind them, and a capture taken while
    // BUILDING a menu would then follow a rotate the reader never aimed at a
    // page. One function per entry point keeps the two apart.
    gMenuRotPage = pg >= 0 ? pg : gPageIndex;
    GtkWidget* menu = buildContextMenu(x, y);
    // The Windows build clears the captured drop point once the dispatch has consumed
    // it (the clear in onContextMenu, src/main.cpp:5597-5598), so a capture never
    // outlives the menu that made
    // it. The port has a second entry point Windows does not - the menu bar posts
    // 133/134 with no capture of its own - so an uncleared gMenuRotPage would let
    // a menu-bar rotate act on the page of an old right-click. The signal fires
    // after the item's activate handler has run, so the context menu's own entries
    // keep the capture.
    //
    // The same handler destroys the menu and drops the reference buildContextMenu
    // sunk: a GtkMenu is created floating, and a popup that is never attached to
    // a widget is never finalized by GTK, so a menu per right-click would leak
    // the whole widget graph (measured over 240 right-clicks: 77 MB -> 155 MB
    // without this, flat after it). selection-done is the canonical teardown for
    // a never-attached popup - it fires on the item's activation AND on Escape,
    // a click outside, or a popdown, always after the item handler ran.
    gOpenMenu = menu;
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkMenuShell* shell, gpointer) {
        if (gOpenMenu == GTK_WIDGET(shell)) gOpenMenu = nullptr;
        gMenuRotPage = -1;
        gtk_widget_destroy(GTK_WIDGET(shell));
        g_object_unref(shell);
    }), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
}

// The menu has four shapes: a pin under the cursor, a highlight, a live
// selection, and neither. The mark-specific commands exist only in their own
// shape, so a dump that opens one menu can never see them all. This builds
// each shape in turn and prints it.
static void dumpEveryMenuShape() {
    int savedHl = gMenuHlIdx, savedPin = gMenuPinIdx, savedPinPage = gMenuPinPage;
    int savedSelA = gSelAnchor, savedSelH = gSelHead;
    int savedSelAP = gSelAnchorPage, savedSelHP = gSelHeadPage;
    for (int shape = 0; shape < 4; shape++) {
        gMenuHlIdx = -1;
        gMenuPinIdx = -1;
        gMenuPinPage = -1;
        clearSelection();
        if (shape == 1 && !gHls.empty()) gMenuHlIdx = 0;
        else if (shape == 2 && !gPins.empty()) gMenuPinIdx = 0;
        else if (shape == 3 && gDoc) {
            // a live selection: the only state in which the Copy/Highlight and
            // the default-colour ids (101, 135, 140, 149) exist at all
            gSelAnchorPage = gSelHeadPage = gPageIndex;
            gSelAnchor = 0; gSelHead = 1;
        }
        if (shape == 1 && gMenuHlIdx < 0) continue;   // no highlights to show
        if (shape == 2 && gMenuPinIdx < 0) continue;  // no pins to show
        if (shape == 3 && !gDoc) continue;            // no page to select on
        GtkWidget* menu = buildContextMenu(gClientW / 2, gClientH / 2);
        printf("== context menu (shape %d) ==\n", shape);
        GList* kids = gtk_container_get_children(GTK_CONTAINER(menu));
        for (GList* k = kids; k; k = k->next)
            if (GTK_IS_MENU_ITEM(k->data)) dumpMenuTree(GTK_WIDGET(k->data), 0);
        g_list_free(kids);
        gtk_widget_destroy(menu);
        g_object_unref(menu);
    }
    gMenuHlIdx = savedHl;
    gMenuPinIdx = savedPin;
    gMenuPinPage = savedPinPage;
    gSelAnchor = savedSelA; gSelHead = savedSelH;
    gSelAnchorPage = savedSelAP; gSelHeadPage = savedSelHP;

    // A right-click on a SCROLLED page is where the two page targets can
    // disagree: buildContextMenu computes the pin's page itself and
    // onContextMenu computes the rotate capture through charIndexAtDoc, so a
    // coordinate-space slip in one and not the other is invisible while the
    // view sits at the top (both read "page 1"). This shape scrolls away from
    // the top and prints the three targets for the same click, so the gate can
    // assert they name the same page.
    //
    // The click is put through onContextMenu's own capture rather than calling
    // buildContextMenu directly: gMenuRotPage is only ever set there (a menu
    // built for the dump is not a right-click), so reading it without that
    // step would print a constant -1 and prove nothing.
    if (gDoc && gDocPages > 1) {
        int savedScroll = gScrollY;
        // far enough that the document's top is well above the viewport
        scrollByDy(3 * gClientH);
        // how far the view actually moved: a zero delta means the scroll never
        // happened, and then every target below reads the top page and the
        // gate's agreement would be vacuous
        int scrolled = gScrollY - savedScroll;
        // the same hit-testing onContextMenu does, without the popup
        int cpg = -1;
        charIndexAtDoc(gClientW / 2, gClientH / 2, &cpg);
        gMenuRotPage = cpg >= 0 ? cpg : gPageIndex;
        GtkWidget* menu = buildContextMenu(gClientW / 2, gClientH / 2);
        gtk_widget_destroy(menu);
        g_object_unref(menu);
        printf("== scrolled click (scroll delta %d) ==\n", scrolled);
        printf("pin-page %d\n", gMenuPinPage);
        printf("rotate-page %d\n", gMenuRotPage);
        // the page charIndexAtDoc names for the same click - the path
        // onContextMenu uses for highlights and the rotate capture
        int hp = -1;
        charIndexAtDoc(gClientW / 2, gClientH / 2, &hp);
        printf("hit-page %d\n", hp);
        printf("view-page %d\n", gPageIndex + 1);
        gMenuRotPage = -1;
        gScrollY = savedScroll;
        scrollByDy(0);   // re-clamp and repaint from the restored offset
    }
}

// ---- mouse ----
static gboolean onButtonPress(GtkWidget* w, GdkEventButton* ev, gpointer) {
    gtk_widget_grab_focus(w);
    // the context menu moves to the RELEASE handler; a secondary press does nothing
    if (ev->button != GDK_BUTTON_PRIMARY) return TRUE;
    const int ex = devX(ev->x), ey = devY(ev->y);

    // scrollbars first
    int vx, vy, vw, vh, hx, hy, hw, hh;
    sbRects(&vx, &vy, &vw, &vh, &hx, &hy, &hw, &hh);
    int ty, th, tx, tw;
    if (ex >= vx && vThumb(&ty, &th)) {
        if (ey >= ty && ey < ty + th) { gSbDragging = true; gSbWhich = 1; gSbGrab = (int)ey - ty; }
        else {   // page up/down in the track
            int total = docHpx(), view = docViewH();
            if (total > view) {
                int step = (ey < ty ? -1 : 1) * view * 9 / 10;
                scrollByDy(step); renderPage();
            }
        }
        return TRUE;
    }
    if (ey >= hy && hThumb(&tx, &tw)) {
        if (ex >= tx && ex < tx + tw) { gSbDragging = true; gSbWhich = 2; gSbGrab = (int)ex - tx; }
        else {
            int total = docWpx() + designPx(2 * MARGIN), view = gClientW;
            if (total > view) {
                gScrollX += (ex < tx ? -1 : 1) * view * 9 / 10;
                clampScroll(); renderPage();
            }
        }
        return TRUE;
    }

    if (!gSigPlacing.empty()) {
        for (int i = 0; i < gDocPages; i++) {
            int px = pageVx(i), py = pageVy(i), pw = pageWpx(i), ph = pageHpx(i);
            if (ex >= px && ex < px + pw && ey >= py && ey < py + ph) {
                commitSigPlacement(i, ex - px, ey - py);
                // a signature landed, so "Clear signatures" is usable now
                refreshMenuLabels();
                return TRUE;
            }
        }
        return TRUE;
    }
    if (ev->type == GDK_2BUTTON_PRESS) {
        int pg;
        int idx = charIndexClamped((int)ex, (int)ey, &pg);
        if (idx < 0 || pg < 0) return TRUE;
        gSelAnchorPage = gSelHeadPage = pg;
        gSelAnchor = gSelHead = idx;
        selectWord(pg, idx);
        gDragging = true;
        renderPage();
        return TRUE;
    }
    if (ev->type == GDK_3BUTTON_PRESS) {
        int pg;
        int idx = charIndexClamped((int)ex, (int)ey, &pg);
        if (idx < 0 || pg < 0) return TRUE;
        selectLine(pg, idx);
        gDragging = true;
        renderPage();
        return TRUE;
    }
    int pg;
    int idx = charIndexClamped((int)ex, (int)ey, &pg);
    gSelAnchorPage = gSelHeadPage = pg;
    gSelAnchor = gSelHead = idx;
    gDragging = true;
    renderPage();
    return TRUE;
}

static gboolean onButtonRelease(GtkWidget* w, GdkEventButton* ev, gpointer) {
    if (ev->button == GDK_BUTTON_SECONDARY) {
        // The menu opens here, on the release: the event still carries the
        // pointer position, device and time the popup needs, and no button is
        // held once it is up, so no release can land on an item by accident.
        onContextMenu(w, ev);
        return TRUE;
    }
    if (ev->button != GDK_BUTTON_PRIMARY) return TRUE;
    gDragging = false;
    gSbDragging = false;
    return TRUE;
}

// Handlers on a *_event signal answer a BOOLEAN question ("handled?"); a void
// body leaves that return slot to whatever the register happened to hold,
// which is undefined behaviour the compiler cannot even warn about through
// g_signal_connect's void*.
static gboolean onMotion(GtkWidget* w, GdkEventMotion* ev, gpointer) {
    if (gSbDragging) {
        int vx, vy, vw, vh, hx, hy, hw, hh;
        sbRects(&vx, &vy, &vw, &vh, &hx, &hy, &hw, &hh);
        (void)vy; (void)vw; (void)vh; (void)hx; (void)hy; (void)hw; (void)hh;
        int ty, th, tx, tw;
        if (gSbWhich == 1 && vThumb(&ty, &th)) {
            int h = gClientH - sbThick();
            int span = h - th;
            double frac = span > 0 ? ((double)devY(ev->y) - gSbGrab) / span : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int total = docHpx(), view = docViewH();
            gScrollY = (int)(frac * (total - view));
            gRestoredPage = false;
            clampScroll(); ensureActivePage(); renderPage();
        } else if (gSbWhich == 2 && hThumb(&tx, &tw)) {
            int ww = gClientW - sbThick();
            int span = ww - tw;
            double frac = span > 0 ? ((double)devX(ev->x) - gSbGrab) / span : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int total = docWpx() + designPx(2 * MARGIN), view = gClientW;
            gScrollX = (int)(frac * (total - view));
            clampScroll(); renderPage();
        }
        return TRUE;
    }
    // cursor shape over text
    int pg;
    int idx = charIndexClamped((int)devX(ev->x), (int)devY(ev->y), &pg);
    gdk_window_set_cursor(gtk_widget_get_window(w), idx >= 0 ? gIbeam : gArrow);
    if (gDragging) {
        int i2 = charIndexClamped((int)devX(ev->x), (int)devY(ev->y), &pg);
        if (i2 >= 0 && (pg != gSelHeadPage || i2 != gSelHead)) {
            gSelHeadPage = pg; gSelHead = i2;
            renderPage();
        }
    }
    return TRUE;
}

// A scroll event does not render immediately: it marks the frame dirty and an
// idle callback renders ONCE for the whole burst. A touchpad delivers dozens of
// deltas between two frames; rendering each of them did the work of a whole
// scroll every frame and was the "very laggy when i scroll" report. Everything
// else (zoom, mark edits, tab switches) still renders synchronously - only the
// scroll path, where the intermediate positions are never shown, is coalesced.
static bool gRenderPending = false;
static gboolean renderFrameIdle(gpointer) {
    gRenderPending = false;
    renderPage();
    return G_SOURCE_REMOVE;
}
static void requestRender() {
    if (gRenderPending) return;
    gRenderPending = true;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, renderFrameIdle, nullptr, nullptr);
}

static gboolean onScroll(GtkWidget* w, GdkEventScroll* ev, gpointer) {
    // MNPDF_TRACE_SCROLL: the Xvfb wheel probe reads this to tell "events never
    // arrive" from "the handler mis-steps" - the two need different fixes. The
    // first line also prints the drawing area's runtime event mask, so the gate
    // can assert GDK_SMOOTH_SCROLL_MASK was really selected on the window (a
    // missing mask folds a touchpad's deltas into discrete events before this
    // handler ever sees them).
    static const bool trace = getenv("MNPDF_TRACE_SCROLL") != nullptr;
    if (trace && w && gtk_widget_get_window(w))
        fprintf(stderr, "scroll: dir=%d dx=%.2f dy=%.2f state=0x%x scrollY=%d fitW=%d zoom=%.3f uis=%.3f evmask=0x%x\n",
                (int)ev->direction, ev->delta_x, ev->delta_y, (unsigned)ev->state,
                gScrollY, (int)gFitWidth, gZoom, ui_scale(),
                (unsigned)gdk_window_get_events(gtk_widget_get_window(w)));
    else if (trace)
        fprintf(stderr, "scroll: dir=%d dx=%.2f dy=%.2f state=0x%x scrollY=%d fitW=%d zoom=%.3f uis=%.3f\n",
                (int)ev->direction, ev->delta_x, ev->delta_y, (unsigned)ev->state,
                gScrollY, (int)gFitWidth, gZoom, ui_scale());
    // A popup owns the pointer; if a scroll still reaches the page (a touchpad
    // can deliver one while a menu is up), ignore it rather than repainting
    // underneath the menu and risking a popdown.
    if (gOpenMenu) return TRUE;
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
    if (dy == 0 && dx == 0) return TRUE;
    // A touchpad sends GDK_SCROLL_SMOOTH in many tiny deltas (one per frame); a
    // wheel sends one delta of +/-1 per notch. The old code accumulated smooth
    // deltas until |sum| >= 1 and then moved a fixed 120 px step, so a touchpad
    // advanced in 120 px jumps and felt unresponsive and coarse - the "not
    // responding to touchpad" report. Scale the actual delta instead: one wheel
    // notch is one step, and a smooth delta moves proportionally so the page
    // follows the fingers.
    const double stepPx = 3.0 * 40.0 * ui_scale();   // one wheel notch, device px
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        if (ctrl) {
            // Zoom is discrete: accumulate small smooth deltas until a
            // meaningful amount, then zoom once, so a pinch does not jitter.
            static double sZoomAcc = 0;
            sZoomAcc += dy;
            if (fabs(sZoomAcc) < 1.0) return TRUE;
            double step = sZoomAcc > 0 ? 1 : -1;
            sZoomAcc = 0;
            zoomAt(step < 0 ? 1.1 : 1 / 1.1, devX(ev->x), devY(ev->y));
            return TRUE;
        }
        if (dy) { scrollByDy((int)(dy * stepPx + (dy > 0 ? 0.5 : -0.5))); markSave(); }
        if (dx) { gScrollX += (int)(dx * stepPx + (dx > 0 ? 0.5 : -0.5)); clampScroll(); }
        requestRender();
        return TRUE;
    }
    if (ctrl) {
        if (dy) zoomAt(dy < 0 ? 1.1 : 1 / 1.1, devX(ev->x), devY(ev->y));
    } else {
        if (dy) { scrollByDy((int)(dy * stepPx)); markSave(); }
        if (dx) { gScrollX += (int)(dx * stepPx); clampScroll(); }
        requestRender();
    }
    return TRUE;
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
    // GDK_SMOOTH_SCROLL_MASK is not optional: without it the backends fold a
    // touchpad's smooth deltas into coarse discrete events (X11 accumulates and
    // synthesizes one UP/DOWN per whole unit of delta), which is the "does not
    // respond to the touchpad" report - the proportional smooth path in
    // onScroll never ran because smooth events never arrived.
    gtk_widget_add_events(area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
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
    //
    // The list may carry more than one id, and they all run in this one turn:
    // a suite drives a second click there to see that it makes no second
    // request while the first one is still in flight.
    static bool ranCommand = false;
    const char* cmd = getenv("MNPDF_CMD");
    if (cmd && *cmd && !ranCommand) {
        ranCommand = true;
        // MNPDF_OPEN_PATH: open this file as command 107 does (a menu Open
        // without the chooser), so a probe can drive open-in-the-same-tab -
        // the tile cache's most dangerous reuse case.
        if (const char* op = getenv("MNPDF_OPEN_PATH"))
            openPath(utf8ToWide(op));
        std::string list(cmd);
        size_t at = 0;
        while (at <= list.size()) {
            size_t comma = list.find(',', at);
            std::string one = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            int id = atoi(one.c_str());
            if (id > 0) onMenuActivate(nullptr, GINT_TO_POINTER(id));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        g_timeout_add(150, onShotIdle, nullptr);
        return G_SOURCE_REMOVE;
    }
    // MNPDF_MENU_DUMP prints the menu tree as "label: id" and quits. The menu
    // has several shapes - nothing under the cursor, a highlight, a pin, a
    // selection - and the mark-specific commands only exist in their own shape,
    // so every shape is built and printed. Opening one menu at a point with
    // nothing under it hid exactly the ids the parity check needs to see.
    // The menu bar is dumped too: its entries post the same commands the
    // context menu carries, so the ids are verified through the shapes above.
    bool dumpMenu = getenv("MNPDF_MENU_DUMP") != nullptr;
    if (dumpMenu) {
        // The menu bar re-reads its ticks and its greying when a submenu is
        // shown (the show handler installMenuBar connects), because GTK builds
        // it once. A dump never opens a submenu, so without this pass it would
        // print the state of the last refresh rather than the state the reader
        // would be looking at - and a suite asserting on the greying would be
        // reading a stale picture.
        refreshMenuLabels();
        if (gMenuBar) dumpMenuShell(gMenuBar, "menu bar");
        dumpEveryMenuShape();
        fflush(stdout);
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
    if (getenv("MNPDF_SHOT_EXIT") || dumpMenu) {
        // A check that is still in flight has to be waited for: the update
        // check needs the network, and a seam that quits 150 ms after the
        // command would leave the app gone before the answer arrived - so the
        // suite would assert on a run that never finished. The wait is bounded,
        // so a check that hangs does not hang the suite.
        if (getenv("MNPDF_SHOT_EXIT") && gUpdateCheckRunning) {
            // The check's own budget bounds how long its answer can take, so
            // the wait here is longer than that budget: a working but slow
            // network must still get its answer. A run that quit first would
            // look like a check that found nothing, which is a failure of the
            // network, not of the code under test.
            static int waited = 0;
            int limit = (updateCheckBudgetSeconds() + 5) * 4;   // 250 ms a tick
            if (waited < limit) {
                waited++;
                g_timeout_add(250, onShotIdle, nullptr);
                return G_SOURCE_REMOVE;
            }
        }
        // through the REAL quit path, so a suite that sets this exercises the
        // same save logic the Esc key, the menu Quit and the window close use -
        // a seam that saved on its own would hide a broken quit path. The menu
        // dump quits through it as well: a run that asked only for the tree
        // must not sit in the main loop forever, and the dump is not a second
        // way out of the app.
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

// The window is gone. Everything that could still reach into it - the update
// check's late answer, most of all - must be told, so the one place the
// window disappears sets the flag. The Windows build does the same in
// WM_DESTROY (src/main.cpp:6118).
//
// A modal dialog runs its own nested main loop. When the window goes down
// while such a loop is up, that loop ends - but the OUTER loop must end too,
// or the process sits there with no window and only the 800 ms autosave timer
// left, with nothing to click. gtk_main_quit() cannot do that: it leaves the
// INNERMOST running loop, so a modal's nested loop would take it with it.
// The app's own GMainLoop is ended by name here, so it is that loop which
// goes, whatever else is running. The window's destroy signal is the honest
// signal for it: it fires exactly when the window is really gone, whether it
// went down through the quit path or through the window manager.
static void onWindowDestroy(GtkWidget*, gpointer) {
    gShuttingDown = true;
    if (gAppLoop && gAppLoopRunning) g_main_loop_quit(gAppLoop);
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
static GtkWidget* gThumbArea = nullptr;
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
static int gThumbScroll = 0;
static double gThumbSmoothAcc = 0;   // fractional trackpad deltas, accumulated
static int gThumbDrawnFor = -1;               // which page gThumbPix holds

struct BmEntry { std::wstring title; int page; int depth; };
static std::vector<BmEntry> gBmList;
static int gBmSel = -1;

// Recompute the row heights and drop the cached raster. The Windows drawer
// drops its pictures here too: layoutThumbs() calls clearThumbCache(), which
// deletes every bitmap (src/main.cpp:4813-4817), so a refresh after a merge, a
// reopen or a rotation starts the rows again. It has to: the cached picture is
// one page's, and a rotation, an undo, a page move and a reopen all change
// what a row shows without changing how many pages there are. Windows can
// afford to keep a picture per page because it holds one bitmap per row; this
// holds one page's raster in total, so there is nothing to keep.
static void layoutThumbs() {
    gThumbHs.clear();
    gThumbPix.clear();
    gThumbDrawnFor = -1;
    if (!gDoc) { gThumbScroll = 0; return; }
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
    gScrollY = yTopPx(idx) - designPx(MARGIN);
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
            // the drawer appeared, exactly what the Windows drawer avoids.
            // The raster is rendered at the device density and mapped with the
            // surface's device scale, the way ui_paint maps the frame, so a 2x
            // desktop shows a 2x raster instead of an upscaled 1x one
            double ds = ui_scale() > 0 ? ui_scale() : 1.0;
            int tw = designPx(kThumbW), rh = designPx(th);
            if (gThumbDrawnFor != (int)i) {
                std::vector<unsigned char> buf;
                int stride = 0;
                if (renderPageToBuffer((int)i, tw, rh, buf, &stride)) {
                    gThumbPix.swap(buf);
                    gThumbDrawnFor = (int)i;
                }
            }
            if (gThumbDrawnFor == (int)i && !gThumbPix.empty()) {
                cairo_surface_t* s = cairo_image_surface_create_for_data(
                    gThumbPix.data(), CAIRO_FORMAT_ARGB32, tw, rh, tw * 4);
                cairo_surface_set_device_scale(s, ds, ds);
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
    if (idx >= 0 && idx < gDocPages) { gThumbSel = idx; openPageAt(idx); gtk_widget_queue_draw(gThumbArea); }
    // the drawer takes the keyboard, so the keys below reach it
    gtk_widget_grab_focus(gThumbArea);
    return TRUE;
}

// The keys the Windows thumbnail window answers (src/main.cpp:5031-5043):
// Delete removes the selected page, Ctrl+Up and Ctrl+Down move it, R turns
// it, and the plain arrows walk the selection. They act on the drawer's own
// selection, not on the page the document happens to show.
static gboolean onThumbKey(GtkWidget*, GdkEventKey* ev, gpointer) {
    if (gThumbSel < 0) return FALSE;
    bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    if (ev->keyval == GDK_KEY_Delete) deletePage(gThumbSel);
    else if (ev->keyval == GDK_KEY_Up && ctrl) { if (gThumbSel > 0) movePage(gThumbSel, gThumbSel - 1); }
    else if (ev->keyval == GDK_KEY_Down && ctrl) { if (gThumbSel + 1 < gDocPages) movePage(gThumbSel, gThumbSel + 1); }
    else if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) rotatePage(gThumbSel, +1);
    else if (ev->keyval == GDK_KEY_Up) { if (gThumbSel > 0) gThumbSel--; openPageAt(gThumbSel); }
    else if (ev->keyval == GDK_KEY_Down) { if (gThumbSel < gDocPages - 1) gThumbSel++; openPageAt(gThumbSel); }
    else return FALSE;
    gtk_widget_queue_draw(gThumbArea);
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
    // the same positive-is-forward convention the document itself uses; onScroll
    // records the measurement behind it
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
        // FPDFBookmark_GetTitle counts BYTES, including the terminating NUL
        // (third_party/pdfium-linux/include/fpdf_doc.h), and pdfBytesToWide is
        // where that becomes a string - the same rule annotContents follows.
        unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
        std::vector<unsigned short> buf(bytes / 2 + 1, 0);
        if (bytes) FPDFBookmark_GetTitle(bm, buf.data(), (int)bytes);
        BmEntry e;
        e.title = pdfBytesToWide(buf.data(), bytes);
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
    // the drawer answers the keyboard, so it takes key presses and can hold
    // the focus the keys arrive on
    gtk_widget_set_can_focus(gThumbArea, TRUE);
    gtk_widget_add_events(gThumbArea,
        GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(gThumbArea, "draw", G_CALLBACK(onThumbDraw), nullptr);
    g_signal_connect(gThumbArea, "button-press-event", G_CALLBACK(onThumbPress), nullptr);
    g_signal_connect(gThumbArea, "scroll-event", G_CALLBACK(onThumbScrollEv), nullptr);
    g_signal_connect(gThumbArea, "key-press-event", G_CALLBACK(onThumbKey), nullptr);
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
    // The menu bar's ticks and greying follow the same state this refresh
    // follows - a document opened or closed, a page moved, deleted, merged
    // or split - so it is re-read here. The Windows cascade re-reads its
    // state every time it is built (advancedMenu, src/main.cpp:5450), and
    // this menu bar is built once, so it needs the explicit pass.
    refreshMenuLabels();
    // A tab that follows no document (a new empty tab, the last one closed
    // except for the quit path) leaves nothing for a drawer to show, so both
    // are closed, the way the Windows build closes them when the bundle they
    // follow is gone (refreshThumbDrawer / refreshOutlineDrawer,
    // src/main.cpp:4958-4969). Keeping them open would show the previous
    // document's pages and bookmarks under the new tab's reader.
    if (!gDoc) {
        if (gThumbDrawer) {
            gtk_widget_destroy(gThumbDrawer);
            gThumbDrawer = gThumbArea = nullptr;
            gThumbScroll = 0;
        }
        if (gOutlineDrawer) {
            gtk_widget_destroy(gOutlineDrawer);
            gOutlineDrawer = gOutlineArea = nullptr;
        }
        return;
    }
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

static gboolean onWindowConfigure(GtkWidget*, GdkEventConfigure* ev, gpointer) {
    gWinX = ev->x; gWinY = ev->y; gWinW = ev->width; gWinH = ev->height;
    return FALSE;
}

static gboolean onWindowState(GtkWidget*, GdkEventWindowState* ev, gpointer) {
    gWinMax = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
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

// The Windows build rebuilds its menu every time it opens
// (src/main.cpp:5507), so a label that carries the state - "Night mode" with
// a tick - and the greying are always current. A GTK menu bar is built once
// and kept, so both would freeze at their first value: after a toggle the tick
// still claimed the old mode, and a command stayed sensitive after the state
// that made it unusable was gone. This refreshes the bar: installMenuBar calls
// it once, every handler that changes what it shows calls it again, and each
// submenu's "show" signal calls it when the reader opens it - the closest GTK
// gets to Windows rebuilding the menu per popup.
//
// The context menu is built afresh every time it opens, so it needs no such
// care - only the menu bar, which lives as long as the window.
static void refreshMenuLabels() {
    if (!gMenuBar) return;
    // the ticks the Windows cascade draws (MF_CHECKED, src/main.cpp:5450-5456)
    // and the greying it draws (MF_GRAYED, :5458-5464): the menu bar is built
    // once, so its state is re-read every time the state it shows changes,
    // the way Windows rebuilds its own menu per popup
    auto item = [&](const char* key) -> GtkWidget* {
        return (GtkWidget*)g_object_get_data(G_OBJECT(gMenuBar), key);
    };
    auto setLabel = [&](const char* key, const std::string& want) {
        GtkWidget* w = item(key);
        if (w && gtk_menu_item_get_label(GTK_MENU_ITEM(w)) != want)
            gtk_menu_item_set_label(GTK_MENU_ITEM(w), want.c_str());
    };
    // Every entry that posts a command greys by the rules the context menu
    // uses, read from the widget tree rather than from a key per entry: an
    // entry added to the bar later gets its greying without another line here,
    // and the two menus cannot drift apart again. The three top-level holders
    // (File, View, Advanced) post nothing, carry no id and are left sensitive.
    std::function<void(GtkWidget*)> grey = [&](GtkWidget* shell) {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(shell));
        for (GList* k = kids; k; k = k->next) {
            GtkWidget* it = GTK_WIDGET(k->data);
            if (!GTK_IS_MENU_ITEM(it)) continue;
            GtkWidget* sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(it));
            if (sub) { grey(sub); continue; }
            int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "cmd-id"));
            if (id) gtk_widget_set_sensitive(it, commandUsable(id) ? TRUE : FALSE);
        }
        g_list_free(kids);
    };
    grey(gMenuBar);
    // the tick is U+2713, the same character the context menu uses. The
    // mnemonics stay in the labels: gtk_menu_item_get_label returns the
    // label with its underscore, so the comparison carries it too
    setLabel("thumbs-item", std::string("_Thumbnails") + (gThumbDrawer ? " \u2713" : ""));
    setLabel("outline-item", std::string("_Outline panel") + (gOutlineDrawer ? " \u2713" : ""));
    setLabel("night-item", gNight ? "Night mode \u2713" : "Night mode");
    setLabel("titlebar-item", gTitlebar ? "Hide titlebar" : "Show titlebar");
}

// A submenu is shown again on every popup, so this is the port's counterpart of
// the Windows build rebuilding its menu each time it opens (src/main.cpp:5507):
// the greying and the ticks are re-read at the moment the reader looks at them,
// not only when something happened to call refreshMenuLabels.
static gboolean onMenuShow(GtkWidget*, gpointer) {
    refreshMenuLabels();
    return FALSE;
}

static void installMenuBar(GtkWidget* box) {
    GtkWidget* bar = gtk_menu_bar_new();
    gMenuBar = bar;
    GtkWidget* file = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget* fileMenu = gtk_menu_new();
    // Every entry that posts a command records the id it posts, so the menu
    // dump sees the whole command set - the menu bar included, exactly like
    // the context menu, the colour submenus and the Advanced cascade. The
    // greying comes from the same commandUsable predicate the context menu
    // uses, so the two can never disagree.
    auto mi = [](const char* label, int id) {
        GtkWidget* it = gtk_menu_item_new_with_mnemonic(label);
        g_signal_connect(it, "activate", G_CALLBACK(onMenuActivate), GINT_TO_POINTER(id));
        g_object_set_data(G_OBJECT(it), "cmd-id", GINT_TO_POINTER(id));
        if (!commandUsable(id)) gtk_widget_set_sensitive(it, FALSE);
        return it;
    };
    GtkWidget* open = mi("_Open...\tCtrl+O", 107);
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Save\tCtrl+S", 118));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("Save _As...\tCtrl+Shift+S", 119));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Print...\tCtrl+P", 137));
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), mi("_Quit", 112));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
    g_signal_connect(fileMenu, "show", G_CALLBACK(onMenuShow), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);

    GtkWidget* view = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget* viewMenu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Find...\tCtrl+F", 103));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Fit Width\tCtrl+0", 104));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _In\t+", 105));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Zoom _Out\t-", 106));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("_Rotate clockwise", 133));
    gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), mi("Rotate c_ounter-clockwise", 134));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), viewMenu);
    g_signal_connect(viewMenu, "show", G_CALLBACK(onMenuShow), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view);

    // "Advanced" is the Windows cascade (src/main.cpp:5449): everything past
    // plain reading lives here, and the ids are the Windows ones so the same
    // command means the same thing on both platforms.
    GtkWidget* adv = gtk_menu_item_new_with_mnemonic("_Advanced");
    GtkWidget* advMenu = gtk_menu_new();
    auto advItem = [&](const char* key, const char* label, int id) {
        GtkWidget* it = mi(label, id);
        g_object_set_data(G_OBJECT(bar), key, it);
        gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), it);
        return it;
    };
    advItem("thumbs-item", "_Thumbnails", 202);
    advItem("outline-item", "_Outline panel", 201);
    advItem("night-item", gNight ? "Night mode \u2713" : "Night mode", 200);
    advItem("titlebar-item", gTitlebar ? "Hide titlebar" : "Show titlebar", 113);
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), gtk_separator_menu_item_new());
    advItem("merge-item", "_Merge PDFs...", 205);
    advItem("split-item", "Split _pages...", 206);
    advItem("sig-item", "Insert _signature...", 203);
    advItem("clearsig-item", "Clear si_gnatures", 204);
    gtk_menu_shell_append(GTK_MENU_SHELL(advMenu), gtk_separator_menu_item_new());
    advItem("reopen-item", "Reopen last document", 207);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(adv), advMenu);
    g_signal_connect(advMenu, "show", G_CALLBACK(onMenuShow), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), adv);
    // the ticks and the greying the Windows cascade draws at build time are
    // re-read here once for the state at startup; refreshDrawers and the
    // command handlers keep them current afterwards
    refreshMenuLabels();

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

// A window that moves to an output with a different scale factor must re-derive
// the device space it was sized for at startup, or the whole reader renders at
// the old density on the new monitor.
static void onScaleFactorChanged(GObject*, GParamSpec*, gpointer) {
    double s = detectUiScale(gWin);
    if (s == gScale) return;
    gScale = s;
    // only the density changed, so size-allocate may not fire again on its own:
    // run the client-area derivation against the current allocation directly.
    // The drawers re-render from the new density too - the cached thumbnail
    // raster was rendered at the old one.
    GtkAllocation a;
    gtk_widget_get_allocation(gCurArea, &a);
    onAreaSize(gCurArea, &a, nullptr);
    refreshDrawers();
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
    // The window icon is the artwork file next to the BINARY (mnpdf.png, the
    // same source as the Windows mnpdf.ico), because a bare theme name falls
    // back to a generic PDF glyph on a desktop with no mnpdf theme entry.
    //
    // It is resolved from /proc/self/exe, not the current directory: launched
    // from the menu entry the installer writes, the CWD is $HOME, so a
    // "mnpdf.png" file test there failed and the real artwork was skipped even
    // though the installer had just put the artwork into hicolor. The fallback
    // below is the literal theme name "mnpdf" - the name installs used to
    // populate before the content-hashed ones (a same-name pixel change is
    // never re-read by a running desktop, so the installer now writes only
    // "mnpdf-<8 hex>" and retires the rest) - which means after a current
    // install this branch lands on the generic sheet, and the artwork beside
    // the binary above is what carries the brand.
    std::string exe;
    char lnk[PATH_MAX + 1] = {0};
    ssize_t n = readlink("/proc/self/exe", lnk, PATH_MAX);
    if (n > 0) exe = std::string(lnk, (size_t)n);
    size_t slash = exe.find_last_of('/');
    std::string png = (slash == std::string::npos ? std::string("mnpdf.png")
                                                  : exe.substr(0, slash + 1) + "mnpdf.png");
    GError* e = nullptr;
    if (!png.empty() && g_file_test(png.c_str(), G_FILE_TEST_EXISTS)
        && gtk_window_set_icon_from_file(GTK_WINDOW(gWin), png.c_str(), &e)) {
        if (e) { g_error_free(e); e = nullptr; }
    } else {
        if (e) { g_error_free(e); e = nullptr; }
        // The literal theme name - the pre-hash installs populated it, a
        // current install retires it - then the theme's generic PDF sheet
        GtkIconTheme* theme = gtk_icon_theme_get_default();
        if (gtk_icon_theme_has_icon(theme, "mnpdf"))
            gtk_window_set_icon_name(GTK_WINDOW(gWin), "mnpdf");
        else
            gtk_window_set_icon_name(GTK_WINDOW(gWin), "application-pdf");
    }
    g_signal_connect(gWin, "destroy", G_CALLBACK(onWindowDestroy), nullptr);
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

    // The device scale has to be known BEFORE the first layout pass: the drawing
    // area's size-allocate multiplies the logical allocation by it to size the
    // back buffer, so a scale learned after show_all leaves the first frame (and
    // any frame painted before the next resize) at 1x - the "low resolution"
    // report. detectUiScale() only needs the screen, which exists by now.
    gScale = detectUiScale(gWin);
    // and re-derived whenever GTK changes the window's scale factor (a move
    // between outputs of different density, a live desktop-scale change)
    g_signal_connect(gWin, "notify::scale-factor", G_CALLBACK(onScaleFactorChanged), nullptr);

    gtk_widget_show_all(gWin);
    // the overlay children must be re-hidden AFTER show_all: show_all walks every
    // widget including the overlay's, which would leave the find bar and the
    // open-on-first-use drawers visible before they are asked for.
    if (gSearchBar && !gSearchOpen) gtk_widget_hide(gSearchBar);

    // open the file named on the command line, else the remembered last document.
    // A plain SECOND launch is a deliberate new window (src/main.cpp:6241):
    // the document the primary instance already has open is the last thing
    // this window should show, so it starts empty. An explicit file argument
    // still opens - two readers side by side is the point of a second window.
    // A launch that opened nothing - the bare second launch, a first launch
    // with no last document, a last document that no longer exists (its
    // existence is checked before opening, like Windows'
    // GetFileAttributesW, src/main.cpp:6248) - offers the Open dialog, like
    // Windows (src/main.cpp:6251).
    bool opened = false;
    if (argc > 1) {
        opened = openPath(utf8ToWide(argv[1]));
    } else if (gPrimaryInstance) {
        std::wstring last = lastDocumentPath();
        if (!last.empty() && g_file_test(wideToUtf8(last).c_str(), G_FILE_TEST_EXISTS))
            opened = openPath(last);
    }
    if (!opened) {
        std::string f = ui_open_file();
        if (!f.empty()) opened = openPath(utf8ToWide(f));
    }
    if (!gShuttingDown) {
        if (!opened) renderPage();

        gSaveTimer = g_timeout_add(800, onSaveTick, nullptr);
        gtk_widget_grab_focus(gCurArea);
        // The Windows build checks for a newer release before its message loop
        // starts (startUpdateCheck(false), src/main.cpp:6286): a reader is told
        // about a newer release at launch, silently unless there is one, and
        // never more often than the cooldown allows. MNPDF_NO_UPDATE skips only
        // this launch check - the same kind of seam the suites use (below), so a
        // suite that is not about the update check stays off the network. A
        // clicked check (case 170) runs regardless: a suite that clicks is
        // asking for the answer.
        if (!getenv("MNPDF_NO_UPDATE")) runUpdateCheck(false);
        // The shot has to wait for the window to be mapped AND for the drawers a
        // command opened to be allocated: an idle callback fires before the first
        // size negotiation, so a drawer created by MNPDF_CMD would be captured at
        // zero width. A short timeout runs after the layout pass instead.
        // The seams are independent: MNPDF_CMD drives a command, MNPDF_SHOT
        // writes a picture, MNPDF_SHOT_EXIT quits, MNPDF_MENU_DUMP prints the menu
        // and quits - a run that asked only for the tree must not sit in the main
        // loop forever.
        // Registering only for MNPDF_SHOT (as this did) meant a run that asked for
        // a command and a quit but no picture did neither, which is how a broken
        // seam hides; MNPDF_MENU_DUMP was missing for the same reason and made the
        // dump silently print nothing.
        if (getenv("MNPDF_SHOT") || getenv("MNPDF_SHOT_EXIT") || getenv("MNPDF_CMD")
            || getenv("MNPDF_MENU_DUMP"))
            g_timeout_add(getenv("MNPDF_CMD") ? 400 : 0, onShotIdle, nullptr);
        // The app runs a loop of its own, so the window's destroy signal can end
        // exactly that loop: gtk_main_quit() would leave the innermost running
        // loop instead, and a modal's nested loop would take that one with it
        gAppLoop = g_main_loop_new(nullptr, FALSE);
        gAppLoopRunning = true;
        g_main_loop_run(gAppLoop);
        gAppLoopRunning = false;
        g_main_loop_unref(gAppLoop);
        gAppLoop = nullptr;
    }

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
