// mnpdf-native - the lightest PDF reader.
// Phase 1: text layer - drag/word/line selection with copy, find bar with
// per-page match highlights, page-walking match navigation.
// RAM discipline: the only bitmap ever allocated is screen-sized. Pages are
// never rasterized whole; the engine draws the page into the viewport DIB
// at an offset, so memory is independent of zoom and page count. The single
// exception is the print path, which allocates one page-sized raster per
// printed page under kPrintPixelBudget, and nothing else does.
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>          // MIDL_INTERFACE for GDI+; also defines STDMETHOD, which
                           // commdlg.h needs or it silently skips PRINTDLGEXW
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <psapi.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <atomic>
#include <thread>
#include <ctime>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>

#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "fpdf_ppo.h"          // page composition: merge, split, reorder
#include "fpdf_doc.h"          // bookmarks, destinations, actions
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "msimg32.lib")

// ---- state ----
static const wchar_t* const kAppVersion = L"2.1.3";
static const wchar_t* const kGitHubRoot = L"https://github.com";
static const wchar_t* const kRepoPath = L"mnsky-tyan/mnpdf";
static const std::wstring kLatestReleaseUrl =
    std::wstring(kGitHubRoot) + L"/" + kRepoPath + L"/releases/latest";
static const UINT WM_MNPDF_UPDATE_RESULT = WM_APP + 1;
static const int kUpdateCheckIntervalMin = 60;    // self-imposed: one request per hour
static HWND gWnd = nullptr;
static std::atomic<bool> gUpdateCheckRunning = false;
static std::atomic<bool> gManualUpdateRequested = false;
static std::atomic<bool> gShuttingDown = false;
static long long gLastUpdateCheck = 0;             // unix seconds of the last GitHub attempt
static std::wstring gLastUpdateTag;                // tag that attempt found ("" = none succeeded yet)
static std::wstring gPath;
static FPDF_DOCUMENT gDoc = nullptr;
static FPDF_PAGE gPage = nullptr;
static FPDF_TEXTPAGE gTextPage = nullptr;
static int gPageIndex = 0;
// true from a sidecar page restore until the user actually scrolls: a restored page is
// deliberate state, so the viewport-center rule must not second-guess it (see ensureActivePage)
static bool gRestoredPage = false;
static int gPageCount = 0;
static double gPageWpt = 612, gPageHpt = 792;   // current page, points
static double gZoom = 1.0;                       // device px per point
static ULONG_PTR gGdiToken = 0;                 // GDI+ startup token
static bool gGdiOk = false;                     // GDI+ usable: else pin dots fall back to GDI
static int gScrollX = 0, gScrollY = 0;           // document-pixel scroll offset
static const int MARGIN = 8;
static const int GAP = 16;                       // px between stacked pages
static bool gFitWidth = false;                   // zoom tracks the window width

// per-page layout in points, gathered at open; pixel positions derive from it
static double* gPageW = nullptr;
static double* gPageH = nullptr;
static double* gPrefixPt = nullptr;              // pt y of each page top
static int* gRot = nullptr;                      // display rotation per page (0..3)
static int gDocPages = 0;
static double gMaxPageW = 0;

static bool gDirty = false;                      // unsaved highlights/pins/rotations

// ---- undo/redo: value-based ops (no ids needed; values re-apply exactly) ----
struct Hl;
struct Pin;

// selection: (page, char-index) pairs, so a drag can span pages; equal or
// negative = none; stays valid across scroll and zoom because it is page-space
static int gSelAnchor = -1, gSelHead = -1;
static int gSelAnchorPage = -1, gSelHeadPage = -1;
static int gMenuHl = -1;                         // highlight the context menu targets
static int gMenuPin = -1;                        // pin the context menu targets
static int gMenuRotPage = -1;                    // page the context menu targets (rotate)
static int gMenuPinPtPage = -1;                  // "add pin here" drop point
static int gColorTarget = 0;                       // what a custom colour applies to
static double gMenuPinPtX = 0, gMenuPinPtY = 0;
static bool gSelDrag = false;                    // left button held with a live drag
static bool gLastWasDbl = false;                 // for triple-click line select
static DWORD gLastDblTime = 0;
static int gLastDblX = 0, gLastDblY = 0;

// highlights: char ranges per page, rects recomputed at render like selection;
// persisted in the sidecar until save bakes them into the PDF
struct Hl { int page, start, count, color; bool baked; };
static std::vector<Hl> gHls;
static int gHlDefault = 0;                       // default highlight color (0 = yellow)
// marks lifted from the PDF on open: theirs belong to the file; tombstones keep
// a deleted baked mark deleted across quit-without-save
struct Tomb { int kind, page, start, count; double x, y; };   // kind 0 = highlight, 1 = pin
static std::vector<Tomb> gTomb;
// one palette serves both mark kinds: built-in colours plus up to three the
// reader enters as #rrggbb. Red sits last because pins have always been red.
static const int kPalPreset = 6;
static const int kPalCustom = 3;
static const int kPalCount = kPalPreset + kPalCustom;
static HDC gHiHl[kPalCount] = {};
static const COLORREF gPalPreset[kPalPreset] = {
    RGB(255, 230, 128),                          // #ffe680 yellow
    RGB(195, 242, 189),                          // #c3f2bd green
    RGB(184, 220, 255),                          // #b8dcff blue
    RGB(255, 201, 212),                          // #ffc9d4 pink
    RGB(255, 217, 173),                          // #ffd9ad orange
    RGB(230, 71, 77),                            // #e6474d red
};
static const wchar_t* const gPalName[kPalPreset] = {
    L"Yellow", L"Green", L"Blue", L"Pink", L"Orange", L"Red" };
static COLORREF gPalCustom[kPalCustom] = { CLR_INVALID, CLR_INVALID, CLR_INVALID };
static int gPalNext = 0;                         // round-robin slot for the next custom colour
static int gPinDefault = kPalPreset - 1;         // a new pin is red

// resolve a palette index; false when it names an unused custom slot
static bool palColor(int idx, COLORREF& out) {
    if (idx < 0 || idx >= kPalCount) return false;
    if (idx < kPalPreset) { out = gPalPreset[idx]; return true; }
    if (gPalCustom[idx - kPalPreset] == CLR_INVALID) return false;
    out = gPalCustom[idx - kPalPreset];
    return true;
}

// palette label: a name for a preset, the hex for a custom colour
static std::wstring palLabel(int idx) {
    if (idx >= 0 && idx < kPalPreset) return gPalName[idx];
    COLORREF c;
    if (palColor(idx, c)) {
        wchar_t b[16];
        _snwprintf(b, 16, L"#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
        return b;
    }
    return L"";
}

// pins: sticky notes anchored to a user-space point; rendered as a dot,
// text edited in the floating editor; baked as real Text annotations on save
struct Pin { int page; double x, y; std::wstring text; int color; bool baked; };
static std::vector<Pin> gPins;
static int gEditPin = -1;                        // pin being edited in the floating box
static std::wstring gPinBefore;                  // text before the edit started
static bool gPinNew = false;                     // fresh pin with empty text: cancel
static HWND gPinBox = nullptr;                   // the floating editor beside the pin
static HFONT gPinBoxFont = nullptr;              // note font in that editor (shared with the tip)
static WNDPROC gPinBoxBase = nullptr;
static HWND gPinTip = nullptr;                   // hover tip showing the pin text
static HWND gPinTipText = nullptr;
static int gTipPin = -1;                         // pin the pending tip belongs to
static DWORD gPinBoxBorn = 0;                    // when the editor opened (activation grace)

static bool gTitlebar = true;                    // OS caption strip shown
static int gWinX = CW_USEDEFAULT, gWinY = CW_USEDEFAULT;   // last placement
static int gWinW = 1100, gWinH = 800;
static bool gWinMax = false;                     // was maximized at last quit
static std::wstring gPrevPath;                  // the document open before this one
static bool gNight = false;                      // invert the page for dark reading
static bool gThumbsOn = false;                   // thumbnails side drawer visible
static bool gOutlineOn = false;                  // bookmarks side drawer visible

// a signature stamp: a JPEG dropped into the live gDoc as a page object, so it
// renders, prints and saves through the paths that already exist. The sidecar
// keeps "sig=page,cx,cy,w,h,path" lines for the file as it is on disk
struct Sig {
    int page = 0;
    double cx = 0, cy = 0;                      // PDF user space, pt, y up, centre
    double w = 150, h = 60;
    std::wstring path;
};
static std::vector<Sig> gSigs;
static std::wstring gSigPlacing;                // a JPEG chosen, waiting for a click

// ---- undo/redo: value-based ops (no ids needed; values re-apply exactly) ----
struct Op {
    int kind;                                    // 0 hladd 1 hldel 2 hlrecolor 3 pinadd 4 pindel 5 pintext 6 rot 7 pinrecolor
    std::vector<Hl> added, replaced;             // hladd: new segments + overlaps they displaced
    Hl hlVal = {};                               // hldel / hlrecolor payload
    int hlFrom = 0, hlTo = 0;                    // hlrecolor / pinrecolor colors
    Pin pinVal = {};                             // pindel / pinadd payload
    std::wstring textFrom, textTo;               // pintext
    int pinIdx = -1;                             // pintext target; pinrecolor: the pin undo recoloured
    int rotPage = 0, rotFrom = 0, rotTo = 0;     // rot
};
static std::vector<Op> gUndo, gRedo;

static void markDirty() {
    if (!gDirty) { gDirty = true; }
}

static void markSave();
static void writeAppPref();

static void pushOp(const Op& op) {
    gUndo.push_back(op);
    gRedo.clear();
    markDirty();
    markSave();                                   // edits reach the sidecar too
}

static void relayoutPages();
static void clampScroll();
static void ensureActivePage();
static void renderPage();
static bool doSaveAs();
static void replaySigs();
static void refreshDrawers();
static void placeDrawers();

static bool hlOverlap(const Hl& a, const Hl& b) {
    return a.page == b.page && a.start < b.start + b.count && b.start < a.start + a.count;
}

// find the position of an exact value in the highlight list
static int hlIndexOf(const Hl& v) {
    for (size_t k = 0; k < gHls.size(); k++)
        if (gHls[k].page == v.page && gHls[k].start == v.start && gHls[k].count == v.count && gHls[k].color == v.color)
            return (int)k;
    return -1;
}

// a pin carries no id, so a value-based op addresses one by page/point/text
static bool pinSame(const Pin& a, const Pin& b) {
    return a.page == b.page && fabs(a.x - b.x) < 2.0 && fabs(a.y - b.y) < 2.0 && a.text == b.text;
}

// every pinadd/pindel undo shifts the vector, and two notes can share a spot and
// text, so a recolor op also pins the match to the colour it left the pin at
static int pinIndexOf(const Pin& v, int color) {
    for (size_t k = 0; k < gPins.size(); k++)
        if (pinSame(gPins[k], v) && gPins[k].color == color)
            return (int)k;
    return -1;
}

// the bounds-guarded pin/hl mutations the undo and redo cases share
static void pinEraseAt(int idx) {
    if (idx >= 0 && idx < (int)gPins.size()) gPins.erase(gPins.begin() + idx);
}

static void pinSetTextAt(int idx, const std::wstring& text) {
    if (idx >= 0 && idx < (int)gPins.size()) gPins[idx].text = text;
}

// recolour the highlight at an identity (page, start, count); first match wins
static void hlRecolorAt(int page, int start, int count, int color) {
    for (Hl& h : gHls)
        if (h.page == page && h.start == start && h.count == count) { h.color = color; break; }
}

static void rotApply(int page, int rot) {
    if (gRot && page >= 0 && page < gDocPages) {
        gRot[page] = rot;
        relayoutPages();
        clampScroll();
        ensureActivePage();      // every yTopPx moved: re-derive the active page
    }
}

static void undoOp() {
    if (gUndo.empty()) return;
    Op op = gUndo.back();
    gUndo.pop_back();
    switch (op.kind) {
    case 0:                                       // hladd: remove added, restore replaced
        for (const Hl& a : op.added) { int i = hlIndexOf(a); if (i >= 0) gHls.erase(gHls.begin() + i); }
        for (const Hl& r : op.replaced) gHls.push_back(r);
        break;
    case 1:                                       // hldel: restore
        gHls.push_back(op.hlVal);
        break;
    case 2:                                       // hlrecolor
        hlRecolorAt(op.hlVal.page, op.hlVal.start, op.hlVal.count, op.hlFrom);
        break;
    case 3:                                       // pinadd: remove
        pinEraseAt(op.pinIdx);
        break;
    case 4:                                       // pindel: restore
        gPins.push_back(op.pinVal);
        break;
    case 5:                                       // pintext
        pinSetTextAt(op.pinIdx, op.textFrom);
        break;
    case 6:                                       // rot
        rotApply(op.rotPage, op.rotFrom);
        break;
    case 7:                                       // pinrecolor: the pin still wears the op's colour
        { int i = pinIndexOf(op.pinVal, op.hlTo); if (i >= 0) { gPins[i].color = op.hlFrom; op.pinIdx = i; } }
        break;
    }
    gRedo.push_back(op);
    markDirty();
    markSave();                                    // sidecar follows the undone state
    renderPage();
}

static void redoOp() {
    if (gRedo.empty()) return;
    Op op = gRedo.back();
    gRedo.pop_back();
    switch (op.kind) {
    case 0:
        for (const Hl& r : op.replaced) { int i = hlIndexOf(r); if (i >= 0) gHls.erase(gHls.begin() + i); }
        for (const Hl& a : op.added) gHls.push_back(a);
        break;
    case 1:
        gHls.push_back(op.hlVal);
        break;
    case 2:
        hlRecolorAt(op.hlVal.page, op.hlVal.start, op.hlVal.count, op.hlTo);
        break;
    case 3:
        gPins.push_back(op.pinVal);
        break;
    case 4:
        pinEraseAt(op.pinIdx);
        break;
    case 5:
        pinSetTextAt(op.pinIdx, op.textTo);
        break;
    case 6:
        rotApply(op.rotPage, op.rotTo);
        break;
    case 7:                                       // pinrecolor: the pin undo repainted, else that colour
        {
            int i = -1;
            if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()
                && pinSame(gPins[op.pinIdx], op.pinVal) && gPins[op.pinIdx].color == op.hlFrom) i = op.pinIdx;
            if (i < 0) i = pinIndexOf(op.pinVal, op.hlFrom);
            if (i >= 0) gPins[i].color = op.hlTo;
        }
        break;
    }
    gUndo.push_back(op);
    markDirty();
    markSave();                                    // sidecar follows the redone state
    renderPage();
}

// transient text page for selection/copy on non-active pages (one cached)


// search: matches are char ranges across the WHOLE document, recomputed when
// the query changes; Enter/F3 walk them page by page; the label shows 9/113
static HWND gSearchBox = nullptr;
static HWND gMatchLabel = nullptr;               // "9/113" beside the find box
static WNDPROC gEditBaseProc = nullptr;
static bool gSearchOpen = false;
static wchar_t gQuery[128] = L"";
static const int kMatchCap = 4096;               // search stops here; the label then reports the capped count
static struct { int page, start, count; } gMatches[kMatchCap];
static int gMatchCount = 0;
static int gMatchActive = -1;
static void updateMatchLabel() {
    if (!gMatchLabel) return;
    wchar_t buf[32];
    if (gMatchCount > 0) _snwprintf(buf, 32, L"%d/%d", gMatchActive + 1, gMatchCount);
    else wcscpy(buf, gQuery[0] ? L"0/0" : L"");
    SetWindowTextW(gMatchLabel, buf);
}

// 1x1 source bitmaps the highlights blend from; GDI blends, while
// FPDFBitmap_FillRect only replaces pixels and would erase the text under it
static HDC gHiSelection = nullptr;               // blue: drag selection
static bool gSaveDirty = false;                  // sidecar write pending
static bool gAutosave = true;                    // toggle: persist state+edits to the sidecar
static int gEdgeScroll = 0;                      // drag auto-scroll: -1 up, +1 down, 0 still
static const int kEdgeZone = 56;                 // drag auto-scroll: the in-window margin that arms it
static POINT gDragPt = { 0, 0 };                 // last drag cursor pos (client)
static HDC gHiMatch = nullptr;                   // amber: search matches
static HDC gHiActive = nullptr;                  // orange: current search match
static const BLENDFUNCTION gSelBlend = { AC_SRC_OVER, 0, 90, 0 };

// viewport bitmap (the only allocation that scales with the screen)
static HDC gMemDC = nullptr;
static HBITMAP gDib = nullptr;
static void* gBits = nullptr;
static FPDF_BITMAP gPdfBitmap = nullptr;
static int gClientW = 0, gClientH = 0;

static void renderPage();
static void updateTitle();
static bool loadPage(int index);

static double clampZoom(double z) { return z < 0.25 ? 0.25 : (z > 8.0 ? 8.0 : z); }

static int clientW() { RECT r; GetClientRect(gWnd, &r); return r.right - r.left; }
static int clientH() { RECT r; GetClientRect(gWnd, &r); return r.bottom - r.top; }

// document layout in device px (pages stacked with a fixed gap)
static int docWpx() { return (int)(gMaxPageW * gZoom); }
static int rotWpt(int i) { return (gRot[i] & 1) ? (int)(gPageH[i] + 0.5) : (int)(gPageW[i] + 0.5); }
static int rotHpt(int i) { return (gRot[i] & 1) ? (int)(gPageW[i] + 0.5) : (int)(gPageH[i] + 0.5); }
static int pageWpx(int i) { return (int)(rotWpt(i) * gZoom); }
static int pageHpx(int i) { return (int)(rotHpt(i) * gZoom); }
static int yTopPx(int i) { return (int)(gPrefixPt[i] * gZoom) + i * GAP + MARGIN; }
static int docHpx() { return gPrefixPt ? (int)(gPrefixPt[gDocPages] * gZoom) + gDocPages * GAP + 2 * MARGIN : 0; }

// recompute the stacked layout from per-page sizes and rotations (after open,
// rotate, delete); keeps the current top of the viewport anchored
static void relayoutPages() {
    if (!gPrefixPt || !gDocPages) return;
    // every yTopPx() shifts, so the scroll a sidecar restore derived from page
    // sizes is stale: the active page has to be re-derived from the viewport again
    gRestoredPage = false;
    double acc = 0;
    gMaxPageW = 0;
    for (int i = 0; i < gDocPages; i++) {
        gPrefixPt[i] = acc;
        acc += rotHpt(i);
        if (rotWpt(i) > gMaxPageW) gMaxPageW = rotWpt(i);
    }
    gPrefixPt[gDocPages] = acc;
}
static int pageLeftPx(int i) { return MARGIN + (docWpx() - pageWpx(i)) / 2; }   // doc space

// viewport position of the document block: centered in the client whenever
// the document is smaller than the window, otherwise scrolled
static int docLeft() {
    int slack = gClientW - (docWpx() + 2 * MARGIN);
    return slack > 0 ? slack / 2 : -gScrollX;
}

static int docTop() {
    int slack = gClientH - docHpx();
    return slack > 0 ? slack / 2 : -gScrollY;
}

static int pageVx(int i) { return docLeft() + pageLeftPx(i); }   // viewport x
static int pageVy(int i) { return docTop() + yTopPx(i); }        // viewport y

static void clampScroll() {
    if (!gPrefixPt || !gDocPages) { gScrollX = gScrollY = 0; return; }   // no doc yet
    int maxX = docWpx() + 2 * MARGIN - gClientW;
    int maxY = docHpx() - gClientH;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    if (gScrollX > maxX) gScrollX = maxX;
    if (gScrollY > maxY) gScrollY = maxY;
    if (gScrollX < 0) gScrollX = 0;
    if (gScrollY < 0) gScrollY = 0;
}

// the page under the viewport center is the active one (owns selection/search)
static void ensureActivePage();

// bounded cache of parsed pages: re-rendering the viewport while scrolling
// must not re-parse pages, or pdfium's per-parse allocations pile up (observed
// ~350 KB per load). Holds a handful of pages, so RAM stays doc-size-proof.
#define PCACHE 6
static int gPcIndex[PCACHE];
static FPDF_PAGE gPcPage[PCACHE];
static FPDF_TEXTPAGE gPcText[PCACHE];
static unsigned gPcTick[PCACHE];
static unsigned gPcClock;

static void flushPageCache(void) {
    for (int k = 0; k < PCACHE; k++) {
        if (gPcText[k]) FPDFText_ClosePage(gPcText[k]);
        if (gPcPage[k]) FPDF_ClosePage(gPcPage[k]);
        gPcIndex[k] = -1;
        gPcPage[k] = nullptr;
        gPcText[k] = nullptr;
    }
    gPage = nullptr;                               // these alias cache slots
    gTextPage = nullptr;
}

// parsed handle for page i, cached; recency-ordered, evicts the oldest slot
// (never the active page's: gPage/gTextPage alias that slot)
static FPDF_PAGE acquirePage(int i) {
    if (!gDoc || i < 0 || i >= gDocPages) return nullptr;
    int slot = -1, oldest = -1;
    for (int k = 0; k < PCACHE; k++) {
        if (gPcIndex[k] == i) { gPcTick[k] = ++gPcClock; return gPcPage[k]; }
        if (gPcIndex[k] < 0) { slot = k; break; }
        if (gPcIndex[k] != gPageIndex && (oldest < 0 || gPcTick[k] < gPcTick[oldest])) oldest = k;
    }
    if (slot < 0) slot = oldest;
    if (slot < 0) return gPage;                      // cache is all active-page copies: impossible
    if (gPcText[slot]) { FPDFText_ClosePage(gPcText[slot]); gPcText[slot] = nullptr; }
    if (gPcPage[slot]) FPDF_ClosePage(gPcPage[slot]);
    gPcPage[slot] = nullptr;
    gPcIndex[slot] = i;
    gPcPage[slot] = FPDF_LoadPage(gDoc, i);
    if (!gPcPage[slot]) gPcIndex[slot] = -1;       // don't cache a failed load
    gPcTick[slot] = ++gPcClock;
    return gPcPage[slot];
}

static FPDF_TEXTPAGE textPageOf(int page) {
    if (!acquirePage(page)) return nullptr;
    for (int k = 0; k < PCACHE; k++)
        if (gPcIndex[k] == page) {
            if (!gPcText[k]) gPcText[k] = FPDFText_LoadPage(gPcPage[k]);
            return gPcText[k];
        }
    return nullptr;
}

static void pagePxToPt(int page, double sx, double sy, double* ox, double* oy);

// client px -> nearest (page, char index); PDF user space, y flipped;
// tolerance widens so a drag started on empty space still snaps to nearby text
static int charIndexAtDoc(int cx, int cy, int* pageOut);

// the page under a document-space y, or -1 when the point is outside the stack
static int pageAtDocY(int docY) {
    for (int i = 0; i < gDocPages; i++) {
        int top = yTopPx(i);
        if (docY >= top && docY < top + pageHpx(i)) return i;
    }
    return -1;
}

// nearest character at a page-space point, widening the tolerance from 4
// device px up to maxPx; -1 when nothing hits. Every hit-test site used to
// carry its own copy of this loop with a different cap, which is how the
// caps drifted (32 / 16 / 128 px).
static int charIndexWithTol(FPDF_TEXTPAGE tp, double px, double py, int maxPx) {
    for (int px10 = 4; px10 <= maxPx; px10 *= 2) {
        double tol = px10 / gZoom;
        int idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
        if (idx >= 0) return idx;
    }
    return -1;
}

// nearest character for a point that missed all glyph boxes: clamps to the
// line horizontally, or to the page start/end when above/below the text
static int snapIndexNear(int page, int cx, int cy) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    int n = FPDFText_CountChars(tp);
    if (n <= 0) return -1;
    double ppx = cx - docLeft() - pageLeftPx(page);
    double ppy = cy - docTop() - yTopPx(page);
    double px, py;
    pagePxToPt(page, ppx, ppy, &px, &py);
    double W = gPageW[page], H = gPageH[page];
    int rot = gRot[page] & 3;
    if (rot == 0) {                                  // upright: y=H is visual top
        if (py >= H) return 0;                       // above the top line
        if (py <= 0) return n - 1;                   // below the last line
    } else if (rot == 2) {                           // 180deg: order flips
        if (py <= 0) return n - 1;
        if (py >= H) return 0;
    }
    int idx = charIndexWithTol(tp, px, py, 128);             // wide horizontal snap
    if (idx >= 0) return idx;
    // still nothing: find the vertically nearest line, take its nearer end
    int best = 0, bestEnd = 0;
    double bestDist = 1e18;
    for (int i = 0; i < n; i += 8) {                 // coarse scan every 8th char
        double l, r, b, t;
        if (!FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) continue;
        double mid = (t + b) / 2;
        double d = mid > py ? mid - py : py - mid;
        if (d < bestDist) {
            bestDist = d;
            best = i;
            int j = i;
            double l2, r2, b2, t2;
            while (j + 1 < n && FPDFText_GetCharBox(tp, j + 1, &l2, &r2, &b2, &t2) &&
                   (t2 + b2) / 2 <= mid + 1 && (t2 + b2) / 2 >= mid - 1)
                j++;
            bestEnd = j;
        }
    }
    double bl, br, bb, bt;
    if (!FPDFText_GetCharBox(tp, best, &bl, &br, &bb, &bt)) return n - 1;
    return px * 2 < bl + br ? best : bestEnd;
}

static int charIndexAtDoc(int cx, int cy, int* pageOut) {
    *pageOut = -1;
    if (!gPrefixPt || !gDocPages) return -1;
    int docY = cy - docTop();
    int page = pageAtDocY(docY);
    if (page < 0) {
        // blank gap between/around pages: snap to the nearer page edge
        if (docY < yTopPx(0)) { *pageOut = 0; return 0; }
        int last = gDocPages - 1;
        int lastBottom = yTopPx(last) + pageHpx(last);
        if (docY >= lastBottom) { *pageOut = last; return -2; }   // end of last page
        for (int i = 0; i < gDocPages - 1; i++) {
            int bot = yTopPx(i) + pageHpx(i);
            int nxtTop = yTopPx(i + 1);
            if (docY >= bot && docY < nxtTop) {
                *pageOut = i;
                return (docY - bot <= nxtTop - docY) ? -2 : -3;   // upper: page end, lower: next page start
            }
        }
        return -1;
    }
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    double ppx = cx - docLeft() - pageLeftPx(page);
    double ppy = docY - yTopPx(page);
    double px, py;
    pagePxToPt(page, ppx, ppy, &px, &py);
    int idx = -1;
    idx = charIndexWithTol(tp, px, py, 32);                  // 4..32 device px
    if (idx < 0) idx = snapIndexNear(page, cx, cy);          // blank space on the page
    if (idx < 0) return -1;
    *pageOut = page;
    return idx;
}

// selection-friendly variant: resolves blank page gaps to (page, char) the way
// a text editor would - negative raw codes become page start/end
// cursor variant: strict glyph hit (no blank-space snapping), cheap per move
static int charIndexStrict(int cx, int cy, int* pageOut) {
    *pageOut = -1;
    if (!gPrefixPt || !gDocPages) return -1;
    int docY = cy - docTop();
    int page = pageAtDocY(docY);
    if (page < 0) return -1;
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    double px, py;
    pagePxToPt(page, cx - docLeft() - pageLeftPx(page), docY - yTopPx(page), &px, &py);
    return charIndexWithTol(tp, px, py, 16);
}

static int charIndexClamped(int cx, int cy, int* pageOut) {
    int idx = charIndexAtDoc(cx, cy, pageOut);
    if (*pageOut < 0) return -1;
    if (idx == -2) {                                 // end of *pageOut
        FPDF_TEXTPAGE tp = textPageOf(*pageOut);
        idx = tp ? FPDFText_CountChars(tp) - 1 : -1;
    } else if (idx == -3) {                          // start of *pageOut + 1
        (*pageOut)++;
        idx = 0;
    }
    return idx;
}

static void clearSelection() { gSelAnchor = gSelHead = -1; gSelAnchorPage = gSelHeadPage = -1; }

static bool hasSelection() {
    if (gSelAnchor < 0 || gSelHead < 0) return false;
    if (gSelAnchorPage == gSelHeadPage) return gSelAnchor != gSelHead;
    return true;
}

// ordered selection range across pages
static void selRange(int* p0, int* i0, int* p1, int* i1) {
    *p0 = gSelAnchorPage; *i0 = gSelAnchor;
    *p1 = gSelHeadPage;   *i1 = gSelHead;
    if (*p0 > *p1 || (*p0 == *p1 && *i0 > *i1)) {
        int t;
        t = *p0; *p0 = *p1; *p1 = t;
        t = *i0; *i0 = *i1; *i1 = t;
    }
}

static bool isWordSep(unsigned int u) {
    return u == ' ' || u == '\t' || u == '\r' || u == '\n' || u == 0xA0;
}

static void selectWord(int page, int idx) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp || idx < 0) return;
    int n = FPDFText_CountChars(tp);
    int a = idx, b = idx;
    while (a > 0 && !isWordSep(FPDFText_GetUnicode(tp, a - 1))) a--;
    while (b + 1 < n && !isWordSep(FPDFText_GetUnicode(tp, b + 1))) b++;
    gSelAnchorPage = gSelHeadPage = page;
    gSelAnchor = a;
    gSelHead = b;
}

static void selectLine(int page, int idx) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp || idx < 0) return;
    double al, ar, ab, at;
    if (!FPDFText_GetCharBox(tp, idx, &al, &ar, &ab, &at)) return;
    int n = FPDFText_CountChars(tp);
    int a = idx, b2 = idx;
    for (int i = idx - 1; i >= 0; i--) {
        double l, r, cb, ct;
        if (!FPDFText_GetCharBox(tp, i, &l, &r, &cb, &ct)) break;
        if (cb >= at || ct <= ab) break;           // no vertical overlap: other line
        a = i;
    }
    for (int i = idx + 1; i < n; i++) {
        double l, r, cb, ct;
        if (!FPDFText_GetCharBox(tp, i, &l, &r, &cb, &ct)) break;
        if (cb >= at || ct <= ab) break;
        b2 = i;
    }
    gSelAnchorPage = gSelHeadPage = page;
    gSelAnchor = a;
    gSelHead = b2;
}

// double click: select the whitespace-delimited run around idx
static bool loadPage(int index);   // defined below the search bar; nextMatch calls it earlier

static void copyTextRange(int p0, int i0, int p1, int i1);

// the [s,e] char slice of page p inside an ordered cross-page range: copy,
// selection drawing and highlighting all walk a range the same way, and this
// prologue used to exist three times over
static bool pageSlice(int p, int p0, int i0, int p1, int i1,
                      FPDF_TEXTPAGE* tpOut, int* sOut, int* eOut) {
    FPDF_TEXTPAGE tp = textPageOf(p);
    if (!tp) return false;
    int n = FPDFText_CountChars(tp);
    *sOut = (p == p0) ? i0 : 0;
    *eOut = (p == p1) ? i1 : n - 1;
    if (*eOut < *sOut || *eOut >= n) return false;
    *tpOut = tp;
    return true;
}

static void copySelection() {
    if (!hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    copyTextRange(p0, i0, p1, i1);
}

// clipboard write of an ordered (page, char) range; pages joined with CRLF
static void copyTextRange(int p0, int i0, int p1, int i1) {
    std::vector<unsigned short> buf;
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        size_t base = buf.size();
        buf.resize(base + (size_t)(e - s + 2));
        int got = FPDFText_GetText(tp, s, e - s + 1, buf.data() + base);
        if (got <= 1) { buf.resize(base); continue; }   // got includes the terminator
        buf.resize(base + got - 1);                     // drop terminator
        if (p < p1 && !buf.empty()) { buf.push_back('\r'); buf.push_back('\n'); }
    }
    if (buf.empty()) return;
    buf.push_back(0);
    BOOL opened = FALSE;                           // clipboard is contended: retry briefly
    for (int i = 0; i < 10 && !(opened = OpenClipboard(gWnd)); i++) Sleep(10);
    if (!opened) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (size_t)buf.size() * 2);
    if (hg) {
        void* p = GlobalLock(hg);
        if (p) { memcpy(p, buf.data(), (size_t)buf.size() * 2); GlobalUnlock(hg); }
        if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
    }
    CloseClipboard();
}

// create the 1x1 highlight source for a slot on first use; BGRA byte order
// (writing a COLORREF here would swap R and B)
static HDC highlightDC(HDC* slot, BYTE bb, BYTE gg, BYTE rr) {
    if (*slot) return *slot;
    HDC screen = GetDC(nullptr);
    *slot = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 1;
    bmi.bmiHeader.biHeight = -1;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) { DeleteDC(*slot); *slot = nullptr; return nullptr; }
    BYTE* p = (BYTE*)bits;
    p[0] = bb; p[1] = gg; p[2] = rr; p[3] = 255;
    SelectObject(*slot, bmp);                      // kept for the process lifetime
    return *slot;
}

// a palette slot's colour changed: drop its blend source so the next draw
// rebuilds it from the new colour (highlightDC caches per slot)
static void releaseHiHl(int idx) {
    if (idx < 0 || idx >= kPalCount || !gHiHl[idx]) return;
    HBITMAP bmp = (HBITMAP)GetCurrentObject(gHiHl[idx], OBJ_BITMAP);
    DeleteDC(gHiHl[idx]);
    if (bmp) DeleteObject(bmp);
    gHiHl[idx] = nullptr;
}

// user-space pt <-> px inside the rendered page box (top-left origin), for the
// page's display rotation; user y is up, screen y is down; includes zoom
static void pagePtToPx(int page, double x, double y, double* ox, double* oy) {
    double W = gPageW[page], H = gPageH[page];
    switch (gRot[page] & 3) {
    case 0:  *ox = x * gZoom;       *oy = (H - y) * gZoom; break;
    case 1:  *ox = y * gZoom;       *oy = x * gZoom;       break;
    case 2:  *ox = (W - x) * gZoom; *oy = y * gZoom;       break;
    case 3:  *ox = (H - y) * gZoom; *oy = (W - x) * gZoom; break;
    }
}

static void pagePxToPt(int page, double sx, double sy, double* ox, double* oy) {
    double W = gPageW[page], H = gPageH[page];
    switch (gRot[page] & 3) {
    case 0:  *ox = sx / gZoom;      *oy = H - sy / gZoom;  break;
    case 1:  *ox = sy / gZoom;      *oy = sx / gZoom;      break;
    case 2:  *ox = W - sx / gZoom;  *oy = sy / gZoom;      break;
    case 3:  *ox = W - sy / gZoom;  *oy = H - sx / gZoom;  break;
    }
}

// blend the highlight rects of a char range on one page; rects recompute every
// render, so highlights follow scroll and zoom for free
static void blendCharRects(int page, FPDF_TEXTPAGE tp, int start, int count, HDC src) {
    if (!gMemDC || !src || !tp) return;
    int nr = FPDFText_CountRects(tp, start, count);
    int pl = pageVx(page), pt2 = pageVy(page);
    for (int i = 0; i < nr; i++) {
        double l, t, r, b;
        if (!FPDFText_GetRect(tp, i, &l, &t, &r, &b)) continue;
        double ax, ay, bx, by;
        pagePtToPx(page, l, t, &ax, &ay);
        pagePtToPx(page, r, b, &bx, &by);
        int x = pl + (int)(ax < bx ? ax : bx);
        int y = pt2 + (int)(ay < by ? ay : by);
        int w = (int)fabs(ax - bx);
        int h = (int)fabs(ay - by);
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > gClientW) w = gClientW - x;
        if (y + h > gClientH) h = gClientH - y;
        if (w > 0 && h > 0)
            AlphaBlend(gMemDC, x, y, w, h, src, 0, 0, 1, 1, gSelBlend);
    }
}

static void drawSelection() {
    if (!gMemDC || !hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    HDC src = highlightDC(&gHiSelection, 215, 120, 0);
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        blendCharRects(p, tp, s, e - s + 1, src);
    }
}

// ---- highlights ----

static int hlAt(int page, int idx) {
    if (idx < 0) return -1;
    for (size_t k = 0; k < gHls.size(); k++) {
        const Hl& h = gHls[k];
        if (h.page == page && idx >= h.start && idx < h.start + h.count) return (int)k;
    }
    return -1;
}

// drop exact duplicate pins (same page, spot, text) - an annotation read back
// from the file must not double with the same pin still in a sidecar
static void normalizePins() {
    for (int k = (int)gPins.size() - 1; k >= 0; k--) {
        for (int j = k + 1; j < (int)gPins.size(); j++) {
            if (pinSame(gPins[j], gPins[k])) {
                gPins.erase(gPins.begin() + k);
                break;
            }
        }
    }
}

// collapse overlaps so no character ever carries two highlights (later wins);
// guards against stacked entries from older sidecars or from the file
static void normalizeHls() {
    for (int k = (int)gHls.size() - 1; k >= 0; k--) {
        for (int j = k + 1; j < (int)gHls.size(); j++) {
            if (hlOverlap(gHls[k], gHls[j])) {     // a later entry covers this one
                gHls.erase(gHls.begin() + k);
                break;
            }
        }
    }
}

// turn the current selection into highlights, one segment per page. New
// segments REPLACE every highlight they overlap - never stack (one undo step)
static void applyHighlightFromSelection(int color) {
    if (!hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    Op op = {};
    op.kind = 0;
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        Hl seg = { p, s, e - s + 1, color };
        for (int k = (int)gHls.size() - 1; k >= 0; k--) {
            if (hlOverlap(seg, gHls[k])) {
                op.replaced.push_back(gHls[k]);
                gHls.erase(gHls.begin() + k);
            }
        }
        gHls.push_back(seg);
        op.added.push_back(seg);
    }
    clearSelection();
    normalizeHls();                                // safety net against legacy stacked entries
    if (!op.added.empty()) pushOp(op);
    renderPage();
}

static void recolorHighlight(int idx, int color) {
    if (idx < 0 || idx >= (int)gHls.size() || gHls[idx].color == color) return;
    Op op = {};
    op.kind = 2;
    op.hlVal = gHls[idx];
    op.hlFrom = gHls[idx].color;
    op.hlTo = color;
    gHls[idx].color = color;
    pushOp(op);
    renderPage();
}

static void deleteHighlight(int idx) {
    if (idx < 0 || idx >= (int)gHls.size()) return;
    Op op = {};
    op.kind = 1;
    op.hlVal = gHls[idx];
    if (gHls[idx].baked)
        gTomb.push_back({ 0, gHls[idx].page, gHls[idx].start, gHls[idx].count, 0, 0 });
    gHls.erase(gHls.begin() + idx);
    pushOp(op);
    renderPage();
}

// ---- pins ----

// pin dot radius: grows with zoom so it reads as part of the page, but stays
// hittable when zoomed far out and modest when zoomed far in
static int pinRadius(double zoom) {
    int r = (int)(3.0 * zoom);                     // a shade smaller than before
    if (r < 3) r = 3;
    if (r > 14) r = 14;
    return r;
}

// where a pin dot lands on the screen and what it is painted with; both draw
// branches used to run this same prologue and differ only in the draw call
struct PinDraw { int x, y, r; COLORREF color; };

static bool pinDrawPos(const Pin& pn, PinDraw* out) {
    int vy = pageVy(pn.page), vh = pageHpx(pn.page);
    if (vy + vh < 0 || vy > gClientH) return false;      // page not in view
    double ax, ay;
    pagePtToPx(pn.page, pn.x, pn.y, &ax, &ay);
    out->x = pageVx(pn.page) + (int)ax;
    out->y = pageVy(pn.page) + (int)ay;
    out->r = pinRadius(gZoom);
    if (!palColor(pn.color, out->color)) out->color = gPalPreset[kPalPreset - 1];
    return true;
}

static void drawPins() {
    if (!gMemDC || gPins.empty()) return;
    if (gGdiOk) {
        // locals, not statics: GDI+ objects must die before GdiplusShutdown
        Gdiplus::Graphics g(gMemDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);   // GDI's Ellipse leaves a wiggly edge
        for (const Pin& pn : gPins) {
            PinDraw d;
            if (!pinDrawPos(pn, &d)) continue;
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(d.color), GetGValue(d.color), GetBValue(d.color)));
            g.FillEllipse(&brush, d.x - d.r, d.y - d.r, 2 * d.r, 2 * d.r);
        }
    } else {
        for (const Pin& pn : gPins) {
            PinDraw d;
            if (!pinDrawPos(pn, &d)) continue;
            HBRUSH br = CreateSolidBrush(d.color);
            HPEN np = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ ob = SelectObject(gMemDC, br);
            HGDIOBJ op = SelectObject(gMemDC, np);
            Ellipse(gMemDC, d.x - d.r, d.y - d.r, d.x + d.r, d.y + d.r);
            SelectObject(gMemDC, op);
            SelectObject(gMemDC, ob);
            DeleteObject(np);
            DeleteObject(br);
        }
    }
}

static int pinAt(int cx, int cy) {
    if (!gPrefixPt || !gDocPages) return -1;
    int i = pageAtDocY(cy - docTop());
    if (i < 0) return -1;
    int docY = cy - docTop();
    double px, py;
    pagePxToPt(i, cx - docLeft() - pageLeftPx(i), docY - yTopPx(i), &px, &py);
    // the hover target stays a comfortable 8px even when the dot itself
    // has shrunk, so a small pin is still easy to hover
    double hr = (double)pinRadius(gZoom) / gZoom;
    double minHr = 8.0 / gZoom;
    if (hr < minHr) hr = minHr;
    for (int k = 0; k < (int)gPins.size(); k++) {
        const Pin& pn = gPins[k];
        if (pn.page == i && fabs(pn.x - px) < hr && fabs(pn.y - py) < hr) return k;
    }
    return -1;
}

static void commitPinEdit();                       // fwd

static void destroyPinBox() {
    if (gPinBox) { HWND b = gPinBox; gPinBox = nullptr; DestroyWindow(b); }
    // gPinBoxFont is the shared note font: freed once at shutdown, not here
}

// client coords of a pin dot
static bool pinClientPos(int idx, int* cx, int* cy) {
    if (idx < 0 || idx >= (int)gPins.size() || !gPrefixPt) return false;
    const Pin& pn = gPins[idx];
    if (pn.page < 0 || pn.page >= gDocPages) return false;
    double ax, ay;
    pagePtToPx(pn.page, pn.x, pn.y, &ax, &ay);
    *cx = pageVx(pn.page) + (int)ax;
    *cy = pageVy(pn.page) + (int)ay;
    return true;
}

static void hidePinTip(HWND h) {
    KillTimer(h, 4);
    if (gPinTip) ShowWindow(gPinTip, SW_HIDE);
    gTipPin = -1;
}

// ------------------------------------------------------------- note boxes
// The hover note and the pin editor are two views of one thing, so they share
// a single shape: a base size that grows sideways while the note is one line
// and downwards once it wraps. The text is sized to the page's own body text
// so a note reads like the document it sits on.

static std::vector<double> gPageTextPt;         // body text pt per page (0 = unmeasured)
static const int kNoteMaxChars = 4000;           // note text length limit
static HFONT gNoteFont = nullptr;               // shared by the hover note and editor
static int gNoteFontPx = 0;                     // px height the shared font was built at

// most common glyph size on a page is its body text, not a heading or a caption
static double pageTextPt(int page) {
    if (page < 0 || page >= (int)gPageTextPt.size()) return 11.0;
    if (gPageTextPt[page] > 0) return gPageTextPt[page];
    double pt = 11.0;
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (tp) {
        int n = FPDFText_CountChars(tp);
        std::vector<std::pair<int, int>> hist;  // rounded pt -> char count
        for (int i = 0; i < n && i < 3000; i++) {
            double s = FPDFText_GetFontSize(tp, i);
            if (s <= 0.5) continue;
            int r = (int)(s + 0.5);
            size_t k = 0;
            for (; k < hist.size(); k++) if (hist[k].first == r) { hist[k].second++; break; }
            if (k == hist.size()) hist.push_back(std::make_pair(r, 1));
        }
        int best = 0, bestN = 0;
        for (size_t k = 0; k < hist.size(); k++)
            if (hist[k].second > bestN) { bestN = hist[k].second; best = hist[k].first; }
        if (bestN >= 5) pt = best;              // a stray glyph is not body text
    }
    gPageTextPt[page] = pt;
    return pt;
}

// one font for both boxes, rebuilt when the page scale changes
static HFONT noteFont(int page) {
    int px = (int)(pageTextPt(page) * gZoom + 0.5);
    if (px < 11) px = 11;                       // legible even zoomed far out
    if (px > 34) px = 34;
    if (gNoteFont && px == gNoteFontPx) return gNoteFont;
    if (gNoteFont) DeleteObject(gNoteFont);
    gNoteFont = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FF_DONTCARE, L"Segoe UI");
    gNoteFontPx = px;
    // a note box that is already up keeps a live font; the hover note comes back
    // at the new size on the next hover rather than showing the old one
    if (gPinTipText) SendMessageW(gPinTipText, WM_SETFONT, (WPARAM)gNoteFont, TRUE);
    if (gTipPin >= 0) hidePinTip(gWnd);
    return gNoteFont;
}

// base size both boxes start from, and the ceiling they grow to
static void noteBoxBounds(int* baseW, int* baseH, int* maxW, int* maxH) {
    double z = gZoom;
    int bw = (int)(120.0 * z);                  // a short note opens compact, not a big empty box
    if (bw < 150) bw = 150;
    if (bw > 380) bw = 380;
    int bh = (int)(34.0 * z);
    if (bh < 44) bh = 44;
    if (bh > 120) bh = 120;
    int mw = (int)(330.0 * z);
    if (mw < 340) mw = 340;
    if (mw > 860) mw = 860;
    int mh = (int)(190.0 * z);
    if (mh < 200) mh = 200;
    if (mh > 560) mh = 560;
    *baseW = bw; *baseH = bh; *maxW = mw; *maxH = mh;
}

// size a note box for [text]: sideways while it stays one line, downwards once
// it wraps; never smaller than the base size, never larger than the ceiling
static void noteBoxSize(HDC hdc, const std::wstring& text, HFONT font,
                        int* w, int* h) {
    int baseW, baseH, maxW, maxH;
    noteBoxBounds(&baseW, &baseH, &maxW, &maxH);
    const int padX = 10, padY = 8;              // text inset plus the border
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT one = { 0, 0, 0, 0 };                  // one line of this font
    DrawTextW(hdc, L"Ag", -1, &one, DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOCLIP);
    int lineH = one.bottom - one.top;
    if (lineH <= 0) lineH = 16;
    RECT nat = { 0, 0, 0, 0 };                  // natural size, no wrapping
    if (!text.empty())
        DrawTextW(hdc, text.c_str(), -1, &nat, DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOCLIP);
    int natW = nat.right - nat.left;
    int natH = nat.bottom - nat.top;
    int lines = natH > 0 ? (natH + lineH - 1) / lineH : 1;
    if (natW + padX * 2 <= maxW) {              // fits sideways: width follows the text
        *w = natW + padX * 2;
        if (*w < baseW) *w = baseW;
        *h = lines * lineH + padY * 2;
    } else {                                    // too wide: wrap, then grow down
        RECT wr = { 0, 0, maxW - padX * 2, 0 };
        DrawTextW(hdc, text.c_str(), -1, &wr,
                  DT_CALCRECT | DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP);
        *w = maxW;
        *h = (wr.bottom - wr.top) + padY * 2;
    }
    if (*h < baseH) *h = baseH;
    if (*h > maxH) *h = maxH;
    SelectObject(hdc, old);
}

// keep a [size]-tall/wide box within [lo, hi]; if it cannot fit, stick to lo
static int clampBox(int pos, int size, int lo, int hi) {
    if (pos < lo) pos = lo;
    else if (pos + size > hi) pos = hi - size;
    if (pos < lo) pos = lo;
    return pos;
}

// where a note box may live, in screen coordinates: the app window inset by 4
// px, and from the client top down so a box never covers the caption strip
static void noteBoxArea(RECT* out) {
    RECT wr;
    GetWindowRect(gWnd, &wr);
    POINT cp = { 0, 0 };
    ClientToScreen(gWnd, &cp);
    out->left = wr.left + 4;
    out->right = wr.right - 4;
    out->top = cp.y + 4;
    out->bottom = wr.bottom - 4;
}

// place a note box beside its anchor inside the app window: flip to the other
// side when it would run off the edge, then clamp so it cannot leave the window
// (a monitor-wide clamp lets a grown box escape onto the desktop)
static void placeNoteBox(int ax, int ay, int w, int h, int* bx, int* by) {
    RECT r;
    noteBoxArea(&r);
    int x = ax + 14;
    if (x + w > r.right) x = ax - 14 - w;
    *bx = clampBox(x, w, r.left, r.right);
    *by = clampBox(ay - h / 2, h, r.top, r.bottom);
}

static void showPinTip(int idx) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    const std::wstring& t = gPins[idx].text;
    if (t.empty()) { hidePinTip(gWnd); return; }
    int page = gPins[idx].page;
    HFONT font = noteFont(page);
    if (!gPinTip) {
        // no WS_EX_TOPMOST: a hover note belongs above this window only, never
        // on top of whatever the reader switched to
        gPinTip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"mnpdfPinTip", L"",
                                  WS_POPUP | WS_BORDER, 0, 0, 10, 10, gWnd, nullptr,
                                  (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE), nullptr);
        if (!gPinTip) return;
        gPinTipText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
                                      5, 5, 10, 10, gPinTip, nullptr,
                                      (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE), nullptr);
        if (!gPinTipText) return;
    }
    std::wstring txt = t;
    if (txt.size() > kNoteMaxChars) txt.resize(kNoteMaxChars);
    SendMessageW(gPinTipText, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowTextW(gPinTipText, txt.c_str());
    // same shape the editor uses for the same text
    HDC hdc = GetDC(gPinTip);
    if (!hdc) { hidePinTip(gWnd); return; }
    int w, h;
    noteBoxSize(hdc, txt, font, &w, &h);
    ReleaseDC(gPinTip, hdc);
    int cx = clientW() / 2, cy = clientH() / 2;
    pinClientPos(idx, &cx, &cy);
    POINT sp = { cx, cy };
    ClientToScreen(gWnd, &sp);
    int bx, by;                                   // beside the pin, inside the window
    placeNoteBox(sp.x, sp.y, w, h, &bx, &by);
    SetWindowPos(gPinTipText, nullptr, 5, 5, w - 10, h - 10,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(gPinTip, HWND_TOP, bx, by, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

static void sizePinBoxToText();                  // fwd
static LRESULT CALLBACK colorBoxProc(HWND b, UINT m, WPARAM wp, LPARAM lp);


static LRESULT CALLBACK pinBoxProc(HWND b, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { commitPinEdit(); return 0; }   // Enter stays a newline
        // Enter alone would only add a line, so the way to finish is Ctrl+Enter
        if (wp == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) { commitPinEdit(); return 0; }
        {
            LRESULT r = CallWindowProcW(gPinBoxBase, b, m, wp, lp);
            // a delete key shortens the note; anything else is a cheap no-op
            sizePinBoxToText();
            return r;
        }
    case WM_CHAR: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO: {
        // everything that can change the note: let the edit control act first,
        // then refit the box so it keeps the shape the hover note shows
        LRESULT r = CallWindowProcW(gPinBoxBase, b, m, wp, lp);
        sizePinBoxToText();
        return r;
    }
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(gPinBoxBase, b, m, wp, lp);
        // an empty editor explains itself in grey instead of a second window
        if (GetWindowTextLengthW(b) == 0 && gEditPin >= 0) {
            HDC hdc = GetDC(b);
            if (hdc) {
                RECT rc;
                GetClientRect(b, &rc);
                InflateRect(&rc, -3, -2);        // match the edit control's text inset
                HFONT old = (HFONT)SelectObject(hdc, gPinBoxFont);
                COLORREF oldCol = SetTextColor(hdc, RGB(150, 150, 150));
                int oldBk = SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Ctrl+Enter to save", -1, &rc,
                          DT_TOP | DT_LEFT | DT_NOCLIP | DT_SINGLELINE);
                SetBkMode(hdc, oldBk);
                SetTextColor(hdc, oldCol);
                SelectObject(hdc, old);
                ReleaseDC(b, hdc);
            }
        }
        return r;
    }
    case WM_KILLFOCUS:
        // a transient activation (a closing menu re-activating us) must not
        // commit a box that only just opened. The 400 ms is a contract with the
        // colour box and its suite: test-colors.ps1 waits out the same grace
        // before it clicks away, because both boxes must ignore a focus loss
        // they did not ask for (src/main.cpp colorBoxProc says the same).
        if (GetTickCount() - gPinBoxBorn > 400) commitPinEdit();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && GetTickCount() - gPinBoxBorn > 400) {
            commitPinEdit(); return 0;
        }
        break;
    }
    return CallWindowProcW(gPinBoxBase, b, m, wp, lp);
}

// resize the open editor to fit what has been typed: it grows in place and the
// position is re-clamped to the app window, so it can never leave the window
static void sizePinBoxToText() {
    if (gEditPin < 0 || gEditPin >= (int)gPins.size() || !gPinBox) return;
    // the shared font is rebuilt when the page scale moves (a resize refits the
    // zoom), so keep the editor on the current one
    HFONT f = noteFont(gPins[gEditPin].page);
    if (f && f != gPinBoxFont) {
        gPinBoxFont = f;
        SendMessageW(gPinBox, WM_SETFONT, (WPARAM)f, TRUE);
    }
    int n = GetWindowTextLengthW(gPinBox);
    std::vector<wchar_t> buf(n + 1);
    if (n) GetWindowTextW(gPinBox, buf.data(), n + 1);
    std::wstring text = n ? buf.data() : L"";
    HDC hdc = GetDC(gPinBox);
    if (!hdc) return;
    int w, h;
    noteBoxSize(hdc, text, gPinBoxFont, &w, &h);
    ReleaseDC(gPinBox, hdc);
    RECT wr;
    GetWindowRect(gPinBox, &wr);
    int bx = wr.left, by = wr.top;                // grows in place, never out of the window
    RECT r;
    noteBoxArea(&r);
    bx = clampBox(bx, w, r.left, r.right);
    by = clampBox(by, h, r.top, r.bottom);
    if (bx == wr.left && by == wr.top
        && wr.right - wr.left == w && wr.bottom - wr.top == h) return;   // nothing to move or grow
    SetWindowPos(gPinBox, nullptr, bx, by, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// open the editor as its own box beside the pin (Enter = newline, click
// outside = commit; the find bar is never reused)
static void startPinEdit(int idx, bool isNew) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    if (gEditPin >= 0) commitPinEdit();            // commit whatever was open
    if (idx >= (int)gPins.size()) return;
    hidePinTip(gWnd);
    gEditPin = idx;
    gPinNew = isNew;
    gPinBefore = gPins[idx].text;
    int cx = clientW() / 2, cy = clientH() / 2;
    pinClientPos(idx, &cx, &cy);
    POINT sp = { cx, cy };
    ClientToScreen(gWnd, &sp);
    // the box starts at the shared base size beside the pin and grows as the
    // note is typed, the same shape the hover note shows for that text
    int baseW, baseH, maxW, maxH;
    noteBoxBounds(&baseW, &baseH, &maxW, &maxH);
    int bw = baseW, bh = baseH;
    int bx, by;                                   // beside the pin, inside the window
    placeNoteBox(sp.x, sp.y, bw, bh, &bx, &by);
    gPinBox = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                              WS_POPUP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                              bx, by, bw, bh, gWnd, nullptr,
                              (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE), nullptr);
    if (!gPinBox) { gEditPin = -1; return; }
    gPinBoxBorn = GetTickCount();
    // the note text matches the page's own body text, so it reads at page scale
    gPinBoxFont = noteFont(gPins[idx].page);
    SendMessageW(gPinBox, WM_SETFONT,
                 (WPARAM)(gPinBoxFont ? gPinBoxFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(gPinBox, EM_LIMITTEXT, kNoteMaxChars, 0);
    SetWindowTextW(gPinBox, gPins[idx].text.c_str());
    gPinBoxBase = (WNDPROC)SetWindowLongPtrW(gPinBox, GWLP_WNDPROC, (LONG_PTR)pinBoxProc);
    ShowWindow(gPinBox, SW_SHOW);
    SetForegroundWindow(gPinBox);
    SetFocus(gPinBox);
    sizePinBoxToText();                          // an existing note opens at its own size
    SendMessageW(gPinBox, EM_SETSEL, 0, -1);
    updateTitle();
    renderPage();                                  // draw the new pin dot right away
}

static void commitPinEdit() {
    if (gEditPin < 0) { destroyPinBox(); return; }
    if (gEditPin >= (int)gPins.size()) { gEditPin = -1; destroyPinBox(); return; }
    std::wstring text;
    if (gPinBox) {
        int n = GetWindowTextLengthW(gPinBox);
        std::vector<wchar_t> buf(n + 1);
        GetWindowTextW(gPinBox, buf.data(), n + 1);
        text.assign(buf.data(), n);
    }
    bool fresh = gPinNew;
    int idx = gEditPin;
    std::wstring before = gPinBefore;
    gEditPin = -1;
    gPinNew = false;
    destroyPinBox();
    if (fresh) {
        if (text.empty()) {
            gPins.erase(gPins.begin() + idx);            // fresh pin, empty text: cancel
        } else {
            Op op = {};
            op.kind = 3;
            op.pinVal = gPins[idx];
            op.pinVal.text = text;
            gPins[idx].text = text;
            pushOp(op);
        }
    } else if (text != before) {
        Op op = {};
        op.kind = 5;
        op.pinIdx = idx;
        op.textFrom = before;
        op.textTo = text;
        gPins[idx].text = text;
        pushOp(op);
    }
    updateTitle();
    renderPage();
}

// add a pin under a client point and open its editor
static void addPinAt(int page, double px, double py) {
    gPins.push_back({ page, px, py, L"", gPinDefault, false });
    startPinEdit((int)gPins.size() - 1, true);
}

static void deletePin(int idx) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    Op op = {};
    op.kind = 4;
    op.pinVal = gPins[idx];
    if (gPins[idx].baked)
        gTomb.push_back({ 1, gPins[idx].page, 0, 0, gPins[idx].x, gPins[idx].y });
    gPins.erase(gPins.begin() + idx);
    pushOp(op);
    renderPage();
}

// recolour a pin: same undo story as a highlight recolour
static void recolorPin(int idx, int color) {
    if (idx < 0 || idx >= (int)gPins.size() || gPins[idx].color == color) return;
    COLORREF probe;                              // an unused custom slot is not a colour
    if (!palColor(color, probe)) return;
    Op op = {};
    op.kind = 7;
    op.pinVal = gPins[idx];                      // the pin as it was, by value
    op.hlFrom = gPins[idx].color;
    op.hlTo = color;
    gPins[idx].color = color;
    pushOp(op);
    renderPage();
}

// ---- page rotation ----

static void rotatePage(int page, int dir) {              // dir: +1 CW, -1 CCW
    if (!gRot || page < 0 || page >= gDocPages) return;
    Op op = {};
    op.kind = 6;
    op.rotPage = page;
    op.rotFrom = gRot[page];
    gRot[page] = (gRot[page] + (dir > 0 ? 1 : 3)) & 3;
    op.rotTo = gRot[page];
    relayoutPages();
    clampScroll();
    ensureActivePage();          // every yTopPx moved: re-derive the active page
    pushOp(op);
    renderPage();
}

static void drawHighlights() {
    if (!gMemDC || gHls.empty()) return;
    for (const Hl& h : gHls) {
        int vy = pageVy(h.page), vh = pageHpx(h.page);
        if (vy + vh < 0 || vy > gClientH) continue;      // page not in view
        FPDF_TEXTPAGE tp = textPageOf(h.page);
        if (!tp) continue;
        COLORREF hc;
        if (!palColor(h.color, hc)) hc = gPalPreset[0];
        blendCharRects(h.page, tp, h.start, h.count,
                       highlightDC(&gHiHl[h.color], GetBValue(hc), GetGValue(hc), GetRValue(hc)));
    }
}

static void drawMatches() {
    if (!gMemDC || !gMatchCount) return;
    for (int m = 0; m < gMatchCount; m++) {
        if (gMatches[m].page != gPageIndex) continue;
        bool active = m == gMatchActive;
        blendCharRects(gPageIndex, gTextPage, gMatches[m].start, gMatches[m].count,
                       highlightDC(active ? &gHiActive : &gHiMatch,
                                   0, active ? 150 : 208, 255));
    }
}

// scan the whole document for the query; case-insensitive (flags = 0)
static void runSearch() {
    gMatchCount = 0;
    gMatchActive = -1;
    if (gQuery[0]) {
        for (int i = 0; i < gDocPages && gMatchCount < kMatchCap; i++) {
            FPDF_TEXTPAGE tp = textPageOf(i);
            if (!tp) continue;
            FPDF_SCHHANDLE sh = FPDFText_FindStart(tp, (FPDF_WIDESTRING)gQuery, 0, 0);
            if (!sh) continue;
            while (gMatchCount < kMatchCap && FPDFText_FindNext(sh)) {
                gMatches[gMatchCount].page = i;
                gMatches[gMatchCount].start = FPDFText_GetSchResultIndex(sh);
                gMatches[gMatchCount].count = FPDFText_GetSchCount(sh);
                gMatchCount++;
            }
            FPDFText_FindClose(sh);
        }
        if (gMatchCount) gMatchActive = 0;
    }
    updateMatchLabel();
    renderPage();
}

// viewport-relative y range of a char-range's rects within its page box, doc
// space (add yTopPx(page) for doc coords)
static bool charRectYRange(int page, FPDF_TEXTPAGE tp, int start, int count, int* ry0, int* ry1) {
    int nr = FPDFText_CountRects(tp, start, count);
    if (nr <= 0) return false;
    int lo = INT_MAX, hi = INT_MIN;
    for (int i = 0; i < nr; i++) {
        double l, t, r, b;
        if (!FPDFText_GetRect(tp, i, &l, &t, &r, &b)) continue;
        double ax, ay, bx, by;
        pagePtToPx(page, l, t, &ax, &ay);
        pagePtToPx(page, r, b, &bx, &by);
        int yTop = (int)(ay < by ? ay : by);
        int yBot = (int)(ay > by ? ay : by);
        if (yTop < lo) lo = yTop;
        if (yBot > hi) hi = yBot;
    }
    if (lo > hi) return false;
    *ry0 = lo;
    *ry1 = hi;
    return true;
}

static void scrollMatchIntoView() {
    if (gMatchActive < 0 || gMatchActive >= gMatchCount) return;
    int pg = gMatches[gMatchActive].page;
    int ry0, ry1;
    if (!charRectYRange(pg, textPageOf(pg), gMatches[gMatchActive].start, gMatches[gMatchActive].count, &ry0, &ry1)) return;
    int y0 = yTopPx(pg) + ry0;
    int y1 = yTopPx(pg) + ry1;
    if (y0 < gScrollY) gScrollY = y0 - 8;
    else if (y1 > gScrollY + gClientH) gScrollY = y1 - gClientH + 8;
    clampScroll();
}

// Enter/F3: advance through the document-wide list, wrapping at the ends
static void nextMatch(int dir) {
    if (!gQuery[0] || !gMatchCount) return;
    int i = gMatchActive + dir;
    if (i < 0) i = gMatchCount - 1;
    if (i >= gMatchCount) i = 0;
    gMatchActive = i;
    if (gMatches[i].page != gPageIndex) loadPage(gMatches[i].page);
    scrollMatchIntoView();
    updateMatchLabel();
    renderPage();
}

static void toggleSearch(bool show) {
    if (!gSearchBox) return;
    gSearchOpen = show;
    if (!show) {
        gMatchCount = 0;
        gMatchActive = -1;
    }
    ShowWindow(gSearchBox, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (gMatchLabel) ShowWindow(gMatchLabel, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) {
        SetFocus(gSearchBox);
        SendMessageW(gSearchBox, EM_SETSEL, 0, -1);
        renderPage();                              // title shows the find prompt
    } else {
        SetFocus(gWnd);
        renderPage();
    }
}


static void placeSearchBar() {
    if (!gSearchBox) return;
    int x = clientW() - 260 - 12;
    MoveWindow(gSearchBox, x, 10, 260, 26, TRUE);
    if (gMatchLabel) MoveWindow(gMatchLabel, x - 66, 10, 62, 26, TRUE);
}

static LRESULT CALLBACK editProc(HWND e, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { toggleSearch(false); return 0; }
        if (wp == VK_RETURN) { nextMatch(1); return 0; }
        if (wp == VK_F3) { nextMatch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0; }
        break;
    }
    return CallWindowProcW(gEditBaseProc, e, m, wp, lp);
}

static void createSearchBar(HWND h, HINSTANCE inst) {
    gSearchBox = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 260, 26,
                                 h, (HMENU)(INT_PTR)1, inst, nullptr);
    SendMessageW(gSearchBox, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(gSearchBox, EM_LIMITTEXT, 127, 0);
    gEditBaseProc = (WNDPROC)SetWindowLongPtrW(gSearchBox, GWLP_WNDPROC, (LONG_PTR)editProc);
    gMatchLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTERIMAGE,
                                  0, 0, 60, 26, h, (HMENU)(INT_PTR)3, inst, nullptr);
    SendMessageW(gMatchLabel, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    ShowWindow(gSearchBox, SW_HIDE);               // hidden until Find asks for it
    ShowWindow(gMatchLabel, SW_HIDE);
    placeSearchBar();
}

// swap the active page handle (the one that owns selection/search); never
// renders and never touches the scroll, callers decide what view follows
static bool loadPage(int index) {
    if (!gDoc || index < 0 || index >= gDocPages) return false;
    FPDF_PAGE p = acquirePage(index);              // owned by the page cache
    if (!p) return false;
    FPDF_TEXTPAGE t = textPageOf(index);           // resolve BEFORE switching gPageIndex
    gPage = p;
    gTextPage = t;
    gPageIndex = index;
    gPageWpt = gPageW[index];
    gPageHpt = gPageH[index];
    return true;
}

static void ensureActivePage() {
    if (!gDoc || gRestoredPage) return;   // a restored page owns gPageIndex until the user scrolls
    int cy = gScrollY + gClientH / 2;
    for (int i = 0; i < gDocPages; i++) {
        if (cy < yTopPx(i) + pageHpx(i)) {
            if (i != gPageIndex) loadPage(i);
            return;
        }
    }
}

// (re)create the screen-sized DIB and render into it
static void rebuildSurface() {
    gClientW = clientW(); gClientH = clientH();
    if (gClientW <= 0 || gClientH <= 0) return;

    if (gPdfBitmap) { FPDFBitmap_Destroy(gPdfBitmap); gPdfBitmap = nullptr; }
    if (gDib) { DeleteObject(gDib); gDib = nullptr; }
    if (gMemDC) { DeleteDC(gMemDC); gMemDC = nullptr; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = gClientW;
    bmi.bmiHeader.biHeight = -gClientH;          // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(gWnd);
    gMemDC = CreateCompatibleDC(screen);
    gDib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &gBits, nullptr, 0);
    ReleaseDC(gWnd, screen);
    if (!gDib || !gBits) return;
    SelectObject(gMemDC, gDib);

    gPdfBitmap = FPDFBitmap_CreateEx(gClientW, gClientH, FPDFBitmap_BGRA, gBits, gClientW * 4);
    clampScroll();
    ensureActivePage();
    renderPage();
}

static void renderPage() {
    updateTitle();   // first: the title must survive the early returns below
    if (!gPdfBitmap || !gDoc) return;
    FPDFBitmap_FillRect(gPdfBitmap, 0, 0, gClientW, gClientH, 0xFF202020);   // letterbox
    int top = gScrollY, bottom = gScrollY + gClientH;
    // render every page intersecting the viewport; only handles for pages in
    // view are alive, so RAM stays independent of document size
    for (int i = 0; i < gDocPages; i++) {
        int y0 = yTopPx(i);
        if (y0 >= bottom) break;                   // pages are in order
        int h = pageHpx(i);
        if (y0 + h <= top) continue;
        FPDF_PAGE p = acquirePage(i);              // cached: no re-parse churn
        if (!p) continue;
        int w = pageWpx(i);
        int vx = pageVx(i);
        int vy = pageVy(i);
        // paper: pdfium paints NO page background of its own, so any PDF that
        // relies on the viewer's default white page renders as faint text on
        // the letterbox. Paint the visible part of the page rect white first;
        // a PDF that draws its own background simply overdraws it.
        {
            int x0 = vx > 0 ? vx : 0, yy0 = vy > 0 ? vy : 0;
            int x1 = vx + w < gClientW ? vx + w : gClientW;
            int y1 = vy + h < gClientH ? vy + h : gClientH;
            if (x1 > x0 && y1 > yy0)
                FPDFBitmap_FillRect(gPdfBitmap, x0, yy0, x1 - x0, y1 - yy0, 0xFFFFFFFF);
        }
        FPDF_RenderPageBitmap(gPdfBitmap, p, vx, vy, w, h, gRot[i], FPDF_ANNOT | FPDF_LCD_TEXT);
    }
    drawHighlights();
    drawPins();
    drawMatches();
    drawSelection();
    if (gEditPin >= 0 && gPinBox) sizePinBoxToText();   // a refit must not strand the editor
    if (gNight && gPdfBitmap) {                      // last, so marks are re-coloured too
        // the paper inverts, not the surround: the letterbox is already the
        // dark background, and inverting it too would ring the page in grey
        BYTE* inv = (BYTE*)FPDFBitmap_GetBuffer(gPdfBitmap);
        int istr = FPDFBitmap_GetStride(gPdfBitmap);
        for (int i = 0; i < gDocPages; i++) {
            int ty = yTopPx(i);
            if (ty >= bottom) break;                 // pages are in order
            int th = pageHpx(i);
            if (ty + th <= top) continue;
            int vx = pageVx(i), vy = pageVy(i), vw = pageWpx(i);
            int x0 = vx > 0 ? vx : 0, ny0 = vy > 0 ? vy : 0;
            int x1 = vx + vw < gClientW ? vx + vw : gClientW;
            int ny1 = vy + th < gClientH ? vy + th : gClientH;
            for (int y = ny0; y < ny1; y++) {
                BYTE* row = inv + y * istr;
                for (int x = x0; x < x1; x++) {
                    row[x * 4 + 0] = 255 - row[x * 4 + 0];
                    row[x * 4 + 1] = 255 - row[x * 4 + 1];
                    row[x * 4 + 2] = 255 - row[x * 4 + 2];
                }
            }
        }
    }
    InvalidateRect(gWnd, nullptr, FALSE);
}

// zoom mode: fit-width refits on every resize until a manual zoom turns it off
static void applyFitWidth() {
    int cw = clientW() - 2 * MARGIN;
    if (cw <= 0 || gMaxPageW <= 0) return;
    double nz = clampZoom(cw / gMaxPageW);
    if (nz == gZoom) return;                     // same fit, so no page moved
    gZoom = nz;
    // a refit rescales every page, so a scroll anchored to page offsets is now
    // stale: put the restored page back under the viewport top
    if (gRestoredPage) gScrollY = yTopPx(gPageIndex) - MARGIN;
}

static void fitWidth() {
    gFitWidth = true;
    applyFitWidth();
    clampScroll();
    markSave();
    renderPage();
}

// zoom anchored proportionally on the document point under the cursor; a
// manual zoom ends fit-width mode
static void zoomAt(double factor, int cx, int cy) {
    double nz = clampZoom(gZoom * factor);
    if (nz == gZoom) return;
    gRestoredPage = false;      // a manual zoom is a new view, not a restored one
    gFitWidth = false;
    double oldH = docHpx(), oldW = docWpx() + 2 * MARGIN;
    gZoom = nz;
    double newH = docHpx(), newW = docWpx() + 2 * MARGIN;
    gScrollX = (int)((gScrollX + cx) * newW / oldW) - cx;
    gScrollY = (int)((gScrollY + cy) * newH / oldH) - cy;
    clampScroll();
    markSave();
    renderPage();
}

// ---- print: standard Windows print dialog, pages rendered clean (no
// annotations and no mnpdf marks). Printing is a VIEW operation: it never
// writes to the file, never marks the document dirty, never enters the undo
// stack and never moves the viewport, so a print cannot corrupt user state.

// The only transient the print path is allowed: one page raster, 64 MB at 4
// bytes per pixel, freed the moment that page is blitted.
static const double kPrintPixelBudget = 16.0 * 1000.0 * 1000.0;

// Rasterize one page at the given pixel size. The box is the same the display
// path uses (rotWpt/rotHpt, same rotation state), so a print always reflects
// what is on screen. flags default to 0 on purpose: FPDF_ANNOT would pull in the
// document's own baked annotations and FPDF_LCD_TEXT the screen-only subpixel
// hinting, which prints as colour fringing - the print omits both, so the probe
// renders the same page a second time with FPDF_ANNOT to prove the ink really
// is dropped.
static BYTE* rasterizePage(int page, int w, int h, int* strideOut, int flags = 0) {
    if (w <= 0 || h <= 0) return nullptr;
    FPDF_PAGE p = FPDF_LoadPage(gDoc, page);
    if (!p) return nullptr;
    int stride = w * 4;
    BYTE* buf = (BYTE*)calloc(1, (size_t)stride * h);
    if (!buf) { FPDF_ClosePage(p); return nullptr; }
    for (int y = 0; y < h; y++) memset(buf + (size_t)y * stride, 0xFF, (size_t)stride);
    FPDF_BITMAP bm = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, buf, stride);
    if (bm) {
        FPDF_RenderPageBitmap(bm, p, 0, 0, w, h, gRot[page], flags);
        FPDFBitmap_Destroy(bm);
    }
    FPDF_ClosePage(p);
    *strideOut = stride;
    return buf;
}

static void freeRaster(BYTE* buf) { if (buf) free(buf); }

// ---- test hook ------------------------------------------------------------
// A posted-message harness cannot click a modal print dialog, so the probe
// renders the raster path for real and skips the dialog entirely. It also
// renders the same page WITH annotations and writes both checksums, which is
// what proves the print path drops ink rather than merely succeeding.
static unsigned long checksumOf(const BYTE* buf, size_t n) {
    unsigned long sum = 5381;
    for (size_t i = 0; i < n; i++) sum = ((sum << 5) + sum) ^ buf[i];
    return sum;
}

static void printProbe() {
    if (!gDoc || gDocPages <= 0) return;
    const wchar_t* out = _wgetenv(L"MNPDF_PRINT_PROBE");   // caller checked too; cheap and first
    if (!out || !*out) return;
    int w = rotWpt(0) * 2, h = rotHpt(0) * 2;   // ~144 dpi in points
    int stride = 0;
    BYTE* clean = rasterizePage(0, w, h, &stride);
    if (!clean) return;
    // write a private temp name and move it into place, so the file only
    // ever exists complete: a reader polling for its existence must never
    // land between the create and the last fwrite and read a 0-byte file
    std::wstring tmp = std::wstring(out) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (f) {
        // annotated twin: identical box, FPDF_ANNOT alone, so the two checksums
        // differ for exactly the one flag the intent is about. Rendered only
        // once the output is known writable - it exists for this one checksum.
        int dirtyStride = 0;
        BYTE* dirty = rasterizePage(0, w, h, &dirtyStride, FPDF_ANNOT);
        // ink present in the clean page: proves a page really rasterized
        long ink = 0;
        for (long i = 0; i < (long)stride * h; i++)
            if (clean[i] < 0xF0) ink++;
        int annots = 0, inkAnnots = 0;
        FPDF_PAGE p = FPDF_LoadPage(gDoc, 0);
        if (p) {
            annots = FPDFPage_GetAnnotCount(p);
            // Link and Popup annotations carry no ink, so a page full of
            // them renders identically with FPDF_ANNOT on. Only an
            // ink-drawing subtype can make clean and annot differ, which
            // is what the test compares.
            for (int i = 0; i < annots; i++) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, i);
                if (!a) continue;
                int st = FPDFAnnot_GetSubtype(a);
                if (st != FPDF_ANNOT_LINK && st != FPDF_ANNOT_POPUP) inkAnnots++;
                FPDFPage_CloseAnnot(a);
            }
            FPDF_ClosePage(p);
        }
        // ASCII on purpose: a wide-mode probe file arrives as UTF-16 with
        // no BOM, which the suites cannot parse
        fprintf(f, "pages=%d w=%d h=%d ink=%ld annots=%d inkannots=%d clean=%lu annot=%lu\n",
                gDocPages, w, h, ink, annots, inkAnnots,
                checksumOf(clean, (size_t)stride * h),
                dirty ? checksumOf(dirty, (size_t)dirtyStride * h) : 0UL);
        fclose(f);
        MoveFileExW(tmp.c_str(), out, MOVEFILE_REPLACE_EXISTING);
        freeRaster(dirty);
    }
    freeRaster(clean);
}

// ---- the print itself -----------------------------------------------------
static void doPrint() {
    if (!gDoc || gDocPages <= 0) return;                      // nothing to print
    if (_wgetenv(L"MNPDF_PRINT_PROBE")) { printProbe(); return; }

    PRINTDLGEXW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = gWnd;
    // the app prints whole pages only: neither the Selection nor the Current Page
    // radio is implemented, so both are disabled and the range selector is a
    // plain All-or-Pages choice. Copies and collation are the app's own job, so
    // PD_USEDEVMODECOPIESANDCOLLATE stays unset and nCopies carries the truth.
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOCURRENTPAGE;
    if (gDocPages < 2) pd.Flags |= PD_NOPAGENUMS;            // nothing to range over
    // the range control is only usable when the dialog knows the document's
    // real page span: left at 0/0 comdlg32 disables the Pages radio and the
    // From/To boxes outright
    pd.nMinPage = 1;
    pd.nMaxPage = gDocPages;
    pd.nStartPage = START_PAGE_GENERAL;                      // "All" pre-selected
    std::vector<PRINTPAGERANGE> ranges(gDocPages);           // one entry per page at most
    pd.lpPageRanges = ranges.data();
    pd.nMaxPageRanges = gDocPages;
    HRESULT hr = PrintDlgExW(&pd);
    // PrintDlgEx returns S_OK for every non-error outcome and reports the
    // outcome in dwResultAction, so Cancel and Apply are caught here
    if (hr != S_OK || pd.dwResultAction != PD_RESULT_PRINT) {
        if (pd.hDC) DeleteDC(pd.hDC);
        GlobalFree(pd.hDevNames);
        GlobalFree(pd.hDevMode);
        return;
    }
    HDC dc = pd.hDC;
    if (!dc) { GlobalFree(pd.hDevNames); GlobalFree(pd.hDevMode); return; }

    // which pages the user actually asked for; anything out of range is clamped
    std::vector<int> pages;
    std::vector<bool> seen(gDocPages);                       // overlapping ranges print a page once
    if (pd.nPageRanges > 0 && pd.lpPageRanges) {
        for (DWORD r = 0; r < pd.nPageRanges; r++) {
            int a = pd.lpPageRanges[r].nFromPage, b = pd.lpPageRanges[r].nToPage;
            if (a < 1) a = 1;
            if (b > gDocPages) b = gDocPages;
            if (a > b) continue;
            for (int p = a; p <= b; p++)
                if (!seen[p - 1]) { seen[p - 1] = true; pages.push_back(p - 1); }
        }
    }
    if (pages.empty())
        for (int i = 0; i < gDocPages; i++) pages.push_back(i);

    // the printable rectangle in device units, not the whole sheet
    int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    int resX = GetDeviceCaps(dc, HORZRES),          resY = GetDeviceCaps(dc, VERTRES);
    int pw = resX > 0 ? resX : 1, ph = resY > 0 ? resY : 1;

    wchar_t title[MAX_PATH + 32] = L"mnpdf";
    size_t slash = gPath.find_last_of(L"\\/");
    if (!gPath.empty() && slash != std::wstring::npos)
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s - mnpdf", gPath.c_str() + slash + 1);

    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    di.lpszDocName = title;
    // "Print to file" needs FILE: so the subsystem prompts for the output name;
    // without it the job goes to the port the user just declined
    if (pd.Flags & PD_PRINTTOFILE) di.lpszOutput = L"FILE:";
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    // collated emits the whole page set once per copy, non-collated every copy
    // of a page before the next one; the flat index keeps it to one page bitmap
    // at a time with no second copy of the page list
    DWORD copies = pd.nCopies > 0 ? pd.nCopies : 1;
    bool collate = (pd.Flags & PD_COLLATE) != 0;
    size_t sheets = (size_t)copies * pages.size();
    bool job = StartDocW(dc, &di) > 0;
    for (size_t s = 0; s < sheets && job; s++) {
        int pg = collate ? pages[s % pages.size()] : pages[s / copies];
        if (StartPage(dc) <= 0) { job = false; break; }
        // one page bitmap at a time, freed right after it is blitted. The source
        // is a fixed 300 dpi target, never the printer's own LOGPIXELS: a
        // 612x792 page becomes 2550x3300x4 (~34 MB), 1:1 on a 300 dpi printer
        // and a 2x upscale on 600 dpi, while rendering at the device resolution
        // would cost ~135 MB at 600 dpi and ~540 MB at 1200 dpi. The budget
        // caps that transient whatever the page size: Letter at 300 dpi (8.4 MP)
        // is untouched, an A0 poster's ~139 MP is reduced by
        // sqrt(budget/pixels) to about 100 dpi, still legible and still bounded.
        int sw = (int)(rotWpt(pg) * 300.0 / 72.0 + 0.5), sh = (int)(rotHpt(pg) * 300.0 / 72.0 + 0.5);
        double px = (double)sw * sh;
        if (px > kPrintPixelBudget) {
            double shrink = sqrt(kPrintPixelBudget / px);
            sw = (int)(sw * shrink); sh = (int)(sh * shrink);
        }
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        int stride = 0;
        BYTE* buf = rasterizePage(pg, sw, sh, &stride);
        // a page that could not be rasterized fails the job instead of emitting
        // a blank sheet, so the print subsystem reports it
        if (!buf) { job = false; break; }
        // fit inside the printable area, preserve the page's own aspect,
        // centre it: a landscape page on portrait paper letterboxes
        // rather than clipping
        double scale = (double)pw / sw;
        if ((double)ph / sh < scale) scale = (double)ph / sh;
        int dw = (int)(sw * scale), dh = (int)(sh * scale);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int dx = offX + (pw - dw) / 2, dy = offY + (ph - dh) / 2;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = sw;
        bi.bmiHeader.biHeight = -sh;                 // top-down, as written
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        int blitted = StretchDIBits(dc, dx, dy, dw, dh, 0, 0, sw, sh, buf, &bi, DIB_RGB_COLORS, SRCCOPY);
        freeRaster(buf);
        // a rejected blit fails the job exactly like an unallocatable raster:
        // the sheet would otherwise go out blank with the job reporting success.
        // Only GDI_ERROR counts - some drivers return 0 for the scan-line
        // count, and a <= 0 test would abort healthy prints.
        if (blitted == GDI_ERROR) { job = false; break; }
        if (EndPage(dc) <= 0) { job = false; break; }
    }
    if (job) EndDoc(dc);
    else AbortDoc(dc);
    DeleteDC(dc);
    GlobalFree(pd.hDevNames);
    GlobalFree(pd.hDevMode);
}

static bool gVerboseTitle = false;               // MNPDF_VERBOSE: page/zoom/RAM in the title

static void updateTitle() {
    wchar_t t[256];
    if (gVerboseTitle && gDoc) {
        PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
        double mb = 0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
            mb = pmc.PrivateUsage / 1048576.0;
        const wchar_t* dot = gDirty ? L"\x2022 " : L"";
        if (gEditPin >= 0)
            swprintf_s(t, L"mnpdf %d/%d  %.0f%%  %.1f MB  pin text (Enter = new line, Esc saves)",
                       gPageIndex + 1, gPageCount, gZoom * 100.0, mb);
        else if (gSearchOpen && gQuery[0])
            swprintf_s(t, L"mnpdf %d/%d  %.0f%%  %.1f MB  %s %d/%d",
                       gPageIndex + 1, gPageCount, gZoom * 100.0, mb,
                       gQuery, gMatchActive + 1, gMatchCount);
        else
            swprintf_s(t, L"%smnpdf %d/%d  %.0f%%  %.1f MB", dot, gPageIndex + 1, gPageCount, gZoom * 100.0, mb);
    } else {
        swprintf_s(t, L"%smnpdf", gDirty ? L"\x2022 " : L"");
    }
    SetWindowTextW(gWnd, t);
}

struct UpdateResult {
    bool manual = false;
    bool ok = false;
    bool newer = false;
    std::wstring tag;
};

static UpdateResult* gPendingUpdateResult = nullptr;

static std::wstring moduleDirectory() {
    wchar_t path[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"(the current mnpdf folder)";
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

static bool parseVersion(const std::wstring& text, int& major, int& minor, int& patch) {
    const wchar_t* p = text.c_str();
    if (*p == L'v' || *p == L'V') p++;
    return swscanf_s(p, L"%d.%d.%d", &major, &minor, &patch) == 3;
}

static bool versionIsNewer(const std::wstring& candidate) {
    int a = 0, b = 0, c = 0;
    int x = 0, y = 0, z = 0;
    if (!parseVersion(kAppVersion, a, b, c) || !parseVersion(candidate, x, y, z)) return false;
    if (x != a) return x > a;
    if (y != b) return y > b;
    return z > c;
}

static std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), &out[0], n);
    return out;
}

// UTF-16 -> UTF-8 body (no terminator) for the stdio writers and pdfium's argv;
// the conversion used to exist four times over with drifted guards
static std::string wideToUtf8(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], n, nullptr, nullptr);
    out.resize(n > 0 ? n - 1 : 0);
    return out;
}

static long long nowSeconds() { return (long long)time(nullptr); }

// minutes since the last attempt, or -1 when there was none (or the clock moved back)
static int minutesSinceLastCheck() {
    if (gLastUpdateCheck <= 0) return -1;
    long long dt = nowSeconds() - gLastUpdateCheck;
    if (dt < 0) return -1;
    return (int)(dt / 60);
}

static bool updateCheckCooling() {
    int m = minutesSinceLastCheck();
    return m >= 0 && m < kUpdateCheckIntervalMin;
}

// one attempt (hit or miss) stamps the clock, so neither a launch nor a
// right-click can burn more than one request per interval
static void noteUpdateCheck(const std::wstring& tag) {
    gLastUpdateCheck = nowSeconds();
    if (!tag.empty()) gLastUpdateTag = tag;               // remember the newest tag we ever saw
    writeAppPref();
}

// The tag is read off the web host, never api.github.com: that host answers
// "API rate limit exceeded" once the IP's unauthenticated quota is spent, and that
// quota is shared with every other unauthenticated client behind the same IP. This
// runs off the UI thread; no PDF path or document contents ever leave the process.
static std::wstring fetchLatestReleaseTag() {
    const std::wstring userAgent = std::wstring(L"mnpdf/") + kAppVersion;
    const std::wstring latestPath = L"/" + std::wstring(kRepoPath) + L"/releases/latest";
    HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    std::wstring result;
    if (session) {
        WinHttpSetTimeouts(session, 1500, 1500, 3000, 3000);
        connect = WinHttpConnect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect) {
            request = WinHttpOpenRequest(connect, L"GET", latestPath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
        }
    }
    bool good = session && connect && request;
    // "latest" answers with a 302 to the tagged release page, and the tag rides in
    // that redirect target, so no body has to be parsed and no request is spent
    // downloading the page itself. Redirects are disabled so the target can be
    // read from the Location header instead of followed.
    if (good) {
        DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof(noRedirects));
        good = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr);
    }
    if (good) {
        wchar_t location[1024] = L"";
        DWORD bytes = sizeof(location);
        bool haveLocation = WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                                WINHTTP_HEADER_NAME_BY_INDEX,
                                                location, &bytes, WINHTTP_NO_HEADER_INDEX) != FALSE;
        if (haveLocation) {
            std::wstring target = location;
            if (target.empty()) haveLocation = false;
            else if (target[0] == L'/') target = std::wstring(kGitHubRoot) + target;   // site-relative
            else if (target.rfind(kGitHubRoot, 0) != 0) haveLocation = false;          // some other host
            if (haveLocation) {
                const std::wstring marker = L"/releases/tag/";
                size_t tag = target.rfind(marker);
                result = tag == std::wstring::npos ? L"" : target.substr(tag + marker.size());
            }
        }
        good = haveLocation && !result.empty();
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return result;
}

static std::wstring checkedAgoNote(int minutesAgo) {
    if (minutesAgo < 0) return L"";
    wchar_t b[64];
    swprintf_s(b, L"\n\n(Last checked %d minute%s ago.)", minutesAgo, minutesAgo == 1 ? L"" : L"s");
    return b;
}

static void showUpdateResult(const UpdateResult& result, int minutesAgo = -1) {
    if (!result.ok) {
        if (result.manual)
            MessageBoxW(gWnd, L"mnpdf could not check GitHub for updates. Try again later.",
                        L"mnpdf updates", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!result.newer) {
        if (result.manual) {
            std::wstring text = L"mnpdf " + std::wstring(kAppVersion) + L" is up to date."
                              + checkedAgoNote(minutesAgo);
            MessageBoxW(gWnd, text.c_str(), L"mnpdf updates", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    std::wstring text = L"mnpdf " + std::wstring(kAppVersion) + L" -> " + result.tag
        + L" is available." + checkedAgoNote(minutesAgo) + L"\n\n"
        + L"This is a portable ZIP update. Downloading it into another folder creates a second copy; it does not replace this copy.\n\n"
        + L"To update this copy:\n"
        + L"1. Open the official release page below.\n"
        + L"2. Close mnpdf.\n"
        + L"3. Extract the ZIP over:\n   " + moduleDirectory() + L"\n"
        + L"4. Replace mnpdf.exe and pdfium.dll.\n\n"
        + L"Your PDFs, notes, and settings are stored separately and will not be deleted.\n\n"
        + L"Open the official release page now?";
    if (MessageBoxW(gWnd, text.c_str(), L"mnpdf update available",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(gWnd, L"open", kLatestReleaseUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void showPendingUpdateResult() {
    if (UpdateResult* r = gPendingUpdateResult) {
        gPendingUpdateResult = nullptr;
        if (!gShuttingDown.load())
            showUpdateResult(*r);
        delete r;
    }
}

// a click inside the cooldown window is answered from what the last attempt
// learned, so the user still gets a true answer and no request is spent
static void reportCachedUpdateResult() {
    int ago = minutesSinceLastCheck();
    if (ago < 0) ago = 0;
    if (gLastUpdateTag.empty()) {                     // last attempt could not reach GitHub
        int left = kUpdateCheckIntervalMin - ago;
        if (left < 1) left = 1;
        std::wstring text = L"mnpdf checked GitHub for updates recently and could not reach it.\n"
                            L"Please try again in " + std::to_wstring(left) + L" minute";
        if (left != 1) text += L"s";
        text += L".";
        MessageBoxW(gWnd, text.c_str(), L"mnpdf updates", MB_OK | MB_ICONWARNING);
        return;
    }
    UpdateResult cached;
    cached.manual = true;
    cached.ok = true;
    cached.tag = gLastUpdateTag;
    cached.newer = versionIsNewer(cached.tag);
    showUpdateResult(cached, ago);
}

static void startUpdateCheck(bool manual) {
    if (manual) gManualUpdateRequested.store(true);
    if (gUpdateCheckRunning.exchange(true)) return;
    if (updateCheckCooling()) {                       // rate limit ourselves before GitHub does
        gManualUpdateRequested.store(false);           // this click is answered from cache
        gUpdateCheckRunning.store(false);
        if (manual) reportCachedUpdateResult();
        return;
    }
    HWND target = gWnd;
    std::thread([target]() {
        UpdateResult* result = new UpdateResult;
        result->tag = fetchLatestReleaseTag();
        result->ok = !result->tag.empty();
        result->newer = result->ok && versionIsNewer(result->tag);
        result->manual = gManualUpdateRequested.exchange(false);
        if (gShuttingDown.load() || !IsWindow(target)
            || !PostMessageW(target, WM_MNPDF_UPDATE_RESULT, 0, (LPARAM)result)) {
            delete result;
            gUpdateCheckRunning.store(false);
        }
    }).detach();
}

// ---- autosave: per-document reading state (zoom, fit, page) + last file ----

static void appDirW(wchar_t* out, size_t n) {
    const wchar_t* app = _wgetenv(L"APPDATA");
    if (app && *app) _snwprintf(out, n, L"%s\\mnpdf", app);
    else _snwprintf(out, n, L".");
    CreateDirectoryW(out, nullptr);                 // ok if it exists
}

static std::wstring sidecarPathFor(const std::wstring& path) {
    uint64_t h = 1469598103934665603ULL;            // FNV-1a over UTF-16 bytes
    const wchar_t* s = path.c_str();
    for (size_t i = 0; i <= path.size(); i++) {
        wchar_t c = s[i];
        h = (h ^ (unsigned char)(c & 0xff)) * 1099511628211ULL;
        h = (h ^ (unsigned char)((c >> 8) & 0xff)) * 1099511628211ULL;
    }
    wchar_t dir[MAX_PATH], name[64];
    appDirW(dir, MAX_PATH);
    _snwprintf(name, 64, L"\\doc-%016llx.txt", (unsigned long long)h);
    return std::wstring(dir) + name;
}

static const int kSidecarFmt = 2;                // sidecar schema: pin lines carry a colour

static void writeSidecarNow() {
    if (gPath.empty() || !gDoc) return;
    FILE* fp = _wfopen(sidecarPathFor(gPath).c_str(), L"wb");
    if (fp) {
        fprintf(fp, "fmt=%d\nzoom=%.6f\nfit=%d\npage=%d\n", kSidecarFmt, gZoom, gFitWidth ? 1 : 0, gPageIndex + 1);
        for (const Hl& h : gHls)
            fprintf(fp, "hl=%d,%d,%d,%d\n", h.page, h.start, h.count, h.color);
        for (int i = 0; i < gDocPages; i++)
            if (gRot && gRot[i]) fprintf(fp, "rot=%d,%d\n", i, gRot[i]);
        for (const Tomb& tb : gTomb) {
            if (tb.kind == 0) fprintf(fp, "dl=h,%d,%d,%d\n", tb.page, tb.start, tb.count);
            else fprintf(fp, "dl=p,%d,%.2f,%.2f\n", tb.page, tb.x, tb.y);
        }
        for (const Pin& pn : gPins) {
            if (pn.text.empty()) continue;
            std::string esc;                       // multiline-safe: \n stays one line
            for (char p : wideToUtf8(pn.text)) {
                if (p == '\\') esc += "\\\\";
                else if (p == '\n') esc += "\\n";
                else if (p == '\r') continue;
                else esc += p;
            }
            fprintf(fp, "pin=%d,%.2f,%.2f,c%d,%s\n", pn.page, pn.x, pn.y, pn.color, esc.c_str());
        }
        for (const Sig& sg : gSigs) {
            std::string esc;                       // the path carries backslashes
            for (char p : wideToUtf8(sg.path)) {
                if (p == '\\') esc += "\\\\";
                else if (p == ',') esc += "\\c";    // the first comma ends the fields
                else esc += p;
            }
            fprintf(fp, "sig=%d,%.2f,%.2f,%.2f,%.2f,%s\n",
                    sg.page, sg.cx, sg.cy, sg.w, sg.h, esc.c_str());
        }
        fclose(fp);
    }
    wchar_t dir[MAX_PATH];
    appDirW(dir, MAX_PATH);
    FILE* lfp = _wfopen((std::wstring(dir) + L"\\last.txt").c_str(), L"wb");
    if (lfp) {
        std::string u8 = wideToUtf8(gPath);
        fwrite(u8.data(), 1, u8.size(), lfp);
        fputc('\n', lfp);
        fclose(lfp);
    }
}

static void markSave() {
    if (gAutosave) gSaveDirty = true;              // flushed by the WM_TIMER tick
}

// app-level prefs in %APPDATA%\mnpdf\app.txt: chrome toggles (titlebar, autosave),
// the default highlight/pin colours, the custom #rrggbb slots (round-robin pointer
// too), the update-check clock that keeps it to one request per interval, and the
// frame geometry winx/winy/winw/winh/winmax the next launch recreates the window
// with. Every call records the rect as it stands at that moment, so the WM_CLOSE
// write is the one that survives a quit. The rect is read from GetWindowPlacement's
// rcNormalPosition rather than GetWindowRect, because that is the rect the user left
// the window in, whatever state the window is in - maximized, GetWindowRect reports
// the monitor rect the system grew the window to, and minimized the iconic
// placeholder at -32000,-32000 - and the same call carries the maximized flag as
// showCmd == SW_SHOWMAXIMIZED or WPF_RESTORETOMAXIMIZED, so a window minimized from
// a maximized one still comes back maximized.
static void writeAppPref() {
    wchar_t dir[MAX_PATH];
    appDirW(dir, MAX_PATH);
    FILE* fp = _wfopen((std::wstring(dir) + L"\\app.txt").c_str(), L"wb");
    if (fp) {
        fprintf(fp, "titlebar=%d\nautosave=%d\nhlcolor=%d\npincolor=%d\nnight=%d\n",
                gTitlebar ? 1 : 0, gAutosave ? 1 : 0, gHlDefault, gPinDefault, gNight ? 1 : 0);
        RECT wr = { 0, 0, 0, 0 };
        WINDOWPLACEMENT wp2 = { sizeof(wp2) };
        if (gWnd && GetWindowPlacement(gWnd, &wp2)) {
            wr = wp2.rcNormalPosition;
            if ((wr.right - wr.left) > 0 && (wr.bottom - wr.top) > 0) {
                const bool winmax = wp2.showCmd == SW_SHOWMAXIMIZED ||
                                    (wp2.flags & WPF_RESTORETOMAXIMIZED);
                fprintf(fp, "winx=%ld\nwiny=%ld\nwinw=%ld\nwinh=%ld\nwinmax=%d\n",
                        (long)wr.left, (long)wr.top, (long)(wr.right - wr.left), (long)(wr.bottom - wr.top),
                        winmax ? 1 : 0);
            }
        }
        for (int c = 0; c < kPalCustom; c++)
            if (gPalCustom[c] != CLR_INVALID)
                fprintf(fp, "pal%d=%02x%02x%02x\n", c,
                        GetRValue(gPalCustom[c]), GetGValue(gPalCustom[c]), GetBValue(gPalCustom[c]));
        fprintf(fp, "palnext=%d\n", gPalNext);
        if (gLastUpdateCheck > 0)
            fprintf(fp, "updcheck=%lld\nupdtag=%s\n", gLastUpdateCheck, wideToUtf8(gLastUpdateTag).c_str());
        fclose(fp);
    }
}

static void loadAppPref() {
    wchar_t dir[MAX_PATH];
    appDirW(dir, MAX_PATH);
    FILE* fp = _wfopen((std::wstring(dir) + L"\\app.txt").c_str(), L"rb");
    if (!fp) return;
    char line[64];
    while (fgets(line, 64, fp)) {
        int iv;
        long long llv;
        if (sscanf(line, "titlebar=%d", &iv) == 1) gTitlebar = iv != 0;
        else if (sscanf(line, "autosave=%d", &iv) == 1) gAutosave = iv != 0;
        else if (sscanf(line, "night=%d", &iv) == 1) gNight = iv != 0;
        // geometry is bounded the same way the colour keys below are: a value
        // a window cannot have leaves the field at its default
        else if (sscanf(line, "winx=%d", &iv) == 1) { if (iv >= -100000 && iv <= 100000) gWinX = iv; }
        else if (sscanf(line, "winy=%d", &iv) == 1) { if (iv >= -100000 && iv <= 100000) gWinY = iv; }
        else if (sscanf(line, "winw=%d", &iv) == 1) { if (iv > 0 && iv <= 32767) gWinW = iv; }
        else if (sscanf(line, "winh=%d", &iv) == 1) { if (iv > 0 && iv <= 32767) gWinH = iv; }
        else if (sscanf(line, "winmax=%d", &iv) == 1) gWinMax = iv != 0;
        else if (sscanf(line, "hlcolor=%d", &iv) == 1) gHlDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "pincolor=%d", &iv) == 1) gPinDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "palnext=%d", &iv) == 1) gPalNext = (iv >= 0 && iv < kPalCustom) ? iv : 0;
        else if (sscanf(line, "updcheck=%lld", &llv) == 1) gLastUpdateCheck = llv > 0 ? llv : 0;
        else if (strncmp(line, "updtag=", 7) == 0) {
            char* v = line + 7;
            size_t len = strlen(v);
            while (len && (v[len - 1] == '\n' || v[len - 1] == '\r')) v[--len] = 0;
            gLastUpdateTag = utf8ToWide(std::string(v, len));
        }
        else if (strncmp(line, "pal", 3) == 0 && line[4] == '=') {
            unsigned int r2, g2, b2;
            if (sscanf(line + 5, "%2x%2x%2x", &r2, &g2, &b2) == 3) {
                int slot = line[3] - '0';
                if (slot >= 0 && slot < kPalCustom) gPalCustom[slot] = RGB(r2, g2, b2);
            }
        }
    }
    fclose(fp);
}

static void toggleTitlebar(HWND h) {
    gTitlebar = !gTitlebar;
    LONG st = (LONG)GetWindowLongPtrW(h, GWL_STYLE);
    if (gTitlebar) st |= WS_CAPTION;
    else st &= ~WS_CAPTION;
    SetWindowLongPtrW(h, GWL_STYLE, (LONG_PTR)st);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    writeAppPref();
    renderPage();
}

// one line of any length: a fixed fgets buffer would silently split a pin
// line that carries kNoteMaxChars of UTF-8 text (the writer can emit several
// KB), truncating the note and parsing the tail as garbage. Reads until \n or
// EOF, strips the trailing newline and any \r, and NUL-terminates.
static bool readSidecarLine(FILE* fp, std::vector<char>& buf) {
    size_t len = 0;
    int c;
    bool any = false;
    while ((c = fgetc(fp)) != EOF) {
        any = true;
        if (c == '\n') break;
        if (len + 1 >= buf.size()) buf.resize(buf.size() * 2);
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = 0;
    return any;
}

static void restoreSidecar(const std::wstring& path) {
    FILE* fp = _wfopen(sidecarPathFor(path).c_str(), L"rb");
    if (!fp) return;
    double zoom = -1;
    int fit = -1, page = -1, fmt = 0;
    std::vector<char> lineBuf(512);
    while (readSidecarLine(fp, lineBuf)) {
        char* line = lineBuf.data();
        double d;
        int a, b, c, d2;
        if (sscanf(line, "zoom=%lf", &d) == 1) zoom = d;
        else if (sscanf(line, "fmt=%d", &a) == 1) fmt = a;
        else if (sscanf(line, "fit=%d", &a) == 1) fit = a;
        else if (sscanf(line, "page=%d", &b) == 1) page = b;
        else if (strncmp(line, "dl=h,", 5) == 0) {
            int q, r, u;
            if (sscanf(line + 5, "%d,%d,%d", &q, &r, &u) == 3 && q >= 0 && q < gDocPages)
                gTomb.push_back({ 0, q, r, u, 0, 0 });
        } else if (strncmp(line, "dl=p,", 5) == 0) {
            int q;
            double xx, yy;
            if (sscanf(line + 5, "%d,%lf,%lf", &q, &xx, &yy) == 3 && q >= 0 && q < gDocPages)
                gTomb.push_back({ 1, q, 0, 0, xx, yy });
        }
        else if (sscanf(line, "hl=%d,%d,%d,%d", &a, &b, &c, &d2) == 4) {
            if (a >= 0 && a < gDocPages && b >= 0 && c > 0) {
                int col = d2 >= 0 && d2 < kPalCount ? d2 : 0;
                // already held from the file itself? identity is page/start/count,
                // but the sidecar also carries the colour, and where the two
                // disagree the sidecar is the newer decision: a recolour that is
                // still unsaved must not quietly revert to the baked colour
                bool have = false;
                for (int k = (int)gHls.size() - 1; k >= 0; k--) {
                    Hl& h = gHls[k];
                    if (h.baked && h.page == a && h.start == b && h.count == c) {
                        have = true;
                        if (h.color != col) {          // the file's colour is stale
                            gHls.erase(gHls.begin() + k);
                            have = false;              // the sidecar entry takes its place
                        }
                        break;
                    }
                }
                if (!have) gHls.push_back({ a, b, c, col });
            }
        } else if (sscanf(line, "rot=%d,%d", &a, &b) == 2) {
            if (a >= 0 && a < gDocPages && b >= 0 && b < 4) gRot[a] = b;
        } else if (strncmp(line, "sig=", 4) == 0) {
            int pg;
            double cx, cy, ww, hh;
            int after = 0;      // %n: characters the five fields consumed, path included
            // six fields: page, cx, cy, w, h, then the path, which may itself
            // hold commas (an escaped \c) - so the path starts where the scan
            // stopped, not at a hand-counted comma
            if (sscanf(line + 4, "%d,%lf,%lf,%lf,%lf,%n", &pg, &cx, &cy, &ww, &hh, &after) == 5
                && after > 0 && pg >= 0 && pg < gDocPages) {
                std::string esc(line + 4 + after);
                std::string un;
                for (size_t q = 0; q < esc.size(); q++) {
                    if (esc[q] == '\\' && q + 1 < esc.size()) {
                        if (esc[q + 1] == 'c') { un += ','; q++; continue; }
                        if (esc[q + 1] == '\\') { un += '\\'; q++; continue; }
                    }
                    un += esc[q];
                }
                Sig sg;
                sg.page = pg; sg.cx = cx; sg.cy = cy;
                sg.w = ww > 4 ? ww : 150; sg.h = hh > 4 ? hh : 60;
                int wn = MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, nullptr, 0);
                std::vector<wchar_t> wt(wn);
                MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, wt.data(), wn);
                sg.path = wt.data();
                gSigs.push_back(sg);
            }
        } else if (strncmp(line, "pin=", 4) == 0) {
            int c1 = -1, c2 = -1, c3 = -1;             // first three commas split the fields
            for (int k = 4; line[k]; k++) {
                if (line[k] == ',') { if (c1 < 0) c1 = k; else if (c2 < 0) c2 = k; else { c3 = k; break; } }
            }
            int pp;
            double xx, yy;
            if (c3 > 0 && sscanf(line + 4, "%d,%lf,%lf", &pp, &xx, &yy) == 3 && pp >= 0 && pp < gDocPages) {
                // format 2 writes a "cN" colour field after the third comma; a
                // sidecar without the fmt line is pre-colour, where the note
                // itself follows that comma and the pin keeps the red it had
                int pcol = kPalPreset - 1;
                const char* txt = line + c3 + 1;
                if (fmt >= kSidecarFmt) {
                    const char* col = line + c3 + 1;
                    if (col[0] == 'c') {
                        char* end = nullptr;
                        long cv = strtol(col + 1, &end, 10);
                        if (end > col + 1 && (*end == ',' || !*end) && cv >= 0 && cv < kPalCount) {
                            pcol = (int)cv;
                            txt = *end == ',' ? end + 1 : end;
                        }
                    }
                }
                std::string esc(txt);                  // \n escapes -> real newlines
                std::string un;
                for (size_t q = 0; q < esc.size(); q++) {
                    if (esc[q] == '\\' && q + 1 < esc.size()) {
                        if (esc[q + 1] == 'n') { un += '\n'; q++; continue; }
                        if (esc[q + 1] == '\\') { un += '\\'; q++; continue; }
                    }
                    un += esc[q];
                }
                int wn = MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, nullptr, 0);
                std::vector<wchar_t> wt(wn);
                MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, wt.data(), wn);
                // already held from the file itself? same rule as the highlights:
                // identity is page/point, and a note or colour the sidecar knows
                // and the file does not yet is the newer state - it wins
                bool have = false;
                for (int k = (int)gPins.size() - 1; k >= 0; k--) {
                    const Pin& p = gPins[k];
                    if (p.baked && p.page == pp && fabs(p.x - xx) < 2.0 && fabs(p.y - yy) < 2.0) {
                        have = true;
                        if (p.color != pcol || p.text != wt.data()) {
                            gPins.erase(gPins.begin() + k);
                            have = false;
                        }
                        break;
                    }
                }
                if (!have) gPins.push_back({ pp, xx, yy, wt.data(), pcol, false });
            }
        }
    }
    fclose(fp);
    normalizeHls();                                   // older builds could stack overlapping hl entries
    normalizePins();                                  // and sidecars could double annotation-restored pins
    for (const Tomb& tb : gTomb) {                    // a deleted baked mark stays out
        if (tb.kind == 0) {
            for (int k = (int)gHls.size() - 1; k >= 0; k--)
                if (gHls[k].page == tb.page && gHls[k].start == tb.start && gHls[k].count == tb.count)
                    { gHls.erase(gHls.begin() + k); break; }
        } else {
            for (int k = (int)gPins.size() - 1; k >= 0; k--)
                if (gPins[k].page == tb.page && fabs(gPins[k].x - tb.x) < 2.0 && fabs(gPins[k].y - tb.y) < 2.0)
                    { gPins.erase(gPins.begin() + k); break; }
        }
    }
    relayoutPages();                                  // rotations may have changed heights
    if (fit == 1) { gFitWidth = true; applyFitWidth(); }
    else if (zoom > 0) { gZoom = clampZoom(zoom); gFitWidth = false; }
    if (page >= 1 && page <= gDocPages) {
        loadPage(page - 1);
        gScrollY = yTopPx(page - 1) - MARGIN;      // page top at the viewport top
        gRestoredPage = true;                      // wins over the center rule until a scroll
    }
}

// the colour Save baked into a mark's annotation. pdfium refuses annotations that
// already carry an appearance stream, so then the caller's seed is the answer
static void annotColor(FPDF_ANNOTATION a, unsigned& r, unsigned& g, unsigned& b) {
    unsigned cr = 0, cg = 0, cb = 0, ca = 255;
    if (FPDFAnnot_GetColor(a, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca)) {
        r = cr; g = cg; b = cb;
    }
}

// nearest palette entry for a colour, the custom slots beside the presets (an
// unused slot never matches, since palColor rejects it)
static int nearestPalColor(unsigned r, unsigned g, unsigned b) {
    int color = -1, best = 1 << 30;
    for (int c = 0; c < kPalCount; c++) {
        COLORREF pc;
        if (!palColor(c, pc)) continue;
        int d = (int)((r - GetRValue(pc)) * (r - GetRValue(pc))
                    + (g - GetGValue(pc)) * (g - GetGValue(pc))
                    + (b - GetBValue(pc)) * (b - GetBValue(pc)));
        if (d < best) { best = d; color = c; }
    }
    return color;
}

static bool openPath(const std::wstring& path) {
    if (gEditPin >= 0) commitPinEdit();            // an open pin box saves first
    // last.txt names the CURRENT document (autosave rewrites it constantly), so
    // "reopen last" cannot read it: the document being left is the previous one
    if (!gPath.empty() && gPath != path) gPrevPath = gPath;
    gTextPage = nullptr;                           // handles live in the cache now
    gPage = nullptr;
    flushPageCache();
    if (gDoc) FPDF_CloseDocument(gDoc); gDoc = nullptr;
    // pdfium wants UTF-8; Windows gave us UTF-16
    std::string u8 = wideToUtf8(path);
    gDoc = FPDF_LoadDocument(u8.c_str(), nullptr);
    if (!gDoc) { MessageBoxW(gWnd, L"Could not open PDF", L"mnpdf", MB_ICONERROR); return false; }
    gPath = path;
    gPageCount = FPDF_GetPageCount(gDoc);
    gHls.clear();
    gPins.clear();
    gSigs.clear();                                // stamps belong to their document
    gSigPlacing.clear();                          // a half-finished placement dies here
    gUndo.clear();
    gRedo.clear();
    gDirty = false;
    gPageTextPt.assign(gPageCount > 0 ? gPageCount : 1, 0.0);   // body text pt, measured lazily
    // gather per-page sizes + rotation once; everything else derives from them
    delete[] gPageW; delete[] gPageH; delete[] gPrefixPt; delete[] gRot;
    gDocPages = gPageCount;
    gPageW = new double[gDocPages ? gDocPages : 1];
    gPageH = new double[gDocPages ? gDocPages : 1];
    gPrefixPt = new double[gDocPages + 1];
    gRot = new int[gDocPages ? gDocPages : 1];
    for (int i = 0; i < gDocPages; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        double w = p ? FPDF_GetPageWidthF(p) : 612;
        double h = p ? FPDF_GetPageHeightF(p) : 792;
        gRot[i] = p ? FPDFPage_GetRotation(p) & 3 : 0;
        // annotations from a previous Save are still live marks: read them back
        // into gHls/gPins and remove them from the in-memory doc, so a re-Save
        // re-bakes exactly the marks that exist now (no duplicates, deletable)
        int na = p ? FPDFPage_GetAnnotCount(p) : 0;
        FPDF_TEXTPAGE tp = nullptr;
        if (p && na) tp = FPDFText_LoadPage(p);
        for (int k = na - 1; k >= 0; k--) {
            FPDF_ANNOTATION a = p ? FPDFPage_GetAnnot(p, k) : nullptr;
            if (!a) continue;
            int ty = FPDFAnnot_GetSubtype(a);
            if (ty != FPDF_ANNOT_HIGHLIGHT && ty != FPDF_ANNOT_TEXT) { FPDFPage_CloseAnnot(a); continue; }
            wchar_t src[64] = L"";
            bool ours = FPDFAnnot_GetStringValue(a, "Source", (FPDF_WCHAR*)src, sizeof(src)) > 2
                        && wcsncmp(src, L"mnpdf", 5) == 0;
            // every annot must be closed exactly once, on every path: a document
            // carrying foreign annotations (other readers' notes, form widgets)
            // leaks one CPDF_Annot per annot per open otherwise
            if (!ours) { FPDFPage_CloseAnnot(a); continue; }   // foreign note: untouched
            bool lifted = false;
            if (ty == FPDF_ANNOT_HIGHLIGHT && tp) {
                unsigned R = 255, G = 230, B = 128;   // yellow, the palette's first entry
                annotColor(a, R, G, B);
                int color = nearestPalColor(R, G, B);
                int qs = -1, qc = 0;
                if (swscanf(src, L"mnpdf %d %d", &qs, &qc) == 2 && qs >= 0 && qc > 0) {
                    bool have = false;                 // one entry per range, not per rect
                    for (const Hl& h : gHls)
                        if (h.baked && h.page == i && h.start == qs && h.count == qc) { have = true; break; }
                    if (!have) gHls.push_back({ i, qs, qc, color, true });
                    lifted = true;
                }
            } else if (ty == FPDF_ANNOT_TEXT && p) {
                FS_RECTF rc;
                unsigned PR = 230, PG = 71, PB = 77;   // historic pin red
                annotColor(a, PR, PG, PB);
                if (FPDFAnnot_GetRect(a, &rc)) {
                    unsigned long need = FPDFAnnot_GetStringValue(a, "Contents", nullptr, 0);
                    std::wstring txt;
                    if (need > 1) {
                        std::vector<wchar_t> buf(need / 1);
                        if (FPDFAnnot_GetStringValue(a, "Contents", (FPDF_WCHAR*)buf.data(), need) > 0) {
                            txt.assign(buf.data(), need / sizeof(wchar_t));
                            while (!txt.empty() && txt.back() == L'\0') txt.pop_back();
                        }
                    }
                    if (!txt.empty()) {
                        int pcol = nearestPalColor(PR, PG, PB);
                        gPins.push_back({ i, ((double)rc.left + rc.right) / 2, ((double)rc.top + rc.bottom) / 2, txt, pcol, true });
                        lifted = true;
                    }
                }
            }
            FPDFPage_CloseAnnot(a);
            if (lifted) FPDFPage_RemoveAnnot(p, k);   // ours: lives in gHls/gPins now
        }
        if (tp) FPDFText_ClosePage(tp);
        if (p) FPDF_ClosePage(p);
        if (gRot[i] & 1) { double t2 = w; w = h; h = t2; }   // store unrotated
        gPageW[i] = w;
        gPageH[i] = h;
    }
    normalizeHls();                                   // overlapping file marks collapse now,
    normalizePins();                                  // sidecar or no sidecar
    relayoutPages();
    gPageWpt = 612; gPageHpt = 792;
    gScrollX = gScrollY = 0;
    gRestoredPage = false;   // a fresh open starts from page 1, not from the previous document's restore
    gFitWidth = true;
    applyFitWidth();
    loadPage(0);
    std::vector<Hl> hlFile = gHls;                   // what the file itself held
    std::vector<Pin> pinFile = gPins;
    restoreSidecar(path);                          // saved zoom/page wins over defaults
    replaySigs();                                  // sidecar stamps drop into the pages now
    if (!gTomb.empty()) {
        markDirty();                                // a baked mark has a pending deletion
    } else {
        for (const Hl& h : gHls) {                  // sidecar edits beyond the file?
            bool found = false;
            for (const Hl& f : hlFile)
                if (h.page == f.page && h.start == f.start && h.count == f.count && h.color == f.color) found = true;
            if (!found) markDirty();
        }
        for (const Pin& p : gPins) {                // and pins (note or colour) beyond it?
            bool found = false;
            for (const Pin& f : pinFile)
                if (f.page == p.page && fabs(f.x - p.x) < 2.0 && fabs(f.y - p.y) < 2.0
                    && f.text == p.text && f.color == p.color) found = true;
            if (!found) markDirty();
        }
    }
    clampScroll();
    renderPage();
    refreshDrawers();                              // an open drawer follows the new document
    writeSidecarNow();                             // records last.txt too
    return true;
}

static void openDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"PDF\0*.pdf\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) openPath(file);
}

// ---- save: bake highlights (Highlight annotations) and pins (Text annotations) plus
// rotations into the PDF with pdfium itself, then write and reload clean ----

struct SaveFW : FPDF_FILEWRITE {
    std::vector<unsigned char>* out;
};

static int WriteBlockCb(FPDF_FILEWRITE* pThis, const void* data, unsigned long len) {
    auto* me = (SaveFW*)pThis;
    const unsigned char* d = (const unsigned char*)data;
    me->out->insert(me->out->end(), d, d + len);
    return 1;
}

static bool bakeAndWrite(const std::wstring& target) {
    if (!gDoc || !gDocPages) return false;
    gTextPage = nullptr;
    gPage = nullptr;
    flushPageCache();
    bool ok = true;
    for (int i = 0; i < gDocPages && ok; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        if (!p) break;
        // strip what an earlier Save baked: a replaced highlight must vanish
        // from the file, not pile up beside its replacement (overlapping
        // annotations render twice wherever no sidecar can normalize them).
        // A mark is dropped only when the current in-memory mark replaces it,
        // so one the read-back could not lift survives instead of vanishing.
        for (int k = FPDFPage_GetAnnotCount(p) - 1; k >= 0; k--) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, k);
            if (!a) continue;
            int ty = FPDFAnnot_GetSubtype(a);
            wchar_t src[64] = L"";
            bool ours = FPDFAnnot_GetStringValue(a, "Source", (FPDF_WCHAR*)src, sizeof(src)) > 2
                        && wcsncmp(src, L"mnpdf", 5) == 0;
            bool replaced = false;
            if (ours && ty == FPDF_ANNOT_HIGHLIGHT) {
                int qs = -1, qc = 0;
                if (swscanf(src, L"mnpdf %d %d", &qs, &qc) == 2 && qs >= 0 && qc > 0)
                    for (const Hl& hl : gHls)
                        if (hl.page == i && hl.start == qs && hl.count == qc) { replaced = true; break; }
            } else if (ours && ty == FPDF_ANNOT_TEXT) {
                FS_RECTF arc;
                if (FPDFAnnot_GetRect(a, &arc))
                    for (const Pin& pn : gPins)
                        if (pn.page == i && !pn.text.empty()
                            && fabs(((double)arc.left + arc.right) / 2 - pn.x) < 1.5
                            && fabs(((double)arc.top + arc.bottom) / 2 - pn.y) < 1.5) { replaced = true; break; }
            }
            FPDFPage_CloseAnnot(a);
            if (replaced) FPDFPage_RemoveAnnot(p, k);       // re-baked below from gHls/gPins
        }
        FPDFPage_SetRotation(p, gRot[i] * 90);
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(p);
        if (tp) {
            for (const Hl& hl : gHls) {
                if (hl.page != i) continue;
                int nr = FPDFText_CountRects(tp, hl.start, hl.count);
                for (int k = 0; k < nr; k++) {
                    double l, t, r, b;
                    if (!FPDFText_GetRect(tp, k, &l, &t, &r, &b)) continue;
                    FPDF_ANNOTATION a = FPDFPage_CreateAnnot(p, FPDF_ANNOT_HIGHLIGHT);
                    if (!a) continue;
                    FS_RECTF rc = { (float)l, (float)t, (float)r, (float)b };
                    FPDFAnnot_SetRect(a, &rc);
                    FS_QUADPOINTSF q = { (float)l, (float)t, (float)r, (float)t, (float)l, (float)b, (float)r, (float)b };
                    FPDFAnnot_AppendAttachmentPoints(a, &q);
                    COLORREF bc;
                    if (!palColor(hl.color, bc)) bc = gPalPreset[0];
                    FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color,
                                       GetRValue(bc), GetGValue(bc), GetBValue(bc), 255);
                    wchar_t src[48];
                    _snwprintf(src, 48, L"mnpdf %d %d", hl.start, hl.count);
                    FPDFAnnot_SetStringValue(a, "Source", (FPDF_WIDESTRING)src);
                    FPDFPage_CloseAnnot(a);
                }
            }
        }
        for (const Pin& pn : gPins) {
            if (pn.page != i || pn.text.empty()) continue;
            FPDF_ANNOTATION a = FPDFPage_CreateAnnot(p, FPDF_ANNOT_TEXT);
            if (a) {
                FS_RECTF rc = { (float)(pn.x - 12), (float)(pn.y - 12), (float)(pn.x + 12), (float)(pn.y + 12) };
                FPDFAnnot_SetRect(a, &rc);
                FPDFAnnot_SetStringValue(a, "Contents", (FPDF_WIDESTRING)pn.text.c_str());
                COLORREF pcol2;
                if (!palColor(pn.color, pcol2)) pcol2 = gPalPreset[kPalPreset - 1];
                FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color,
                                   GetRValue(pcol2), GetGValue(pcol2), GetBValue(pcol2), 255);
                FPDFAnnot_SetStringValue(a, "Source", (FPDF_WIDESTRING)L"mnpdf");
                FPDFPage_CloseAnnot(a);
            }
        }
        if (tp) FPDFText_ClosePage(tp);
        FPDF_ClosePage(p);
    }
    if (!ok) return false;
    std::vector<unsigned char> out;
    SaveFW fw = {};
    fw.version = 1;
    fw.WriteBlock = WriteBlockCb;
    fw.out = &out;
    if (!FPDF_SaveAsCopy(gDoc, &fw, 0) || out.empty()) return false;
    FILE* fp = _wfopen(target.c_str(), L"wb");
    if (!fp) return false;
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return true;
}

static bool doSaveImpl(const std::wstring& target) {
    if (!gDoc) return false;
    if (!gDirty) return true;
    if (bakeAndWrite(target)) {
        gHls.clear();                              // highlights now live in the file
        gPins.clear();                             // so do pins
        gTomb.clear();                             // deletions are baked in now
        gSigs.clear();                             // so do stamps: they are page objects
        gDirty = false;
        writeSidecarNow();                         // clean sidecar BEFORE the reload,
        openPath(target);                          // so restore doesn't resurrect edits
        return true;
    }
    MessageBoxW(gWnd, L"Save failed", L"mnpdf", MB_ICONERROR);
    return false;
}

static bool doSave() {
    if (!gDoc) return false;
    if (!gDirty) return true;
    if (gPath.empty()) return doSaveAs();
    return doSaveImpl(gPath);
}

static bool doSaveAs() {
    if (!gDoc) return false;
    wchar_t file[MAX_PATH] = L"";
    if (!gPath.empty()) { wcsncpy_s(file, gPath.c_str(), MAX_PATH - 1); }
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"PDF\0*.pdf\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return false;
    gPath = file;
    return doSaveImpl(file);
}

// one colour submenu for every mark kind: the presets, the custom slots in use,
// then a Custom... row that asks for a #rrggbb. presetBase/customBase/customId
// are the command ids the caller handles.
static HMENU colorSubmenu(int current, int presetBase, int customBase, int customId) {
    HMENU sub = CreatePopupMenu();
    for (int c = 0; c < kPalPreset; c++)
        AppendMenuW(sub, MF_STRING | (current == c ? MF_CHECKED : 0), presetBase + c, gPalName[c]);
    bool any = false;
    for (int c = 0; c < kPalCustom; c++) {
        if (gPalCustom[c] == CLR_INVALID) continue;
        AppendMenuW(sub, MF_STRING | (current == kPalPreset + c ? MF_CHECKED : 0),
                    customBase + c, palLabel(kPalPreset + c).c_str());
        any = true;
    }
    if (any) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    if (any) AppendMenuW(sub, MF_STRING, 180, L"Clear custom colours");
    AppendMenuW(sub, MF_STRING, customId, L"Custom...");
    return sub;
}

// ---- custom colour entry -------------------------------------------------
// a one-line popup that asks for a #rrggbb. The colour lands in the first
// matching custom slot, else in a round-robin one, so three of the reader's
// own colours sit beside the presets for both pins and highlights.
static HWND gColorBox = nullptr;
static WNDPROC gColorBoxBase = nullptr;
static DWORD gColorBoxBorn = 0;
// the error popup steals activation from the box; this keeps the box's
// focus-loss close from firing underneath it
static bool gColorErrBusy = false;

static void closeColorBox();
static bool parseHexColor(const std::wstring& in, COLORREF& out);
static void applyCustomColor(COLORREF c);

static void closeColorBox() {
    if (gColorBox) { HWND b = gColorBox; gColorBox = nullptr; DestroyWindow(b); }
}

// read what is in the box and, if it names a colour, keep it: a reader who typed
// a colour and then clicked away asked for that colour, so dropping it would
// mean the app silently refusing to provide it
static void commitColorBox() {
    if (!gColorBox) return;
    wchar_t buf[16];
    int n = GetWindowTextLengthW(gColorBox);
    if (n > 15) n = 15;
    GetWindowTextW(gColorBox, buf, n + 1);
    COLORREF c;
    bool keep = parseHexColor(buf, c);
    closeColorBox();
    if (keep) applyCustomColor(c);
}

// a committed entry that is not a colour: say what is wrong now, while the text
// is still on screen in front of the reader, and leave the box open to retype
static void showColorError(HWND box, const wchar_t* text) {
    gColorErrBusy = true;
    wchar_t shown[24];
    int n = (int)wcslen(text);
    if (n > 20) n = 20;
    wmemcpy(shown, text, (size_t)n);
    shown[n] = 0;
    wchar_t msg[220];
    _snwprintf_s(msg, _TRUNCATE,
                 L"\"%s\" is not a colour.\n\n"
                 L"Type six hex digits, with or without the #:\n\n"
                 L"    #ff4d00\n    ff4d00\n    FF4D00",
                 shown);
    MessageBoxW(box, msg, L"mnpdf", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    gColorErrBusy = false;
    SetForegroundWindow(box);
    SetFocus(box);
    SendMessageW(box, EM_SETSEL, 0, -1);      // select all: retyping replaces it
}

// "#a1b2c3", "a1b2c3" and "A1B2C3" all mean the same colour
static bool parseHexColor(const std::wstring& in, COLORREF& out) {
    unsigned int v = 0;
    int digits = 0;
    for (size_t i = 0; i < in.size(); i++) {
        wchar_t c = in[i];
        if (c == L'#' || c == L' ') continue;
        int d;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
        else return false;
        v = v * 16 + (unsigned int)d;
        if (++digits > 6) return false;
    }
    if (digits != 6) return false;
    out = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    return true;
}

// file an entered colour and hand it to whatever asked for it
static bool anyCustomColor() {
    for (int c = 0; c < kPalCustom; c++) if (gPalCustom[c] != CLR_INVALID) return true;
    return false;
}

// hand every custom slot back: with only three slots a reader who wants a
// different set has to be able to start from empty. A mark still pointing at a
// cleared slot takes the preset for its kind, so it stays visible and keeps its
// undo history rather than turning yellow-coincidence garbage.
static void clearCustomColors() {
    bool had = anyCustomColor();
    for (int c = 0; c < kPalCustom; c++) gPalCustom[c] = CLR_INVALID;
    for (int c = 0; c < kPalCustom; c++) releaseHiHl(kPalPreset + c);
    if (!had) return;
    // a default that named a cleared slot would draw the fallback colour anyway,
    // so point the defaults at the preset for their kind from the start
    if (gHlDefault >= kPalPreset) gHlDefault = 0;
    if (gPinDefault >= kPalPreset) gPinDefault = kPalPreset - 1;
    int hlBack = gHlDefault;
    int pinBack = gPinDefault;
    for (Hl& hl : gHls) if (hl.color >= kPalPreset) hl.color = hlBack;
    for (Pin& pn : gPins) if (pn.color >= kPalPreset) pn.color = pinBack;
    gPalNext = 0;
    writeAppPref();
    markSave();
    renderPage();
}

static void applyCustomColor(COLORREF c) {
    int slot = -1;
    for (int k = 0; k < kPalCustom; k++)
        if (gPalCustom[k] == c) { slot = k; break; }
    if (slot < 0) {                               // a new colour: round-robin the three slots
        slot = gPalNext;
        gPalNext = (gPalNext + 1) % kPalCustom;
    }
    gPalCustom[slot] = c;
    releaseHiHl(kPalPreset + slot);              // marks on that slot repaint now
    int idx = kPalPreset + slot;
    switch (gColorTarget) {
    case 0: recolorHighlight(gMenuHl, idx); break;
    case 1: recolorPin(gMenuPin, idx); break;
    case 2:
        gHlDefault = idx;
        if (hasSelection()) applyHighlightFromSelection(idx);
        break;
    default: gPinDefault = idx; break;
    }
    writeAppPref();
}

static void startColorEntry() {
    if (gColorBox) closeColorBox();
    if (gEditPin >= 0) commitPinEdit();
    hidePinTip(gWnd);
    int cw = clientW(), ch = clientH();
    int bw = 190, bh = 30;
    RECT wr;
    GetWindowRect(gWnd, &wr);
    int bx = wr.left + (cw - bw) / 2, by = wr.top + (ch - bh) / 2;
    bx = clampBox(bx, bw, wr.left + 4, wr.right - 4);
    by = clampBox(by, bh, wr.top + 4, wr.bottom - 4);
    // the '#' is already in the box, so the reader is told the shape of the
    // answer instead of having to guess whether the # belongs there
    gColorBox = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"#",
                                WS_POPUP | WS_BORDER | ES_AUTOHSCROLL, bx, by, bw, bh,
                                gWnd, nullptr,
                                (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE), nullptr);
    if (!gColorBox) return;
    gColorBoxBorn = GetTickCount();
    SendMessageW(gColorBox, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(gColorBox, EM_LIMITTEXT, 7, 0);
    gColorBoxBase = (WNDPROC)SetWindowLongPtrW(gColorBox, GWLP_WNDPROC, (LONG_PTR)colorBoxProc);
    ShowWindow(gColorBox, SW_SHOW);
    SendMessageW(gColorBox, EM_SETSEL, 1, 1);   // caret just past the '#'
    SetForegroundWindow(gColorBox);
    SetFocus(gColorBox);
}

static LRESULT CALLBACK colorBoxProc(HWND b, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CHAR:
        // only hex digits ever get in, so the box cannot hold a mistake that went
        // past the reader; backspace and editing keys still pass
        if (wp >= 0x20 && wp != L'#') {
            bool hex = (wp >= L'0' && wp <= L'9') || (wp >= L'a' && wp <= L'f')
                    || (wp >= L'A' && wp <= L'F');
            if (!hex) { MessageBeep(0); return 0; }
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { closeColorBox(); return 0; }
        if (wp == VK_RETURN) {
            wchar_t buf[16];
            int n = GetWindowTextLengthW(b);
            if (n > 15) n = 15;
            GetWindowTextW(b, buf, n + 1);
            COLORREF c;
            if (parseHexColor(buf, c)) {
                closeColorBox();                   // gone before the mark repaints
                applyCustomColor(c);
            } else {
                showColorError(b, buf);            // keep asking, box stays open
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(gColorBoxBase, b, m, wp, lp);
        int len = GetWindowTextLengthW(b);
        if (len <= 1) {                            // only the seed '#': show the rest
            HDC hdc = GetDC(b);
            if (hdc) {
                RECT rc;
                GetClientRect(b, &rc);
                InflateRect(&rc, -3, -2);
                int ox = 0;                        // leave room for the '#'
                SIZE sz;
                if (GetTextExtentPoint32W(hdc, L"#", 1, &sz)) ox = sz.cx + 3;
                rc.left += ox;
                HFONT old = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
                COLORREF oldCol = SetTextColor(hdc, RGB(150, 150, 150));
                int oldBk = SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"rrggbb", -1, &rc, DT_TOP | DT_LEFT | DT_NOCLIP | DT_SINGLELINE);
                SetBkMode(hdc, oldBk);
                SetTextColor(hdc, oldCol);
                SelectObject(hdc, old);
                ReleaseDC(b, hdc);
            }
        }
        return r;
    }
    case WM_KILLFOCUS:
        if (gColorErrBusy) return 0;
        // 400 ms grace: the box ignores a focus loss it did not ask for, so a
        // stray keystroke cannot become a colour. The pin editor holds the
        // same grace and test-colors.ps1 waits out the same 400 ms before it
        // clicks away - the three agree by design.
        if (GetTickCount() - gColorBoxBorn > 400) commitColorBox();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            if (gColorErrBusy) return 0;
            if (GetTickCount() - gColorBoxBorn > 400) commitColorBox();
        }
        break;
    }
    return CallWindowProcW(gColorBoxBase, b, m, wp, lp);
}

// ---- the four big dispatchers, lifted out of wndProc ----------------------
// (review: wndProc owned ~20 message types with four inline dispatchers; the
// bodies below are the same code, one indent level flatter)

// vertical scroll by dy px; the restored-page flag dies here: scrolling is the
// user taking over from a sidecar-restored position
static void scrollByDy(int dy) {
    gScrollY += dy;
    gRestoredPage = false;                 // from here the center rule applies again
    clampScroll();
    ensureActivePage();
}

// ---- side drawers, page editing, signatures --------------------------------
// (everything below is additive: it reuses the page geometry helpers, the save
// path and the sidecar, and touches no existing behaviour when its commands are
// never issued)

// ---- signature stamps ------------------------------------------------------
// (Sig, gSigs and gSigPlacing live with the other document globals)

// pdfium reads the JPEG through a file-access struct, so the image goes into
// memory first; the Inline loader copies it into the document, which is what
// lets the bytes die here instead of living as long as the page
struct JpegBlob {
    std::vector<unsigned char> bytes;
};

static int jpegGetBlock(void* param, unsigned long position, unsigned char* buf,
                        unsigned long size) {
    JpegBlob* blob = (JpegBlob*)param;
    if (position + size > blob->bytes.size()) return 0;
    memcpy(buf, blob->bytes.data() + position, size);
    return 1;
}

static bool readWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// A JPEG is a chain of length-prefixed segments running from SOI to EOI, and
// that chain is the only honest test. Three magic bytes also fit a stream the
// loader cannot decode - entropy data cut off, garbage behind the header, a
// PNG body behind an FFD8FF prefix - and that loader reports success on
// exactly those, so the stamp reaches the page and the sidecar and the reader
// dies at the first paint, taking every later launch of the document with it.
// Anything else - a PNG, a GIF, a text file the reader renamed - has to be
// refused here for the same reason.
static bool looksLikeJpeg(const std::wstring& path) {
    std::vector<unsigned char> b;
    if (!readWholeFile(path, b)) return false;
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return false;
    bool sawScan = false;
    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF) return false;
        // any number of 0xFF fill bytes may precede a marker (ITU T.81
        // B.1.1.2), so a run is one marker, not a marker code of its own
        size_t j = i + 1;
        while (j < b.size() && b[j] == 0xFF) j++;
        if (j >= b.size()) return false;
        unsigned char m = b[j];
        i = j + 1;
        if (m == 0xD9) return sawScan;                          // EOI ends the image
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;    // TEM and restarts carry no length
        if (i + 1 >= b.size()) return false;
        size_t len = ((size_t)b[i] << 8) | b[i + 1];            // a segment length counts itself
        if (len < 2 || i + len > b.size()) return false;
        i += len;
        if (m != 0xDA) continue;
        // SOS: entropy-coded data runs on until the next marker that is neither
        // a stuffed FF00 nor a restart - and a fill run ahead of that marker
        // belongs to the marker, not to the data
        sawScan = true;
        while (i < b.size()) {
            if (b[i] != 0xFF) { i++; continue; }
            size_t e = i + 1;
            while (e < b.size() && b[e] == 0xFF) e++;
            if (e >= b.size()) { i = b.size(); break; }   // trailing FFs: no EOI follows
            if (b[e] == 0x00) { i = e + 1; continue; }    // a stuffed byte, not a marker
            if (b[e] >= 0xD0 && b[e] <= 0xD7) { i = e + 1; continue; }   // a restart
            break;                                       // a marker ends the entropy run
        }
    }
    return false;                                               // ran out before an EOI
}

// drop a stamp into gDoc at its recorded spot. Returns false when the image
// cannot be placed (the file moved away).
static bool placeSigObj(const Sig& s) {
    if (!gDoc || s.page < 0 || s.page >= gDocPages) return false;
    if (!looksLikeJpeg(s.path)) return false;   // never hand non-JPEG bytes to pdfium
    FPDF_PAGE pg = acquirePage(s.page);
    if (!pg) return false;
    JpegBlob blob;
    if (!readWholeFile(s.path, blob.bytes)) return false;
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(gDoc);
    if (!obj) return false;
    FPDF_FILEACCESS fa = {};
    fa.m_FileLen = (unsigned long)blob.bytes.size();
    fa.m_GetBlock = jpegGetBlock;
    fa.m_Param = &blob;
    if (!FPDFImageObj_LoadJpegFileInline(nullptr, 0, obj, &fa)) {
        // a moved or deleted image must not leave a hollow box in the document
        FPDFPageObj_Destroy(obj);
        return false;
    }
    FPDFImageObj_SetMatrix(obj, s.w, 0, 0, s.h, s.cx - s.w / 2, s.cy - s.h / 2);
    // FPDFPage_InsertObject takes ownership, and frees the object itself when it
    // fails: freeing it here as well would be a double free
    if (!FPDFPage_InsertObject(pg, obj)) return false;
    FPDFPage_GenerateContent(pg);
    return true;
}

// stamps come back from the sidecar only after the pages exist
static void replaySigs() {
    // an unreadable image drops that stamp and nothing else: the stamps placed
    // before it are already in the pages, and dropping the whole set would
    // leave them drawn with no record of them anywhere
    bool any = false;
    for (size_t i = 0; i < gSigs.size(); ) {
        if (placeSigObj(gSigs[i])) { i++; any = true; }
        else gSigs.erase(gSigs.begin() + i);
    }
    // The stamp is now in the page and the PDF on disk does not carry it, so the
    // document really is unsaved: without this the title shows a clean document,
    // Save does nothing, and the stamp is lost at the next quit.
    if (any) markDirty();
}

// the reader clicked: turn the click into PDF user space, keep the stamp on the
// page, and hand it to the document
static void commitSigPlacement(int page, double sx, double sy) {
    double x, y;
    pagePxToPt(page, sx, sy, &x, &y);       // handles rotation for us
    double W = gPageW[page], H = gPageH[page];
    Sig s;
    s.page = page;
    s.path = gSigPlacing;
    s.w = 150; s.h = 60;
    if (s.w > W * 0.8) s.w = W * 0.8;
    if (s.h > H * 0.8) s.h = H * 0.8;
    double lox = s.w / 2, hix = W - s.w / 2, loy = s.h / 2, hiy = H - s.h / 2;
    if (hix < lox) { lox = hix = W / 2; }
    if (hiy < loy) { loy = hiy = H / 2; }
    s.cx = x < lox ? lox : (x > hix ? hix : x);
    s.cy = y < loy ? loy : (y > hiy ? hiy : y);
    if (!placeSigObj(s)) {
        MessageBoxW(gWnd, L"That image could not be placed.", L"mnpdf", MB_OK | MB_ICONWARNING);
        return;
    }
    gSigs.push_back(s);
    gSigPlacing.clear();
    markDirty();
    writeSidecarNow();
    renderPage();
}

// Clearing has to remove the page objects themselves: a stamp lives inside
// gDoc, not only in the sidecar, so forgetting the sidecar would leave it drawn
// and saved. The objects are found by the box each stamp occupies - no handle is
// kept across page reloads, which would dangle the moment the cache evicts the
// page - and the walk goes backwards because removing shifts the indices.
static void clearSignatures() {
    if (gSigs.empty()) return;
    std::vector<Sig> stamps = gSigs;
    for (int i = 0; i < gDocPages && !stamps.empty(); i++) {
        bool anyHere = false;
        for (const Sig& s : stamps) if (s.page == i) { anyHere = true; break; }
        if (!anyHere) continue;
        FPDF_PAGE pg = acquirePage(i);
        if (!pg) continue;
        for (int k = FPDFPage_CountObjects(pg) - 1; k >= 0; k--) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pg, k);
            if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;
            float bl, bb, br, bt;
            if (!FPDFPageObj_GetBounds(obj, &bl, &bb, &br, &bt)) continue;
            for (size_t n = 0; n < stamps.size(); n++) {
                const Sig& s = stamps[n];
                if (s.page != i) continue;
                if (fabs((double)bl - (s.cx - s.w / 2)) < 0.25
                    && fabs((double)bb - (s.cy - s.h / 2)) < 0.25
                    && fabs((double)br - (s.cx + s.w / 2)) < 0.25
                    && fabs((double)bt - (s.cy + s.h / 2)) < 0.25) {
                    // ownership of a removed object comes back to the caller, so
                    // what was taken off the page has to be freed here
                    if (FPDFPage_RemoveObject(pg, obj)) FPDFPageObj_Destroy(obj);
                    break;
                }
            }
        }
        FPDFPage_GenerateContent(pg);
    }
    gSigs.clear();
    flushPageCache();
    markDirty();
    writeSidecarNow();
    renderPage();
}

static void pickSignatureImage() {
    wchar_t buf[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"JPEG image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"Choose a signature image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!looksLikeJpeg(buf)) {
        MessageBoxW(gWnd, L"A signature has to be a JPEG image. That file is not one.",
                    L"Place signature", MB_OK | MB_ICONWARNING);
        return;
    }
    gSigPlacing = buf;
    MessageBoxW(gWnd, L"Click the page where the signature should sit. Esc cancels.",
                L"Place signature", MB_OK | MB_ICONINFORMATION);
}

static HWND gThumbWnd = nullptr;
static HWND gOutlineWnd = nullptr;
static int gThumbSel = -1;                      // page picked in the thumbnails drawer

// The drawers show the document as it is now, so anything that changes the
// pages has to rebuild them: a thumbnail cache built before a delete would keep
// showing a page that is gone, and outline entries carry page indices that a
// move invalidates. A drawer that is not open has nothing to rebuild.
static void refreshThumbDrawer();
static void refreshOutlineDrawer();
static void refreshDrawers() { refreshThumbDrawer(); refreshOutlineDrawer(); }

// ---- document surgery ------------------------------------------------------
// Every structural change rebuilds gDoc through FPDF_ImportPages, then re-reads
// the page tables. Page-indexed marks travel with the permutation that produced
// the new document: page import copies a page's content stream, so a stamp
// drawn into a page is still drawn after the rebuild.
static void adoptDoc(FPDF_DOCUMENT nd, int oldCount) {
    flushPageCache();
    // page import fills in the page tree and leaves the rest of the catalog
    // empty, so the one catalog entry the API can carry over goes over here,
    // while the document that still holds it is open
    if (gDoc) FPDF_CopyViewerPreferences(nd, gDoc);
    if (gDoc) FPDF_CloseDocument(gDoc);
    gDoc = nd;
    gDocPages = FPDF_GetPageCount(gDoc);
    gPageCount = gDocPages;          // the title reads this one
    int* oldRot = new int[oldCount ? oldCount : 1];
    for (int i = 0; i < oldCount; i++) oldRot[i] = (i < gDocPages) ? gRot[i] : 0;
    delete[] gPageW; delete[] gPageH; delete[] gPrefixPt; delete[] gRot;
    gPageW = new double[gDocPages ? gDocPages : 1];
    gPageH = new double[gDocPages ? gDocPages : 1];
    gPrefixPt = new double[gDocPages + 1];
    gRot = new int[gDocPages ? gDocPages : 1];
    for (int i = 0; i < gDocPages; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        double w = p ? FPDF_GetPageWidthF(p) : 612;
        double h = p ? FPDF_GetPageHeightF(p) : 792;
        int fileRot = p ? (FPDFPage_GetRotation(p) & 3) : 0;
        // pdfium answers the size questions already turned by the page's own
        // /Rotate, so the swap that stores the unrotated size (openPath does the
        // same) follows THAT rotation - not ours, which is set on the next line
        if (fileRot & 1) { double t2 = w; w = h; h = t2; }
        gRot[i] = (i < oldCount) ? oldRot[i] : fileRot;   // our rotation, not the file's
        if (p) FPDF_ClosePage(p);
        gPageW[i] = w;
        gPageH[i] = h;
    }
    delete[] oldRot;
    flushPageCache();
    gPageTextPt.assign(gDocPages ? gDocPages : 1, 0.0);   // body text pt, measured lazily
    relayoutPages();
    if (gPageIndex >= gDocPages) gPageIndex = gDocPages - 1;
    if (gPageIndex < 0) gPageIndex = 0;
    loadPage(gPageIndex);
    gScrollY = yTopPx(gPageIndex) - MARGIN;
    clampScroll();
    gUndo.clear();
    gRedo.clear();
    markDirty();
    writeSidecarNow();
    renderPage();
    refreshDrawers();
}

// reorder/delete pages: order[newIndex] = oldIndex
static void replaceDocByOrder(const std::vector<int>& order) {
    int n = (int)order.size();
    if (n <= 0) return;          // a full permutation keeps the count: only an empty set is wrong
    if (gEditPin >= 0) commitPinEdit();   // an open note lands on the pin it edits, before indices move
    std::vector<int> back(gDocPages, -1);
    for (int j = 0; j < n; j++) back[order[j]] = j;
    std::vector<int> rot(gDocPages, 0);
    for (int j = 0; j < n; j++) rot[j] = gRot[order[j]];
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    if (!FPDF_ImportPagesByIndex(nd, gDoc, order.data(), (unsigned long)n, 0)) {
        FPDF_CloseDocument(nd);
        MessageBoxW(gWnd, L"Those pages could not be reordered.", L"mnpdf", MB_OK | MB_ICONWARNING);
        return;
    }
    // marks follow the permutation; a deleted page takes its marks with it
    for (size_t k = 0; k < gHls.size();) {
        int np = (gHls[k].page >= 0 && gHls[k].page < gDocPages) ? back[gHls[k].page] : -1;
        if (np < 0) gHls.erase(gHls.begin() + k); else { gHls[k].page = np; k++; }
    }
    for (size_t k = 0; k < gPins.size();) {
        int np = (gPins[k].page >= 0 && gPins[k].page < gDocPages) ? back[gPins[k].page] : -1;
        if (np < 0) gPins.erase(gPins.begin() + k); else { gPins[k].page = np; k++; }
    }
    for (size_t k = 0; k < gTomb.size();) {
        int np = (gTomb[k].page >= 0 && gTomb[k].page < gDocPages) ? back[gTomb[k].page] : -1;
        if (np < 0) gTomb.erase(gTomb.begin() + k); else { gTomb[k].page = np; k++; }
    }
    for (size_t k = 0; k < gSigs.size();) {
        int np = (gSigs[k].page >= 0 && gSigs[k].page < gDocPages) ? back[gSigs[k].page] : -1;
        if (np < 0) gSigs.erase(gSigs.begin() + k); else { gSigs[k].page = np; k++; }
    }
    // the selection names its pages by number too: it moves with the permutation,
    // and one that lost a page has nothing left to paint, copy or highlight
    if (gSelAnchorPage >= 0 || gSelHeadPage >= 0) {
        int na = (gSelAnchorPage >= 0 && gSelAnchorPage < gDocPages) ? back[gSelAnchorPage] : -1;
        int nh = (gSelHeadPage >= 0 && gSelHeadPage < gDocPages) ? back[gSelHeadPage] : -1;
        if (na < 0 || nh < 0) clearSelection();
        else { gSelAnchorPage = na; gSelHeadPage = nh; }
    }
    // search hits travel the same way; their char offsets still fit because the
    // page text came across verbatim, but a hit on a deleted page is simply gone
    bool hitKept = false;
    int w = 0;
    for (int m = 0; m < gMatchCount; m++) {
        int np = (gMatches[m].page >= 0 && gMatches[m].page < gDocPages) ? back[gMatches[m].page] : -1;
        if (np < 0) continue;
        gMatches[w] = gMatches[m];
        gMatches[w].page = np;
        if (m == gMatchActive) hitKept = true;
        w++;
    }
    gMatchCount = w;
    if (!hitKept) gMatchActive = gMatchCount ? 0 : -1;
    updateMatchLabel();                  // a dropped hit changes the counter the find bar shows
    delete[] gRot;
    gRot = new int[gDocPages ? gDocPages : 1];
    for (int j = 0; j < n; j++) gRot[j] = rot[j];
    for (int j = n; j < gDocPages; j++) gRot[j] = 0;
    adoptDoc(nd, gDocPages);                  // the drawer selection follows the reader
}

static void deletePage(int idx) {
    if (idx < 0 || idx >= gDocPages || gDocPages <= 1) return;
    std::vector<int> order;
    for (int i = 0; i < gDocPages; i++) if (i != idx) order.push_back(i);
    replaceDocByOrder(order);
}

static void movePage(int from, int to) {
    if (from < 0 || from >= gDocPages || to < 0 || to >= gDocPages || from == to) return;
    std::vector<int> order;
    for (int i = 0; i < gDocPages; i++) order.push_back(i);
    int v = order[from];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, v);
    replaceDocByOrder(order);                      // adoptDoc refreshes the drawers
}

// the reader picks the signature's spot; the click never reaches the text layer
static bool sigPlacementAt(int x, int y, int* pageOut) {
    if (gSigPlacing.empty() || !gDoc) return false;
    for (int i = 0; i < gDocPages; i++) {
        int px = pageVx(i), py = pageVy(i);
        int w = pageWpx(i), h = pageHpx(i);
        if (x >= px && x < px + w && y >= py && y < py + h) {
            *pageOut = i;
            commitSigPlacement(i, x - px, y - py);
            return true;
        }
    }
    MessageBeep(0);
    return true;                            // consumed: the click was still a placement try
}

// ---- merge and split -------------------------------------------------------
static void mergeFiles(const std::vector<std::wstring>& files) {
    if (!gDoc || files.empty()) return;
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    int at = 0;
    if (!FPDF_ImportPages(nd, gDoc, nullptr, at)) {
        FPDF_CloseDocument(nd);
        MessageBoxW(gWnd, L"The current document could not be merged.", L"mnpdf", MB_OK | MB_ICONWARNING);
        return;
    }
    at = FPDF_GetPageCount(nd);
    for (const std::wstring& f : files) {
        std::string u8 = wideToUtf8(f);
        FPDF_DOCUMENT src = FPDF_LoadDocument(u8.c_str(), nullptr);
        if (!src) continue;                 // one unreadable file must not sink the merge
        int n = FPDF_GetPageCount(src);
        if (n > 0) FPDF_ImportPages(nd, src, nullptr, at);   // one bad file must not sink the merge
        at = FPDF_GetPageCount(nd);
        FPDF_CloseDocument(src);
    }
    if (FPDF_GetPageCount(nd) <= gDocPages) {
        FPDF_CloseDocument(nd);
        MessageBoxW(gWnd, L"No pages could be added.", L"mnpdf", MB_OK | MB_ICONWARNING);
        return;
    }
    adoptDoc(nd, gDocPages);                 // current pages keep their indices
    writeAppPref();
}

static void mergeDialog() {
    if (!gDoc) return;
    std::vector<BYTE> buf(64 * 1024, 0);
    wchar_t* file = (wchar_t*)buf.data();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"PDF files (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(buf.size() / 2);
    ofn.nMaxFileTitle = (DWORD)(buf.size() / 2);
    ofn.lpstrTitle = L"Add files to the end of this document";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> files;
    std::wstring dir = file;
    size_t slash = dir.find_last_of(L"\\/:");
    dir = (slash == std::wstring::npos) ? L"" : dir.substr(0, slash + 1);
    for (const wchar_t* p = file + ofn.nFileOffset; *p; ) {
        std::wstring one = dir + p;
        p += wcslen(p) + 1;
        if (!one.empty()) files.push_back(one);
    }
    if (files.empty()) return;
    mergeFiles(files);
}

// export a page range ("2,5-7") into a new file next to the Save As dialog
static void splitExport(const std::wstring& target, const std::string& range, bool quiet) {
    if (!gDoc) return;
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    if (!FPDF_ImportPages(nd, gDoc, range.c_str(), 0)) {
        FPDF_CloseDocument(nd);
        if (!quiet)
            MessageBoxW(gWnd, L"That page range could not be read. Try 2-5 or 1,3,8.",
                        L"Split pages", MB_OK | MB_ICONWARNING);
        return;
    }
    std::vector<unsigned char> bytes;
    SaveFW fw = {};
    fw.version = 1;                       // the two FPDF_FILEWRITE fields pdfium calls
    fw.WriteBlock = WriteBlockCb;
    fw.out = &bytes;
    bool wrote = FPDF_SaveAsCopy(nd, &fw, 0) && !bytes.empty();
    if (wrote) {
        FILE* f = _wfopen(target.c_str(), L"wb");
        if (!f) wrote = false;            // a folder the reader cannot write to
        else {
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    }
    FPDF_CloseDocument(nd);
    if (quiet) return;
    if (wrote) MessageBoxW(gWnd, L"Pages exported.", L"Split pages", MB_OK | MB_ICONINFORMATION);
    else MessageBoxW(gWnd, L"These pages could not be written to that file.",
                     L"Split pages", MB_OK | MB_ICONWARNING);
}

// ---- the range prompt for Split -------------------------------------------
static HWND gRangeWnd = nullptr;
static std::wstring gRangeText = L"1-1";

static void closeRangePrompt() {
    if (gRangeWnd) { HWND w = gRangeWnd; gRangeWnd = nullptr; DestroyWindow(w); }
}

static LRESULT CALLBACK rangeProc(HWND w, UINT m, WPARAM wp, LPARAM lp);

static void openRangePrompt() {
    if (gRangeWnd) { SetForegroundWindow(gRangeWnd); return; }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = rangeProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MNRange";
        RegisterClassExW(&wc);
        reg = true;
    }
    RECT wr;
    GetWindowRect(gWnd, &wr);
    int w = 300, h = 110;
    int x = wr.left + ((wr.right - wr.left) - w) / 2;
    int y = wr.top + ((wr.bottom - wr.top) - h) / 2;
    gRangeWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"MNRange", L"Split pages",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h,
                                gWnd, nullptr, inst, nullptr);
    if (!gRangeWnd) return;
    ShowWindow(gRangeWnd, SW_SHOW);
}

static LRESULT CALLBACK rangeProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE);
        CreateWindowExW(0, L"STATIC", L"Pages to export (1-based):", WS_CHILD | WS_VISIBLE,
                        12, 12, 260, 18, w, nullptr, inst, nullptr);
        CreateWindowExW(0, L"EDIT", gRangeText.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                        12, 34, 260, 24, w, (HMENU)101, inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Choose file...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        12, 66, 120, 26, w, (HMENU)102, inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        152, 66, 120, 26, w, (HMENU)103, inst, nullptr);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 103) { closeRangePrompt(); return 0; }
        if (id == 102 || id == IDOK) {
            wchar_t buf[64] = {};
            GetDlgItemTextW(w, 101, buf, 64);
            std::wstring text = buf;
            // trim
            while (!text.empty() && (text.front() == L' ')) text.erase(text.begin());
            while (!text.empty() && (text.back() == L' ')) text.pop_back();
            if (text.empty()) { MessageBeep(0); return 0; }
            std::vector<BYTE> nb(MAX_PATH * 4, 0);
            wchar_t* target = (wchar_t*)nb.data();
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = w;
            ofn.lpstrFilter = L"PDF files (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0";
            ofn.lpstrFile = target;
            ofn.nMaxFile = MAX_PATH * 4;
            ofn.lpstrTitle = L"Export the selected pages to";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return 0;
            closeRangePrompt();
            splitExport(target, wideToUtf8(text), false);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        closeRangePrompt();
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---- thumbnails drawer -----------------------------------------------------
static std::vector<HBITMAP> gThumbBmps;
static std::vector<int> gThumbHs;
static double gThumbScroll = 0;

static const int kThumbW = 108;             // drawer thumbnail width in px
static const int kThumbGap = 6;
static const int kOutlineW = 240;           // outline drawer width in px
static const int kDrawerGap = 8;            // drawer to reader gap in px

static void clearThumbCache() {
    for (HBITMAP b : gThumbBmps) if (b) DeleteObject(b);
    gThumbBmps.clear();
    gThumbHs.clear();
}

// A row's height is arithmetic on the page boxes, so every page gets one at
// once; its picture does not. Rasterizing every page up front would parse and
// draw the whole document before the drawer appears - and again after every
// delete, move and rotate - so a row is drawn when it first reaches the screen
// (thumbProc's paint) and kept until the pages change underneath it.
static void layoutThumbs() {
    clearThumbCache();
    if (!gDoc) return;
    for (int i = 0; i < gDocPages; i++) {
        double ar = (gPageH[i] > 0 && gPageW[i] > 0) ? gPageH[i] / gPageW[i] : 1.3;
        int h = (int)(kThumbW * ar + 0.5);
        if (h < 8) h = 8;
        if (h > 400) h = 400;
        gThumbBmps.push_back(nullptr);       // not drawn yet
        gThumbHs.push_back(h);
    }
}

// rasterize one row; a row that already has a picture keeps it
static void renderThumb(int i) {
    if (i < 0 || i >= (int)gThumbBmps.size() || gThumbBmps[i]) return;
    int h = gThumbHs[i];
    int stride = 0;
    BYTE* buf = rasterizePage(i, kThumbW, h, &stride);
    if (!buf) return;
    HDC screen = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kThumbW;
    bmi.bmiHeader.biHeight = -h;              // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bmp && bits)
        for (int y = 0; y < h; y++)
            memcpy((BYTE*)bits + y * kThumbW * 4, buf + y * stride, (size_t)kThumbW * 4);
    freeRaster(buf);
    gThumbBmps[i] = bmp;
}

// a rotation changed what one row shows, so that row's picture is stale
static void dropThumb(int i) {
    if (i < 0 || i >= (int)gThumbBmps.size()) return;
    if (gThumbBmps[i]) { DeleteObject(gThumbBmps[i]); gThumbBmps[i] = nullptr; }
}

// A drawer hangs off the reader's own rect, and that rect moves: a maximized
// window leaves no screen beside it, so a drawer placed there is off screen and
// unreachable while its menu row still reads as open. Both drawers go through
// here, so a move or a resize lands them on the work area of the monitor the
// reader is on.
static void placeDrawer(HWND drawer, int width, int side) {
    if (!drawer || !gWnd) return;
    RECT wr;
    GetWindowRect(gWnd, &wr);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int h = wr.bottom - wr.top - 40;
    if (h > mi.rcWork.bottom - mi.rcWork.top) h = mi.rcWork.bottom - mi.rcWork.top;
    if (h < 200) h = 200;
    int x = side ? wr.right + kDrawerGap : wr.left - width - kDrawerGap;
    SetWindowPos(drawer, nullptr,
                 clampBox(x, width, mi.rcWork.left, mi.rcWork.right),
                 clampBox(wr.top + 40, h, mi.rcWork.top, mi.rcWork.bottom),
                 width, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void placeDrawers() {
    placeDrawer(gThumbWnd, kThumbW + 24, 0);
    placeDrawer(gOutlineWnd, kOutlineW, 1);
}

static int thumbListHeight() {
    int total = 0;
    for (int hh : gThumbHs) total += hh + kThumbGap;
    return total;
}

// client y -> page index under it, or -1
static int thumbAt(int x, int y) {
    int cy = 4 - (int)gThumbScroll;
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
}

static LRESULT CALLBACK thumbProc(HWND w, UINT m, WPARAM wp, LPARAM lp);

static void toggleThumbDrawer() {
    if (!gDoc) return;
    gThumbsOn = !gThumbsOn;
    if (gThumbWnd) {
        HWND dead = gThumbWnd;
        gThumbWnd = nullptr;
        DestroyWindow(dead);
        gThumbScroll = 0;
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = thumbProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MNThumbs";
        RegisterClassExW(&wc);
        reg = true;
    }
    gThumbSel = gPageIndex;
    gThumbWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"MNThumbs", L"Thumbnails",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                0, 0, kThumbW + 24, 200,
                                gWnd, nullptr, inst, nullptr);
    if (gThumbWnd) {
        layoutThumbs();            // rows first; each picture is drawn when it shows
        placeDrawer(gThumbWnd, kThumbW + 24, 0);
        ShowWindow(gThumbWnd, SW_SHOW);
    }
}

static void invalidateThumbDrawer() {
    if (gThumbWnd) InvalidateRect(gThumbWnd, nullptr, FALSE);
}

static void refreshThumbDrawer() {
    if (!gThumbWnd) return;
    gThumbSel = gPageIndex;
    layoutThumbs();
    invalidateThumbDrawer();
}

static LRESULT CALLBACK thumbProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        HDC mem = CreateCompatibleDC(dc);
        int cy = 4 - (int)gThumbScroll;
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SetBkMode(dc, TRANSPARENT);
        for (size_t i = 0; i < gThumbBmps.size(); i++) {
            int th = gThumbHs[i];
            if (cy + th >= 0 && cy <= rc.bottom) {
                int x = (rc.right - kThumbW) / 2;
                if ((int)i == gThumbSel) {
                    RECT sel = { x - 2, cy - 2, x + kThumbW + 2, cy + th + 2 };
                    FillRect(dc, &sel, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
                }
                if (!gThumbBmps[i]) renderThumb((int)i);
                if (gThumbBmps[i]) {
                    HBITMAP old = (HBITMAP)SelectObject(mem, gThumbBmps[i]);
                    BitBlt(dc, x, cy, kThumbW, th, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                }
                wchar_t num[16];
                _snwprintf_s(num, 16, _TRUNCATE, L"%d", (int)i + 1);
                RECT nr = { x, cy + th + 1, x + kThumbW, cy + th + 15 };
                DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_SINGLELINE);
            }
            cy += th + kThumbGap;
        }
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int idx = thumbAt(x, y);
        if (idx >= 0) {
            gThumbSel = idx;
            openPageAt(idx);
            InvalidateRect(w, nullptr, FALSE);
        }
        SetFocus(w);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        gThumbScroll -= d / WHEEL_DELTA * 60;
        if (gThumbScroll < 0) gThumbScroll = 0;
        RECT rc;
        GetClientRect(w, &rc);
        int maxScroll = thumbListHeight() - (rc.bottom - rc.top);
        if (maxScroll < 0) maxScroll = 0;
        if (gThumbScroll > maxScroll) gThumbScroll = maxScroll;
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        if (gThumbSel < 0) break;
        if (wp == VK_DELETE) deletePage(gThumbSel);
        else if (wp == VK_UP && (GetKeyState(VK_CONTROL) & 0x8000)) movePage(gThumbSel, gThumbSel - 1);
        else if (wp == VK_DOWN && (GetKeyState(VK_CONTROL) & 0x8000)) movePage(gThumbSel, gThumbSel + 1);
        else if (wp == 'R') { rotatePage(gThumbSel, +1); dropThumb(gThumbSel); }
        else if (wp == VK_UP) { if (gThumbSel > 0) gThumbSel--; openPageAt(gThumbSel); }
        else if (wp == VK_DOWN) { if (gThumbSel < gDocPages - 1) gThumbSel++; openPageAt(gThumbSel); }
        else break;
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(w);                  // WM_DESTROY clears the state and the cache
        return 0;
    case WM_DESTROY:
        if (gThumbWnd == w) { gThumbWnd = nullptr; gThumbsOn = false; }
        clearThumbCache();
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---- outline drawer --------------------------------------------------------
struct BmEntry { std::wstring title; int page; int depth; };
static std::vector<BmEntry> gBmList;
static int gBmSel = -1;

static void collectBookmarks(FPDF_BOOKMARK bm, int depth) {
    for (; bm; bm = FPDFBookmark_GetNextSibling(gDoc, bm)) {
        unsigned int len = FPDFBookmark_GetTitle(bm, nullptr, 0);
        std::vector<unsigned short> buf(len + 1, 0);
        if (len) FPDFBookmark_GetTitle(bm, buf.data(), (int)buf.size());
        BmEntry e;
        e.title = (const wchar_t*)buf.data();
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

static const int kBmRow = 20;

static LRESULT CALLBACK outlineProc(HWND w, UINT m, WPARAM wp, LPARAM lp);

static void toggleOutlineDrawer() {
    if (!gDoc) return;
    gOutlineOn = !gOutlineOn;
    if (gOutlineWnd) {
        HWND dead = gOutlineWnd;
        gOutlineWnd = nullptr;
        DestroyWindow(dead);
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = outlineProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MNOutline";
        RegisterClassExW(&wc);
        reg = true;
    }
    buildBmList();
    gOutlineWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"MNOutline", L"Outline",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                  0, 0, kOutlineW, 200,
                                  gWnd, nullptr, inst, nullptr);
    if (gOutlineWnd) {
        placeDrawer(gOutlineWnd, kOutlineW, 1);
        ShowWindow(gOutlineWnd, SW_SHOW);
    }
}

static void invalidateOutlineDrawer() {
    if (gOutlineWnd) InvalidateRect(gOutlineWnd, nullptr, FALSE);
}

static void refreshOutlineDrawer() {
    if (!gOutlineWnd) return;
    buildBmList();
    invalidateOutlineDrawer();
}

static LRESULT CALLBACK outlineProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        if (gBmList.empty()) {
            RECT e = rc;
            InflateRect(&e, -8, -8);
            SetTextColor(dc, RGB(120, 120, 120));
            DrawTextW(dc, L"This document has no bookmarks.", -1, &e,
                      DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_SINGLELINE);
        } else {
            HFONT old = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            int y = 4;
            for (size_t i = 0; i < gBmList.size(); i++) {
                if (y > rc.bottom) break;
                RECT row = { 4 + gBmList[i].depth * 12, y, rc.right - 4, y + kBmRow };
                if ((int)i == gBmSel) FillRect(dc, &row, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
                DrawTextW(dc, gBmList[i].title.c_str(), -1, &row,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                y += kBmRow;
            }
            SelectObject(dc, old);
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int y = GET_Y_LPARAM(lp);
        int idx = (y - 4) / kBmRow;
        if (idx >= 0 && idx < (int)gBmList.size()) {
            gBmSel = idx;
            if (gBmList[idx].page >= 0) openPageAt(gBmList[idx].page);
            InvalidateRect(w, nullptr, FALSE);
        }
        SetFocus(w);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        if (gOutlineWnd == w) { gOutlineWnd = nullptr; gOutlineOn = false; }
        gBmList.clear();
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---- test hook ------------------------------------------------------------
// A posted-message harness cannot click a modal file dialog, so the probe calls
// the same code the menu calls with the arguments the menu would have collected:
// MNPDF_HOOK="merge:<pdf>", "sig:<jpeg>:<page>", "split:<range>:<out.pdf>",
// "delpage:<n>", "movepage:<n>:<m>", "rotatepage:<n>". Every verb runs the real
// function, then leaves the window up so the suite can measure the result.
static void runHook(const std::wstring& spec);

static void runHook(const std::wstring& spec) {
    if (!gDoc || spec.empty()) return;
    size_t cut = spec.find(L";;");                 // "a|b;;c|d" runs a, then c
    if (cut != std::wstring::npos) {
        runHook(spec.substr(0, cut));
        runHook(spec.substr(cut + 2));
        return;
    }
    std::vector<std::wstring> parts;
    size_t at = 0;
    while (parts.size() < 3) {
        size_t c = spec.find(L'|', at);
        if (c == std::wstring::npos) { parts.push_back(spec.substr(at)); break; }
        parts.push_back(spec.substr(at, c - at));
        at = c + 1;
    }
    const std::wstring& verb = parts[0];
    auto num = [](const std::wstring& t, int dflt) -> int {
        return t.empty() ? dflt : _wtoi(t.c_str());
    };
    const std::wstring arg1 = parts.size() > 1 ? parts[1] : L"";
    const std::wstring arg2 = parts.size() > 2 ? parts[2] : L"";
    if (verb == L"merge" && !arg1.empty()) {
        mergeFiles({ arg1 });
    } else if (verb == L"sig" && !arg1.empty()) {
        Sig sg;
        sg.page = num(arg2, 0);
        if (sg.page >= 0 && sg.page < gDocPages) {
            sg.path = arg1;
            sg.cx = gPageW[sg.page] / 2;
            sg.cy = gPageH[sg.page] / 2;
            if (placeSigObj(sg)) { gSigs.push_back(sg); markDirty(); writeSidecarNow(); renderPage(); }
        }
    } else if (verb == L"split" && parts.size() >= 3 && !arg2.empty()) {
        splitExport(arg2, wideToUtf8(arg1), true);
    } else if (verb == L"delpage") {
        deletePage(num(arg1, -1));
    } else if (verb == L"movepage") {
        movePage(num(arg1, -1), num(arg2, -1));
    } else if (verb == L"rotatepage") {
        rotatePage(num(arg1, -1), num(arg2, 1) >= 0 ? +1 : -1);
    } else if (verb == L"open" && !arg1.empty()) {
        openPath(arg1);                            // the swap Reopen last has to undo
    } else if (verb == L"clearsigs") {
        clearSignatures();
    } else if (verb == L"pagehash" && !arg1.empty()) {
        FILE* f = _wfopen(arg1.c_str(), L"wb");
        if (f) {
            for (int i = 0; i < gDocPages; i++) {
                int stride = 0;
                BYTE* buf = rasterizePage(i, rotWpt(i), rotHpt(i), &stride);
                if (!buf) continue;
                fprintf(f, "%d %lu\n", i, checksumOf(buf, (size_t)stride * rotHpt(i)));
                freeRaster(buf);
            }
            fclose(f);
        }
    } else if (verb == L"night") {
        gNight = num(arg1, 0) != 0;
        writeAppPref();
        renderPage();
    }
}

// ---- reopen the last document ---------------------------------------------
static void reopenLastDocument() {
    if (!gDoc) return;
    // The document this one replaced. last.txt is no use here: autosave keeps it
    // pointing at whatever is open, so reading it would reopen the same file.
    std::wstring want = gPrevPath;
    if (want.empty() || want == gPath) {
        MessageBoxW(gWnd, L"No earlier document to reopen.", L"mnpdf", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (GetFileAttributesW(want.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(gWnd, L"That document has moved or been deleted since you left it.",
                    L"mnpdf", MB_OK | MB_ICONINFORMATION);
        return;
    }
    openPath(want);
}

// ---- the Advanced cascade --------------------------------------------------
// Everything that needs more than the small reader's own machinery lives here,
// greyed out, so the reader can see the shape of it without a second app.
static HMENU advancedMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 250, L"OCR text layer  (planned)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 251, L"Export as Word  (planned)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 252, L"Edit text in place  (planned)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 253, L"Create forms  (planned)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 254, L"Encrypt / password on save  (planned)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 255, L"Side-by-side tabs  (planned)");
    return m;
}
static void onKeyDown(HWND h, WPARAM wp) {
    int page = clientH() * 9 / 10;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    bool scrolled = false;
    if (!gSigPlacing.empty()) {              // placement owns the keyboard until Esc
        if (wp == VK_ESCAPE) { gSigPlacing.clear(); return; }
        return;
    }
    if (wp == VK_NEXT || wp == ' ') { scrollByDy(page); scrolled = true; }
    else if (wp == VK_PRIOR) { scrollByDy(-page); scrolled = true; }
    else if (wp == VK_DOWN)  { scrollByDy(60); scrolled = true; }
    else if (wp == VK_UP)    { scrollByDy(-60); scrolled = true; }
    else if (wp == VK_LEFT)  { gScrollX -= 60; clampScroll(); scrolled = true; }
    else if (wp == VK_RIGHT) { gScrollX += 60; clampScroll(); scrolled = true; }
    else if (wp == '0' && ctrl) fitWidth();
    else if (wp == 'F' && ctrl) toggleSearch(true);
    else if (wp == 'O' && ctrl) openDialog();
    else if (wp == 'C' && ctrl) copySelection();
    else if (wp == VK_F3 && !gSearchOpen) toggleSearch(true);
    else if (wp == VK_F3) nextMatch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
    else if (wp == 'S' && ctrl) { (GetKeyState(VK_SHIFT) & 0x8000) ? doSaveAs() : doSave(); }
    else if (wp == 'P' && ctrl && gDoc) doPrint();
    else if (wp == 'Z' && ctrl) { (GetKeyState(VK_SHIFT) & 0x8000) ? redoOp() : undoOp(); }
    else if (wp == 'Y' && ctrl) redoOp();
    else if ((wp == VK_ADD || wp == VK_OEM_PLUS)) zoomAt(1.2, clientW() / 2, clientH() / 2);
    else if ((wp == VK_SUBTRACT || wp == VK_OEM_MINUS)) zoomAt(1 / 1.2, clientW() / 2, clientH() / 2);
    else if (wp == VK_ESCAPE) { if (gSearchOpen) toggleSearch(false); else PostMessageW(h, WM_CLOSE, 0, 0); }
    if (scrolled) { markSave(); renderPage(); }
}

static void onContextMenu(HWND h, LPARAM lp) {   // right click
    hidePinTip(h);
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    if (x == -1 && y == -1) {                   // keyboard-invoked: menu at center
        x = clientW() / 2; y = clientH() / 2;
        POINT pt = { x, y };
        ClientToScreen(h, &pt);
        x = pt.x; y = pt.y;
    }
    HMENU menu = CreatePopupMenu();
    POINT cpt = { x, y };
    ScreenToClient(h, &cpt);                    // hit tests need client coords
    int pg;
    int ci = charIndexAtDoc(cpt.x, cpt.y, &pg);
    int hit = hlAt(pg, ci);
    gMenuHl = -1;
    int menuPage = pg >= 0 ? pg : gPageIndex;
    gMenuRotPage = menuPage;
    int pt = pinAt(cpt.x, cpt.y);
    if (pt >= 0) {                              // right-click on a pin
        gMenuPin = pt;
        AppendMenuW(menu, MF_STRING, 131, L"Edit text");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorSubmenu(gPins[pt].color, 150, 156, 159), L"Color");
        AppendMenuW(menu, MF_STRING, 132, L"Delete pin");
    } else if (hit >= 0) {                      // right-click on a highlight
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorSubmenu(gHls[hit].color, 120, 126, 136), L"Color");
        AppendMenuW(menu, MF_STRING, 116, L"Copy text");
        AppendMenuW(menu, MF_STRING, 117, L"Delete highlight");
        gMenuHl = hit;
    } else if (hasSelection()) {                // selection pending: offer highlight/copy
        AppendMenuW(menu, MF_STRING, 135, L"Highlight");           // one click: default color
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorSubmenu(gHlDefault, 140, 146, 149), L"Set default color");
        AppendMenuW(menu, MF_STRING, 101, L"Copy\tCtrl+C");
    } else if (gDoc) {                          // bare page: pin under cursor
        double px, py;
        pagePxToPt(menuPage, cpt.x - docLeft() - pageLeftPx(menuPage),
                   cpt.y - docTop() - yTopPx(menuPage), &px, &py);
        gMenuPinPtPage = menuPage;
        gMenuPinPtX = px;
        gMenuPinPtY = py;
        AppendMenuW(menu, MF_STRING, 130, L"Add pin here");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorSubmenu(gPinDefault, 160, 166, 169), L"Set pin color");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 107, L"Open...\tCtrl+O");
    AppendMenuW(menu, gDoc && gDirty ? MF_STRING : MF_GRAYED, 118, L"Save\tCtrl+S");
    AppendMenuW(menu, gDoc && gDirty ? MF_STRING : MF_GRAYED, 119, L"Save As...\tCtrl+Shift+S");
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 137, L"Print...\tCtrl+P");
    AppendMenuW(menu, MF_STRING | (gAutosave ? MF_CHECKED : 0), 109, L"Autosave");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, gUndo.empty() ? MF_GRAYED : MF_STRING, 114, L"Undo\tCtrl+Z");
    AppendMenuW(menu, gRedo.empty() ? MF_GRAYED : MF_STRING, 115, L"Redo\tCtrl+Y");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 103, L"Find...\tCtrl+F");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 104, L"Fit Width\tCtrl+0");
    AppendMenuW(menu, MF_STRING, 105, L"Zoom In\t+");
    AppendMenuW(menu, MF_STRING, 106, L"Zoom Out\t-");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 133, L"Rotate clockwise");
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 134, L"Rotate counter-clockwise");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, gDoc ? MF_STRING | (gOutlineOn ? MF_CHECKED : 0) : MF_GRAYED,
               201, L"Outline panel");
    AppendMenuW(menu, gDoc ? MF_STRING | (gThumbsOn ? MF_CHECKED : 0) : MF_GRAYED,
               202, L"Thumbnails");
    AppendMenuW(menu, MF_STRING | (gNight ? MF_CHECKED : 0), 200, L"Night mode");
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 203, L"Insert signature...");
    AppendMenuW(menu, gSigs.empty() ? MF_GRAYED : MF_STRING, 204, L"Clear signatures");
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 205, L"Merge PDFs...");
    AppendMenuW(menu, gDoc && gDocPages > 1 ? MF_STRING : MF_GRAYED, 206, L"Split pages...");
    AppendMenuW(menu, gDoc ? MF_STRING : MF_GRAYED, 207, L"Reopen last document");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)advancedMenu(), L"Advanced");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 110, L"Minimize");
    AppendMenuW(menu, MF_STRING, 111, IsZoomed(h) ? L"Restore" : L"Maximize");
    AppendMenuW(menu, MF_STRING, 113, gTitlebar ? L"Hide titlebar" : L"Show titlebar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 170, L"Check for updates");
    AppendMenuW(menu, MF_STRING, 112, L"Quit");
    SetForegroundWindow(h);   // TrackPopupMenu dismisses instantly without foreground
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, h, nullptr);
    DestroyMenu(menu);
    if (cmd) SendMessageW(h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);   // sync: focus is still ours
}

// the colour a colour-menu command id names, or -1 when it is not one: a preset
// at presetBase+c names palette entry c, a custom at customBase+c a custom slot
static int menuColorIndex(int id, int presetBase, int customBase) {
    if (id >= presetBase && id < presetBase + kPalPreset) return id - presetBase;
    if (id >= customBase && id < customBase + kPalCustom) return kPalPreset + (id - customBase);
    return -1;
}

// The menu-command ids are the protocol: colorSubmenu builds them, the reader's
// clicks carry them, and the test suites post them by number (named as CMD_*
// constants in tests/lib.ps1). They stay exactly where they have always been.
static void onCommand(HWND h, WPARAM wp) {
    int id = LOWORD(wp);
    switch (id) {
    case 1:                                     // the search Edit control's EN_CHANGE
        if (HIWORD(wp) == EN_CHANGE && gSearchOpen) {
            GetWindowTextW(gSearchBox, gQuery, 128);
            runSearch();
        }
        return;
    case 2:                                     // Edit > Find (the search bar's own id)
        toggleSearch(true);
        return;
    case 101: copySelection(); return;
    case 103: toggleSearch(true); return;
    case 104: fitWidth(); return;
    case 105: zoomAt(1.2, clientW() / 2, clientH() / 2); return;
    case 106: zoomAt(1 / 1.2, clientW() / 2, clientH() / 2); return;
    case 107: openDialog(); return;
    case 200:                                   // night mode: the same pixels, inverted
        gNight = !gNight;
        writeAppPref();
        renderPage();
        return;
    case 201: toggleOutlineDrawer(); return;
    case 202: toggleThumbDrawer(); return;
    case 203: if (gDoc) pickSignatureImage(); return;
    case 204: clearSignatures(); return;
    case 205: if (gDoc) mergeDialog(); return;
    case 206: if (gDoc && gDocPages > 1) openRangePrompt(); return;
    case 207: reopenLastDocument(); return;
    case 116:                                   // copy highlight text
        if (gMenuHl >= 0 && gMenuHl < (int)gHls.size()) {
            const Hl& h2 = gHls[gMenuHl];
            copyTextRange(h2.page, h2.start, h2.page, h2.start + h2.count - 1);
        }
        return;
    case 117:                                   // delete highlight
        if (gMenuHl >= 0 && gMenuHl < (int)gHls.size()) deleteHighlight(gMenuHl);
        return;
    case 135:                                   // Highlight with the default color
        applyHighlightFromSelection(gHlDefault);
        return;
    // ---- the four colour menus. Each submenu answers two id families that
    // differ only in base - presets at 120/140/150/160, custom slots at
    // 126/146/156/166 - so one arm per menu replaces one case group per family.
    case 120: case 121: case 122: case 123: case 124: case 125:
    case 126: case 127: case 128: {             // recolour the menued highlight
        int c = menuColorIndex(id, 120, 126);
        if (c >= 0) recolorHighlight(gMenuHl, c);
        return;
    }
    case 136: gColorTarget = 0; startColorEntry(); return;   // Custom... on a highlight
    case 140: case 141: case 142: case 143: case 144: case 145:
    case 146: case 147: case 148: {             // set the default highlight colour
        int c = menuColorIndex(id, 140, 146);
        if (c >= 0) {
            gHlDefault = c;
            writeAppPref();
            if (hasSelection()) applyHighlightFromSelection(gHlDefault);
        }
        return;
    }
    case 149: gColorTarget = 2; startColorEntry(); return;   // Custom... on the default highlight
    case 150: case 151: case 152: case 153: case 154: case 155:
    case 156: case 157: case 158: {             // recolour the menued pin
        int c = menuColorIndex(id, 150, 156);
        if (c >= 0) recolorPin(gMenuPin, c);
        return;
    }
    case 159: gColorTarget = 1; startColorEntry(); return;   // Custom... on a pin
    case 160: case 161: case 162: case 163: case 164: case 165:
    case 166: case 167: case 168: {             // set the default pin colour
        int c = menuColorIndex(id, 160, 166);
        if (c >= 0) {
            gPinDefault = c;
            writeAppPref();
        }
        return;
    }
    case 169: gColorTarget = 3; startColorEntry(); return;   // Custom... on the default pin
    case 180:                                   // hand the three custom slots back
        if (!anyCustomColor()) return;
        if (MessageBoxW(h, L"Clear your custom colours?\n\n"
                           L"Marks that use one fall back to the preset for that "
                           L"kind - a highlight to yellow, a pin to red.",
                        L"mnpdf", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        clearCustomColors();
        return;
    case 109:                                  // toggle autosave
        gAutosave = !gAutosave;
        writeAppPref();
        if (gAutosave) {
            SetTimer(h, 1, 800, nullptr);      // re-arm the debounce tick
            markSave();                        // flush current state soon
        } else {
            gSaveDirty = false;
            KillTimer(h, 1);
        }
        return;
    case 110: PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0); return;
    case 111: PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0); return;
    case 112: PostMessageW(h, WM_CLOSE, 0, 0); return;
    case 113: toggleTitlebar(h); return;
    case 114: undoOp(); return;
    case 115: redoOp(); return;
    case 118: doSave(); return;
    case 119: doSaveAs(); return;
    case 137: doPrint(); return;
    case 170: startUpdateCheck(true); return;
    case 130:                                   // add pin at the menu drop point
        if (gMenuPinPtPage >= 0) {
            addPinAt(gMenuPinPtPage, gMenuPinPtX, gMenuPinPtY);
        } else if (gDoc) {                      // fallback (posted id): center of the active page
            addPinAt(gPageIndex, gPageW[gPageIndex] / 2, gPageH[gPageIndex] / 2);
        }
        return;
    case 131:                                   // edit pin text
        if (gMenuPin >= 0) startPinEdit(gMenuPin, false);
        return;
    case 132: deletePin(gMenuPin); return;
    case 133: case 134: {
        int rp = gMenuRotPage >= 0 ? gMenuRotPage : gPageIndex;
        rotatePage(rp, id == 133 ? +1 : -1);
        dropThumb(rp);                        // that drawer row now shows a different page
        return;
    }
    }
}

static void onTimer(HWND h, WPARAM wp) {
    if (wp == 1 && gSaveDirty) {               // debounced autosave flush
        gSaveDirty = false;
        writeSidecarNow();
    } else if (wp == 4 && gTipPin >= 0) {      // hover dwell elapsed: show the pin text
        KillTimer(h, 4);
        showPinTip(gTipPin);
    } else if (wp == 3 && gSelDrag && gEdgeScroll) {   // drag auto-scroll at the margins
        // The speed follows how far the drag has pushed past the edge:
        // barely over the edge crawls, and the further past it goes the
        // faster it runs - whether the cursor is still inside the window
        // or has already been dragged beyond it. Measured in pages per
        // second rather than pixels, so the pace feels the same at every
        // zoom level.
        const int zone = kEdgeZone;                 // the in-window margin
        int depth = gEdgeScroll > 0 ? gDragPt.y - (gClientH - zone) : zone - gDragPt.y;
        if (depth < 0) depth = 0;
        const int maxDepth = zone * 2;              // held well past the window: top speed
        if (depth > maxDepth) depth = maxDepth;
        double frac = (double)depth / zone;         // 1.0 at the window edge, 2.0 past it
        if (frac < 0.2) frac = 0.2;                 // never a dead stop
        int ph = pageHpx(gPageIndex);               // one page of scroll, in px
        if (ph < 64) ph = gClientH > 64 ? gClientH : 64;
        double step = ph * 0.017 * frac;            // ~1.8 s per page at the window edge
        scrollByDy(gEdgeScroll * (int)step);
        int pg;
        int idx = charIndexClamped(gDragPt.x, gDragPt.y, &pg);
        if (idx >= 0) {
            gSelHeadPage = pg;
            gSelHead = idx;
        }
        renderPage();
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        gWnd = h;
        createSearchBar(h, ((LPCREATESTRUCT)lp)->hInstance);
        SetTimer(h, 1, 800, nullptr);              // autosave flush tick
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            if (gFitWidth) applyFitWidth();       // refit before rendering
            rebuildSurface();
            placeDrawers();                   // the drawers hang off this rect
        }
        placeSearchBar();
        hidePinTip(h);                            // the refit moved the tip's anchor
        updateTitle();                           // every size message, SIZE_MINIMIZED included
        return 0;
    case WM_MOVE:                                 // both note boxes are owned popups:
        placeDrawers();
        if (gEditPin >= 0 && gPinBox) sizePinBoxToText();   // they do not follow the window
        hidePinTip(h);
        return 0;
    case WM_NCACTIVATE:
        // WS_CAPTION stays in the style (see the note at CreateWindowExW: the
        // system puts it back even when the create style omits it), so on
        // every activation change DefWindowProc repaints the non-client area
        // with the stock system caption - the ghost title bar that appears
        // whenever another window takes focus while mnpdf's own is hidden.
        // lParam = -1 tells it the frame does not need repainting; the client
        // area already covers the whole window, so there is no frame to paint.
        if (!gTitlebar) return DefWindowProcW(h, m, wp, (LPARAM)-1);
        break;
    case WM_NCCALCSIZE:
        // with the caption gone DWM still paints its accent-coloured resize
        // border (the light blue hint); let the client cover the whole window
        // so no frame is drawn. WS_THICKFRAME stays, so the edges still resize.
        if (!gTitlebar && wp) {
            RECT* rc = &((NCCALCSIZE_PARAMS*)lp)->rgrc[0];
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
                RECT w = mi.rcWork;
                // a maximized window sits outside the work area on all four sides and a
                // snapped one on three, each by exactly the frame thickness; a window
                // merely dragged off-screen exceeds on one side by an arbitrary amount
                const int frame = 32;
                bool outL = w.left - rc->left > 0 && w.left - rc->left <= frame;
                bool outT = w.top - rc->top > 0 && w.top - rc->top <= frame;
                bool outR = rc->right - w.right > 0 && rc->right - w.right <= frame;
                bool outB = rc->bottom - w.bottom > 0 && rc->bottom - w.bottom <= frame;
                if ((int)outL + (int)outT + (int)outR + (int)outB >= 2) {
                    if (outL) rc->left = w.left;
                    if (outT) rc->top = w.top;
                    if (outR) rc->right = w.right;
                    if (outB) rc->bottom = w.bottom;
                }
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (gMemDC && gBits) BitBlt(dc, 0, 0, gClientW, gClientH, gMemDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!gSigPlacing.empty()) {          // the click places the stamp, nothing else
            int hit;
            sigPlacementAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &hit);
            return 0;
        }
        hidePinTip(h);
        SetFocus(h);
        SetCapture(h);
        gSelDrag = true;
        int pg;
        int idx = charIndexClamped(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &pg);
        if (gLastWasDbl && GetMessageTime() - (LONG)gLastDblTime < (LONG)GetDoubleClickTime()
            && abs(GET_X_LPARAM(lp) - gLastDblX) < 8 && abs(GET_Y_LPARAM(lp) - gLastDblY) < 8) {
            selectLine(pg, idx);                    // third click: whole line
        } else {
            gSelAnchorPage = gSelHeadPage = pg;
            gSelAnchor = gSelHead = idx;
        }
        gLastWasDbl = false;
        renderPage();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        hidePinTip(h);
        SetFocus(h);
        SetCapture(h);
        gSelDrag = true;
        int pg;
        int idx = charIndexClamped(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &pg);
        gSelAnchorPage = gSelHeadPage = pg;
        gSelAnchor = gSelHead = idx;
        selectWord(pg, idx);
        gLastWasDbl = true;
        gLastDblTime = GetMessageTime();
        gLastDblX = GET_X_LPARAM(lp);
        gLastDblY = GET_Y_LPARAM(lp);
        renderPage();
        return 0;
    }
    case WM_ACTIVATE:
        // a hover note must not outlive the reader's attention: as soon as
        // another window comes forward the pin note goes away with it
        if (LOWORD(wp) == WA_INACTIVE) hidePinTip(h);
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && gDoc) {
            POINT cp;
            GetCursorPos(&cp);
            ScreenToClient(h, &cp);
            int pg;
            int idx = charIndexStrict(cp.x, cp.y, &pg);
            SetCursor(LoadCursor(nullptr, idx >= 0 ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!gSelDrag && gEditPin < 0) {           // hover a pin: offer its text
            int p = pinAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (p >= 0 && !gPins[p].text.empty()) {
                if (p != gTipPin) {
                    hidePinTip(h);
                    gTipPin = p;
                    SetTimer(h, 4, 300, nullptr);
                }
            } else hidePinTip(h);
        }
        if (gSelDrag && gSelAnchor >= 0) {
            gDragPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int edge = 0;
            if (gDragPt.y < kEdgeZone) edge = -1;         // near the top margin: scroll up
            else if (gDragPt.y > gClientH - kEdgeZone) edge = 1;
            if (edge != gEdgeScroll) {
                gEdgeScroll = edge;
                if (edge) SetTimer(h, 3, 25, nullptr);
                else KillTimer(h, 3);
            }
            int pg;
            int idx = charIndexClamped(gDragPt.x, gDragPt.y, &pg);
            if (idx >= 0 && (pg != gSelHeadPage || idx != gSelHead)) {
                gSelHeadPage = pg;
                gSelHead = idx;
                renderPage();
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        gSelDrag = false;
        gEdgeScroll = 0;
        KillTimer(h, 3);
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_COPY:                                   // standard copy-selection message
        copySelection();
        return 0;
    case WM_CAPTURECHANGED:
        gSelDrag = false;
        gEdgeScroll = 0;
        KillTimer(h, 3);
        return 0;
    case WM_MOUSEWHEEL: {
        hidePinTip(h);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        double delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (wp & MK_CONTROL) {
            zoomAt(delta > 0 ? 1.1 : 1 / 1.1, pt.x, pt.y);
        } else {
            scrollByDy(-(int)(delta / 120.0 * 3 * 40.0));   // pure scroll; pages are stacked
            markSave();
            renderPage();
        }
        return 0;
    }
    case WM_KEYDOWN: onKeyDown(h, wp); return 0;
    case WM_CONTEXTMENU: onContextMenu(h, lp); return 0;
    case WM_COMMAND: onCommand(h, wp); return 0;
    case WM_NCHITTEST: {
        // With the caption hidden the client covers the whole window (see
        // WM_NCCALCSIZE), so DefWindowProc finds no non-client margin anywhere
        // and returns HTCLIENT for every pixel - the frame can then never be
        // dragged. The eight edge zones are computed here instead, over a band
        // as thick as the frame the style still reserves.
        if (!gTitlebar) {
            const int dpi = GetDpiForWindow(h);
            // GetSystemMetrics is not per-monitor aware and already reports the
            // primary monitor's scale, so the window DPI is not scaled in again
            int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (frame < 4) frame = 4;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt);   // outside the maximized state WM_NCCALCSIZE gives the client the whole window
            HWND kid = ChildWindowFromPointEx(h, pt, CWP_SKIPINVISIBLE);
            if (kid && kid != h) return HTCLIENT;   // the control under the cursor owns the click
            // The caption is gone, so the top strip is the only way to move the
            // window. The strip is the app's own 8 design pixels at this dpi,
            // which is at least as thick as the corrected frame band, so the
            // whole top edge is caption and resize comes from the other three
            // edges and the four corners.
            int capStrip = MulDiv(8, dpi, USER_DEFAULT_SCREEN_DPI);
            if (capStrip < 4) capStrip = 4;
            if (pt.y < capStrip) return HTCAPTION;
            RECT cr;
            GetClientRect(h, &cr);
            const int x = pt.x, y = pt.y;
            const int w = cr.right - cr.left, bh = cr.bottom - cr.top;
            const bool L = x < frame, R = x >= w - frame, T = y < frame, B = y >= bh - frame;
            if (L && T) return HTTOPLEFT;
            if (R && T) return HTTOPRIGHT;
            if (L && B) return HTBOTTOMLEFT;
            if (R && B) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            return HTCLIENT;
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_MNPDF_UPDATE_RESULT: {
        UpdateResult* result = (UpdateResult*)lp;
        if (result) {
            gUpdateCheckRunning.store(false);
            if (!gShuttingDown.load()) {
                noteUpdateCheck(result->tag);
                // 170 = "Check for updates". A click that arrived while the check ran is
                // answered by posting the command rather than recursing from the worker's
                // tail, so the cooldown decision and its dialog stay on the UI thread.
                if (gManualUpdateRequested.exchange(false))
                    PostMessageW(h, WM_COMMAND, MAKEWPARAM(170, 0), 0);
            }
            delete gPendingUpdateResult;
            gPendingUpdateResult = result;
        }
        return 0;
    }
    case WM_TIMER: onTimer(h, wp); return 0;
    case WM_CLOSE:
        if (gEditPin >= 0) commitPinEdit();        // an open pin box saves first
        if (gAutosave) {
            writeSidecarNow();                     // edits survive in the sidecar: no prompt
        } else if (gDirty) {
            int r = MessageBoxW(h, L"Save changes?", L"mnpdf", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !doSave()) return 0;  // save failed: stay open
            writeSidecarNow();
        } else {
            writeSidecarNow();                     // sidecar always keeps the reading position
        }
        writeAppPref();                            // the last rect read here is the one restored
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        gShuttingDown.store(true);
        flushPageCache();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput gsi;
    gGdiOk = (Gdiplus::GdiplusStartup(&gGdiToken, &gsi, nullptr) == Gdiplus::Ok);   // anti-aliased pin dots

    FPDF_LIBRARY_CONFIG cfg = { 2, nullptr, nullptr };
    FPDF_InitLibraryWithConfig(&cfg);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MNPDF));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MNPDF), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"mnpdf";
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    WNDCLASSW tc = { 0 };
    tc.lpfnWndProc = DefWindowProcW;
    tc.hInstance = hInst;
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tc.lpszClassName = L"mnpdfPinTip";
    tc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&tc);

    // include the engine dll dir in the search path so the exe runs anywhere
    const std::wstring exeDir = moduleDirectory();
    SetDllDirectoryW(exeDir.c_str());

    loadAppPref();   // titlebar preference, and the saved frame geometry + winmax flag
    // both read before the window exists: WM_CREATE and the first WM_SIZE
    // (WS_VISIBLE is in the style) both run inside CreateWindowExW, so a title
    // computed during them must already see these two
    const bool background = _wgetenv(L"MNPDF_BACKGROUND") != nullptr;
    gVerboseTitle = _wgetenv(L"MNPDF_VERBOSE") != nullptr;   // test-suite opt-in: richer window title
    // MNPDF_BACKGROUND=1: gate/test runs must not steal focus. Mask WS_VISIBLE
    // out of the create style so the first appearance is already minimized
    // and inactive - a later ShowWindow cannot undo an initial flash.
    // Restore the frame where the last session left it in three cases.
    // (1) A rect no monitor can see any more - a screen was unplugged, or the
    //     resolution dropped - falls back to the default placement instead of
    //     opening where the user cannot reach it.
    // (2) A rect that only partly fits keeps its saved width and height and has
    //     just its top-left moved into that monitor's work area, so the caption
    //     strip and a resize border stay reachable: a window larger than the
    //     screen it lands on is ordinary and the user can resize it down. That
    //     size is honored only up to what the landing monitor's work area shows:
    //     Windows caps a created window at that work area plus a little slack, a
    //     platform limit the app cannot lift, and because every quit records the
    //     rect as the window actually stands, a size that could not be shown here
    //     is not preserved for a future larger monitor.
    // (3) A rect that fits is restored verbatim.
    int startX = CW_USEDEFAULT, startY = CW_USEDEFAULT, startW = gWinW, startH = gWinH;
    if (gWinX != CW_USEDEFAULT && gWinY != CW_USEDEFAULT) {
        RECT sr = { gWinX, gWinY, gWinX + gWinW, gWinY + gWinH };
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR hm = MonitorFromRect(&sr, MONITOR_DEFAULTTONULL);
        if (hm && GetMonitorInfoW(hm, &mi)) {
            startW = sr.right - sr.left;          // the size the user left it in
            startH = sr.bottom - sr.top;
            startX = clampBox(sr.left, startW, mi.rcWork.left, mi.rcWork.right);
            startY = clampBox(sr.top, startH, mi.rcWork.top, mi.rcWork.bottom);
        }
    }

    // WS_CAPTION is deliberately left in the style even when the preference
    // says hidden. Passing a captionless style to CreateWindowExW does not
    // stick: with WS_MINIMIZEBOX/WS_MAXIMIZEBOX present the system ORs
    // WS_CAPTION back in before the window exists (verified by reading
    // GWL_STYLE immediately after CreateWindowExW - the cleared bits are
    // already set again). The caption is hidden by WM_NCCALCSIZE below, which
    // gives the client area the whole window so there is nothing left to draw.
    DWORD winStyle = WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    if (background) winStyle &= ~(DWORD)WS_VISIBLE;
    gWnd = CreateWindowExW(0, L"mnpdf", L"mnpdf", winStyle,
                           startX, startY, startW, startH,
                           nullptr, nullptr, hInst, nullptr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool opened = false;
    if (argc >= 2) {
        opened = openPath(argv[1]);
    } else {
        wchar_t dir[MAX_PATH];
        appDirW(dir, MAX_PATH);
        FILE* fp = _wfopen((std::wstring(dir) + L"\\last.txt").c_str(), L"rb");
        if (fp) {                                   // plain launch: reopen last document
            char buf[MAX_PATH * 3] = "";
            size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) got--;
            buf[got] = 0;
            if (got) {
                int wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
                std::vector<wchar_t> wpath(wn);
                MultiByteToWideChar(CP_UTF8, 0, buf, -1, wpath.data(), wn);
                if (GetFileAttributesW(wpath.data()) != INVALID_FILE_ATTRIBUTES) opened = openPath(wpath.data());
            }
        }
    }
    if (argv) LocalFree(argv);
    if (!opened) openDialog();
    {
        const wchar_t* hookSpec = _wgetenv(L"MNPDF_HOOK");
        if (hookSpec && hookSpec[0]) runHook(hookSpec);
    }

    // A launch with the titlebar preference hidden must clear WS_CAPTION from the
    // live style, not rely on the style passed to CreateWindowExW: with
    // WS_MINIMIZEBOX/WS_MAXIMIZEBOX present the system ORs WS_CAPTION back in
    // during creation (confirmed by reading GWL_STYLE immediately after the
    // call), so the create-time mask never survives.
    // Hidden is therefore applied the same way the Hide titlebar menu applies
    // it - by writing the style and re-running the frame calculation - because
    // WM_NCCALCSIZE alone does not hide it on the first recalc: a plain launch
    // with titlebar=0 draws the caption (the client is 58 px short of the
    // window at 192 dpi) and it only disappears after a later frame recalc such
    // as a minimize/restore. Setting the style bit off before the first show
    // leaves nothing for the frame calc to carve.
    if (!gTitlebar) {
        LONG st = (LONG)GetWindowLongPtrW(gWnd, GWL_STYLE);
        SetWindowLongPtrW(gWnd, GWL_STYLE, (LONG_PTR)(st & ~(LONG)WS_CAPTION));
        SetWindowPos(gWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    // The title needs no WinMain-side call: WM_SIZE drives updateTitle on every
    // size message, SIZE_MINIMIZED included, and renderPage calls it first.
    // The window still exists: MainWindowHandle resolves, posted messages land,
    // and title reads work - it is only ever minimized and inactive.
    ShowWindow(gWnd, background ? SW_SHOWMINNOACTIVE
                                : (gWinMax ? SW_SHOWMAXIMIZED : show));
    startUpdateCheck(false);                         // notify only when a newer release exists
    MSG msg;
    for (;;) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            showPendingUpdateResult();
            WaitMessage();
        }
    }
    flushPageCache();                              // releases gPage/gTextPage too
    if (gDoc) FPDF_CloseDocument(gDoc);
    FPDF_DestroyLibrary();
    if (gNoteFont) { DeleteObject(gNoteFont); gNoteFont = nullptr; gNoteFontPx = 0; }
    if (gGdiOk) Gdiplus::GdiplusShutdown(gGdiToken);
    return 0;
}
