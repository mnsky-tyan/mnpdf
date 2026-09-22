// mnpdf-native - the lightest PDF reader.
// Phase 1: text layer - drag/word/line selection with copy, find bar with
// per-page match highlights, page-walking match navigation.
// RAM discipline: the only bitmap ever allocated is screen-sized. Pages are
// never rasterized whole; the engine draws the page into the viewport DIB
// at an offset, so memory is independent of zoom and page count.
#pragma comment(lib, "gdiplus.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <psapi.h>
#include <unknwn.h>          // MIDL_INTERFACE for the GDI+ headers
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "msimg32.lib")

// ---- state ----
static HWND gWnd = nullptr;
static std::wstring gPath;
static FPDF_DOCUMENT gDoc = nullptr;
static FPDF_PAGE gPage = nullptr;
static FPDF_TEXTPAGE gTextPage = nullptr;
static int gPageIndex = 0;
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
static bool gMenuRecolor = false;
static int gMenuPin = -1;                        // pin the context menu targets
static int gMenuRotPage = -1;                    // page the context menu targets (rotate)
static int gMenuPinPtPage = -1;                  // "add pin here" drop point
static int gColorTarget = 0;                       // what a custom colour applies to
static double gMenuPinPtX = 0, gMenuPinPtY = 0;
static bool gSelDrag = false;                    // left button held with a live drag
static bool gMultiClick = false;                 // dbl/triple-click sequence: release keeps the selection
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

static void pushOp(const Op& op) {
    gUndo.push_back(op);
    gRedo.clear();
    markDirty();
    markSave();                                   // edits reach the sidecar too
}

static void relayoutPages();
static void renderPage();
static bool doSaveAs();

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
        for (Hl& h : gHls) if (h.page == op.hlVal.page && h.start == op.hlVal.start && h.count == op.hlVal.count) { h.color = op.hlFrom; break; }
        break;
    case 3:                                       // pinadd: remove
        if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()) gPins.erase(gPins.begin() + op.pinIdx);
        break;
    case 4:                                       // pindel: restore
        gPins.push_back(op.pinVal);
        break;
    case 5:                                       // pintext
        if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()) gPins[op.pinIdx].text = op.textFrom;
        break;
    case 6:                                       // rot
        if (gRot && op.rotPage >= 0 && op.rotPage < gDocPages) {
            gRot[op.rotPage] = op.rotFrom;
            relayoutPages();
        }
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
        for (Hl& h : gHls) if (h.page == op.hlVal.page && h.start == op.hlVal.start && h.count == op.hlVal.count) { h.color = op.hlTo; break; }
        break;
    case 3:
        gPins.push_back(op.pinVal);
        break;
    case 4:
        if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()) gPins.erase(gPins.begin() + op.pinIdx);
        break;
    case 5:
        if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()) gPins[op.pinIdx].text = op.textTo;
        break;
    case 6:
        if (gRot && op.rotPage >= 0 && op.rotPage < gDocPages) {
            gRot[op.rotPage] = op.rotTo;
            relayoutPages();
        }
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
static struct { int page, start, count; } gMatches[4096];
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
    for (int px10 = 4; px10 <= 128; px10 *= 2) {     // wide horizontal snap
        double tol = px10 / gZoom;
        int idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
        if (idx >= 0) return idx;
    }
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
    int page = -1;
    for (int i = 0; i < gDocPages; i++) {
        int top = yTopPx(i);
        if (docY >= top && docY < top + pageHpx(i)) { page = i; break; }
    }
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
    for (int px10 = 4; idx < 0 && px10 <= 32; px10 *= 2) {   // 4..32 device px
        double tol = px10 / gZoom;
        idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
    }
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
    int page = -1;
    for (int i = 0; i < gDocPages; i++) {
        int top = yTopPx(i);
        if (docY >= top && docY < top + pageHpx(i)) { page = i; break; }
    }
    if (page < 0) return -1;
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    double px, py;
    pagePxToPt(page, cx - docLeft() - pageLeftPx(page), docY - yTopPx(page), &px, &py);
    for (int px10 = 4; px10 <= 16; px10 *= 2) {
        double tol = px10 / gZoom;
        int idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
        if (idx >= 0) { *pageOut = page; return idx; }
    }
    return -1;
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
static void selectWord(int page, int idx);
static void selectLine(int page, int idx);
static bool loadPage(int index);
static void markSave();
static void relayoutPages();
static void renderPage();
static void toggleSearch(bool show);
static void pagePxToPt(int page, double sx, double sy, double* ox, double* oy);

static void copyTextRange(int p0, int i0, int p1, int i1);

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
        FPDF_TEXTPAGE tp = textPageOf(p);
        if (!tp) continue;
        int n = FPDFText_CountChars(tp);
        int s = (p == p0) ? i0 : 0;
        int e = (p == p1) ? i1 : n - 1;
        if (e < s || e >= n) continue;
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
    default: *ox = x * gZoom;     *oy = (H - y) * gZoom; break;
    case 1:  *ox = y * gZoom;     *oy = x * gZoom;       break;
    case 2:  *ox = (W - x) * gZoom; *oy = y * gZoom;     break;
    case 3:  *ox = (H - y) * gZoom; *oy = (W - x) * gZoom; break;
    }
}

static void pagePxToPt(int page, double sx, double sy, double* ox, double* oy) {
    double W = gPageW[page], H = gPageH[page];
    switch (gRot[page] & 3) {
    default: *ox = sx / gZoom;     *oy = H - sy / gZoom; break;
    case 1:  *ox = sy / gZoom;     *oy = sx / gZoom;       break;
    case 2:  *ox = W - sx / gZoom; *oy = sy / gZoom;       break;
    case 3:  *ox = W - sy / gZoom; *oy = H - sx / gZoom;   break;
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
        FPDF_TEXTPAGE tp = textPageOf(p);
        if (!tp) continue;
        int n = FPDFText_CountChars(tp);
        int s = (p == p0) ? i0 : 0;
        int e = (p == p1) ? i1 : n - 1;
        if (e < s || e >= n) continue;
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
        FPDF_TEXTPAGE tp = textPageOf(p);
        if (!tp) continue;
        int n = FPDFText_CountChars(tp);
        int s = (p == p0) ? i0 : 0;
        int e = (p == p1) ? i1 : n - 1;
        if (e < s || e >= n) continue;
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

static void drawPins() {
    if (!gMemDC || gPins.empty()) return;
    if (gGdiOk) {
        // locals, not statics: GDI+ objects must die before GdiplusShutdown
        Gdiplus::Graphics g(gMemDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);   // GDI's Ellipse leaves a wiggly edge
        for (const Pin& pn : gPins) {
            int vy = pageVy(pn.page), vh = pageHpx(pn.page);
            if (vy + vh < 0 || vy > gClientH) continue;      // page not in view
            double ax, ay;
            pagePtToPx(pn.page, pn.x, pn.y, &ax, &ay);
            int x = pageVx(pn.page) + (int)ax;
            int y = pageVy(pn.page) + (int)ay;
            int r = pinRadius(gZoom);
            COLORREF pc;
            if (!palColor(pn.color, pc)) pc = gPalPreset[kPalPreset - 1];
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(pc), GetGValue(pc), GetBValue(pc)));
            g.FillEllipse(&brush, x - r, y - r, 2 * r, 2 * r);
        }
    } else {
        for (const Pin& pn : gPins) {
            int vy = pageVy(pn.page), vh = pageHpx(pn.page);
            if (vy + vh < 0 || vy > gClientH) continue;      // page not in view
            double ax, ay;
            pagePtToPx(pn.page, pn.x, pn.y, &ax, &ay);
            int x = pageVx(pn.page) + (int)ax;
            int y = pageVy(pn.page) + (int)ay;
            int r = pinRadius(gZoom);
            COLORREF pc;
            if (!palColor(pn.color, pc)) pc = gPalPreset[kPalPreset - 1];
            HBRUSH br = CreateSolidBrush(pc);
            HPEN np = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ ob = SelectObject(gMemDC, br);
            HGDIOBJ op = SelectObject(gMemDC, np);
            Ellipse(gMemDC, x - r, y - r, x + r, y + r);
            SelectObject(gMemDC, op);
            SelectObject(gMemDC, ob);
            DeleteObject(np);
            DeleteObject(br);
        }
    }
}

static int pinAt(int cx, int cy) {
    if (!gPrefixPt || !gDocPages) return -1;
    int docY = cy - docTop();
    for (int i = 0; i < gDocPages; i++) {
        int top = yTopPx(i);
        if (docY < top || docY >= top + pageHpx(i)) continue;
        double px, py;
        pagePxToPt(i, cx - docLeft() - pageLeftPx(i), docY - top, &px, &py);
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
        // commit a box that only just opened
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
        for (int i = 0; i < gDocPages && gMatchCount < (int)(sizeof(gMatches) / sizeof(gMatches[0])); i++) {
            FPDF_TEXTPAGE tp = textPageOf(i);
            if (!tp) continue;
            FPDF_SCHHANDLE sh = FPDFText_FindStart(tp, (FPDF_WIDESTRING)gQuery, 0, 0);
            if (!sh) continue;
            while (gMatchCount < (int)(sizeof(gMatches) / sizeof(gMatches[0])) && FPDFText_FindNext(sh)) {
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
        lo = (int)(ax < bx ? ax : bx) < lo ? (int)(ax < bx ? ax : bx) : lo;
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
    if (!gDoc) return;
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
    updateTitle();
    InvalidateRect(gWnd, nullptr, FALSE);
}

// zoom mode: fit-width refits on every resize until a manual zoom turns it off
static void applyFitWidth() {
    int cw = clientW() - 2 * MARGIN;
    if (cw > 0 && gMaxPageW > 0) gZoom = clampZoom(cw / gMaxPageW);
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

static bool gVerboseTitle = false;               // debug harnesses: page/zoom/RAM in the title

static void updateTitle() {
    wchar_t t[256];
    if (gVerboseTitle) {
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
            int n = WideCharToMultiByte(CP_UTF8, 0, pn.text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::vector<char> u8(n);
            WideCharToMultiByte(CP_UTF8, 0, pn.text.c_str(), -1, u8.data(), n, nullptr, nullptr);
            std::string esc;                       // multiline-safe: \n stays one line
            for (char* p = u8.data(); *p; p++) {
                if (*p == '\\') esc += "\\\\";
                else if (*p == '\n') esc += "\\n";
                else if (*p == '\r') continue;
                else esc += *p;
            }
            fprintf(fp, "pin=%d,%.2f,%.2f,c%d,%s\n", pn.page, pn.x, pn.y, pn.color, esc.c_str());
        }
        fclose(fp);
    }
    wchar_t dir[MAX_PATH];
    appDirW(dir, MAX_PATH);
    FILE* lfp = _wfopen((std::wstring(dir) + L"\\last.txt").c_str(), L"wb");
    if (lfp) {
        int n = WideCharToMultiByte(CP_UTF8, 0, gPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::vector<char> u8(n);
        WideCharToMultiByte(CP_UTF8, 0, gPath.c_str(), -1, u8.data(), n, nullptr, nullptr);
        fwrite(u8.data(), 1, (size_t)n - 1, lfp);   // n includes the terminator
        fputc('\n', lfp);
        fclose(lfp);
    }
}

static void markSave() {
    if (gAutosave) gSaveDirty = true;              // flushed by the WM_TIMER tick
}

// app-level prefs in %APPDATA%\mnpdf\app.txt: chrome toggles, the default
// highlight/pin colours and the custom #rrggbb slots (round-robin pointer too)
static void writeAppPref() {
    wchar_t dir[MAX_PATH];
    appDirW(dir, MAX_PATH);
    FILE* fp = _wfopen((std::wstring(dir) + L"\\app.txt").c_str(), L"wb");
    if (fp) {
        fprintf(fp, "titlebar=%d\nautosave=%d\nhlcolor=%d\npincolor=%d\n",
                gTitlebar ? 1 : 0, gAutosave ? 1 : 0, gHlDefault, gPinDefault);
        for (int c = 0; c < kPalCustom; c++)
            if (gPalCustom[c] != CLR_INVALID)
                fprintf(fp, "pal%d=%02x%02x%02x\n", c,
                        GetRValue(gPalCustom[c]), GetGValue(gPalCustom[c]), GetBValue(gPalCustom[c]));
        fprintf(fp, "palnext=%d\n", gPalNext);
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
        if (sscanf(line, "titlebar=%d", &iv) == 1) gTitlebar = iv != 0;
        else if (sscanf(line, "autosave=%d", &iv) == 1) gAutosave = iv != 0;
        else if (sscanf(line, "hlcolor=%d", &iv) == 1) gHlDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "pincolor=%d", &iv) == 1) gPinDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "palnext=%d", &iv) == 1) gPalNext = (iv >= 0 && iv < kPalCustom) ? iv : 0;
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

static void restoreSidecar(const std::wstring& path) {
    FILE* fp = _wfopen(sidecarPathFor(path).c_str(), L"rb");
    if (!fp) return;
    double zoom = -1;
    int fit = -1, page = -1, fmt = 0;
    char line[512];
    while (fgets(line, 512, fp)) {
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
                bool have = false;                     // already held from the file itself?
                for (const Hl& h : gHls)
                    if (h.baked && h.page == a && h.start == b && h.count == c) { have = true; break; }
                if (!have) gHls.push_back({ a, b, c, col });
            }
        } else if (sscanf(line, "rot=%d,%d", &a, &b) == 2) {
            if (a >= 0 && a < gDocPages && b >= 0 && b < 4) gRot[a] = b;
        } else if (strncmp(line, "pin=", 4) == 0) {
            int c1 = -1, c2 = -1, c3 = -1;             // first three commas split the fields
            for (int k = 4; line[k]; k++) {
                if (line[k] == ',') { if (c1 < 0) c1 = k; else if (c2 < 0) c2 = k; else { c3 = k; break; } }
            }
            int pp, rr;
            double xx, yy;
            if (c3 > 0 && sscanf(line + 4, "%d,%lf,%lf", &pp, &xx, &yy) == 3 && pp >= 0 && pp < gDocPages) {
                line[(int)strlen(line) - 1] = 0;       // drop trailing newline
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
                bool have = false;                     // already held from the file itself?
                for (const Pin& p : gPins)
                    if (p.baked && p.page == pp && fabs(p.x - xx) < 2.0 && fabs(p.y - yy) < 2.0) { have = true; break; }
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
    gTextPage = nullptr;                           // handles live in the cache now
    gPage = nullptr;
    flushPageCache();
    if (gDoc) FPDF_CloseDocument(gDoc); gDoc = nullptr;
    // pdfium wants UTF-8; Windows gave us UTF-16
    int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> u8(n);
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, u8.data(), n, nullptr, nullptr);
    gDoc = FPDF_LoadDocument(u8.data(), nullptr);
    if (!gDoc) { MessageBoxW(gWnd, L"Could not open PDF", L"mnpdf", MB_ICONERROR); return false; }
    gPath = path;
    gPageCount = FPDF_GetPageCount(gDoc);
    gHls.clear();
    gPins.clear();
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
            if (ty != FPDF_ANNOT_HIGHLIGHT && ty != FPDF_ANNOT_TEXT) continue;
            wchar_t src[64] = L"";
            bool ours = FPDFAnnot_GetStringValue(a, "Source", (FPDF_WCHAR*)src, sizeof(src)) > 2
                        && wcsncmp(src, L"mnpdf", 5) == 0;
            if (!ours) continue;                       // foreign note: untouched
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
    gFitWidth = true;
    applyFitWidth();
    loadPage(0);
    std::vector<Hl> hlFile = gHls;                   // what the file itself held
    std::vector<Pin> pinFile = gPins;
    restoreSidecar(path);                          // saved zoom/page wins over defaults
    if (!gTomb.empty()) markDirty();                // a baked mark has a pending deletion
    else for (const Hl& h : gHls) {                 // sidecar edits beyond the file?
        bool found = false;
        for (const Hl& f : hlFile)
            if (h.page == f.page && h.start == f.start && h.count == f.count && h.color == f.color) found = true;
        if (!found) markDirty();
    }
    clampScroll();
    renderPage();
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
        FPDFText_ClosePage(tp);
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

static bool doSave();

// returns true when the document ended up saved (or had nothing to save)
static bool doSaveImpl(const std::wstring& target) {
    if (!gDoc) return false;
    if (!gDirty) return true;
    if (bakeAndWrite(target)) {
        gHls.clear();                              // highlights now live in the file
        gPins.clear();                             // so do pins
        gTomb.clear();                             // deletions are baked in now
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

static void closeColorBox() {
    if (gColorBox) { HWND b = gColorBox; gColorBox = nullptr; DestroyWindow(b); }
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
    gColorBox = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                                WS_POPUP | WS_BORDER | ES_AUTOHSCROLL, bx, by, bw, bh,
                                gWnd, nullptr,
                                (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE), nullptr);
    if (!gColorBox) return;
    gColorBoxBorn = GetTickCount();
    SendMessageW(gColorBox, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(gColorBox, EM_LIMITTEXT, 7, 0);
    gColorBoxBase = (WNDPROC)SetWindowLongPtrW(gColorBox, GWLP_WNDPROC, (LONG_PTR)colorBoxProc);
    ShowWindow(gColorBox, SW_SHOW);
    SetForegroundWindow(gColorBox);
    SetFocus(gColorBox);
}

static LRESULT CALLBACK colorBoxProc(HWND b, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { closeColorBox(); return 0; }
        if (wp == VK_RETURN) {
            int n = GetWindowTextLengthW(b);
            std::vector<wchar_t> buf(n + 1);
            GetWindowTextW(b, buf.data(), n + 1);
            COLORREF c;
            if (parseHexColor(buf.data(), c)) {
                closeColorBox();                   // gone before the mark repaints
                applyCustomColor(c);
            } else {
                MessageBeep(MB_OK);                // not a colour: keep asking
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(gColorBoxBase, b, m, wp, lp);
        if (GetWindowTextLengthW(b) == 0) {        // an empty box says what it wants
            HDC hdc = GetDC(b);
            if (hdc) {
                RECT rc;
                GetClientRect(b, &rc);
                InflateRect(&rc, -3, -2);
                HFONT old = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
                COLORREF oldCol = SetTextColor(hdc, RGB(150, 150, 150));
                int oldBk = SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"#rrggbb", -1, &rc, DT_TOP | DT_LEFT | DT_NOCLIP | DT_SINGLELINE);
                SetBkMode(hdc, oldBk);
                SetTextColor(hdc, oldCol);
                SelectObject(hdc, old);
                ReleaseDC(b, hdc);
            }
        }
        return r;
    }
    case WM_KILLFOCUS:
        if (GetTickCount() - gColorBoxBorn > 400) closeColorBox();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && GetTickCount() - gColorBoxBorn > 400) {
            closeColorBox(); return 0;
        }
        break;
    }
    return CallWindowProcW(gColorBoxBase, b, m, wp, lp);
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
        }
        placeSearchBar();
        hidePinTip(h);                            // the refit moved the tip's anchor
        return 0;
    case WM_MOVE:                                 // both note boxes are owned popups:
        if (gEditPin >= 0 && gPinBox) sizePinBoxToText();   // they do not follow the window
        hidePinTip(h);
        return 0;
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
        hidePinTip(h);
        SetFocus(h);
        SetCapture(h);
        gSelDrag = true;
        int pg;
        int idx = charIndexClamped(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &pg);
        if (gLastWasDbl && GetMessageTime() - (LONG)gLastDblTime < (LONG)GetDoubleClickTime()
            && abs(GET_X_LPARAM(lp) - gLastDblX) < 8 && abs(GET_Y_LPARAM(lp) - gLastDblY) < 8) {
            selectLine(pg, idx);                    // third click: whole line
            gMultiClick = true;
        } else {
            gMultiClick = false;
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
        gMultiClick = true;
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
            if (gDragPt.y < 56) edge = -1;         // near the top margin: scroll up
            else if (gDragPt.y > gClientH - 56) edge = 1;
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
        gMultiClick = false;                       // drag stays a text selection
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
            gScrollY -= (int)(delta / 120.0 * 3 * 40.0);   // pure scroll; pages are stacked
            clampScroll();
            ensureActivePage();
            markSave();
            renderPage();
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int page = clientH() * 9 / 10;
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        bool scrolled = false;
        if (wp == VK_NEXT || wp == ' ') { gScrollY += page; clampScroll(); ensureActivePage(); scrolled = true; }
        else if (wp == VK_PRIOR) { gScrollY -= page; clampScroll(); ensureActivePage(); scrolled = true; }
        else if (wp == VK_DOWN)  { gScrollY += 60; clampScroll(); ensureActivePage(); scrolled = true; }
        else if (wp == VK_UP)    { gScrollY -= 60; clampScroll(); ensureActivePage(); scrolled = true; }
        else if (wp == VK_LEFT)  { gScrollX -= 60; clampScroll(); scrolled = true; }
        else if (wp == VK_RIGHT) { gScrollX += 60; clampScroll(); scrolled = true; }
        else if (wp == '0' && ctrl) fitWidth();
        else if (wp == 'F' && ctrl) toggleSearch(true);
        else if (wp == 'O' && ctrl) openDialog();
        else if (wp == 'C' && ctrl) copySelection();
        else if (wp == VK_F3 && !gSearchOpen) toggleSearch(true);
        else if (wp == VK_F3) nextMatch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
        else if (wp == 'S' && ctrl) { (GetKeyState(VK_SHIFT) & 0x8000) ? doSaveAs() : doSave(); }
        else if (wp == 'Z' && ctrl) { (GetKeyState(VK_SHIFT) & 0x8000) ? redoOp() : undoOp(); }
        else if (wp == 'Y' && ctrl) redoOp();
        else if ((wp == VK_ADD || wp == VK_OEM_PLUS)) zoomAt(1.2, clientW() / 2, clientH() / 2);
        else if ((wp == VK_SUBTRACT || wp == VK_OEM_MINUS)) zoomAt(1 / 1.2, clientW() / 2, clientH() / 2);
        else if (wp == VK_ESCAPE) { if (gSearchOpen) toggleSearch(false); else PostMessageW(h, WM_CLOSE, 0, 0); }
        if (scrolled) { markSave(); renderPage(); }
        return 0;
    }
    case WM_CONTEXTMENU: {                         // right click
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
        gMenuRecolor = false;
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
            gMenuRecolor = true;
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
        AppendMenuW(menu, MF_STRING, 110, L"Minimize");
        AppendMenuW(menu, MF_STRING, 111, IsZoomed(h) ? L"Restore" : L"Maximize");
        AppendMenuW(menu, MF_STRING, 113, gTitlebar ? L"Hide titlebar" : L"Show titlebar");
        AppendMenuW(menu, MF_STRING, 112, L"Quit");
        SetForegroundWindow(h);   // TrackPopupMenu dismisses instantly without foreground
        int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, h, nullptr);
        DestroyMenu(menu);
        if (cmd) SendMessageW(h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);   // sync: focus is still ours
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case 101: copySelection(); return 0;
        case 103: toggleSearch(true); return 0;
        case 104: fitWidth(); return 0;
        case 105: zoomAt(1.2, clientW() / 2, clientH() / 2); return 0;
        case 106: zoomAt(1 / 1.2, clientW() / 2, clientH() / 2); return 0;
        case 107: openDialog(); return 0;
        case 116:                                   // copy highlight text
            if (gMenuHl >= 0 && gMenuHl < (int)gHls.size()) {
                const Hl& h2 = gHls[gMenuHl];
                copyTextRange(h2.page, h2.start, h2.page, h2.start + h2.count - 1);
            }
            return 0;
        case 117:                                   // delete highlight
            if (gMenuHl >= 0 && gMenuHl < (int)gHls.size()) deleteHighlight(gMenuHl);
            return 0;
        case 135:                                   // Highlight with the default color
            applyHighlightFromSelection(gHlDefault);
            return 0;
        case 140: case 141: case 142: case 143: case 144: case 145: {
            int c = id - 140;                        // set the default highlight color
            gHlDefault = c;
            writeAppPref();
            if (hasSelection()) applyHighlightFromSelection(gHlDefault);
            return 0;
        }
        case 120: case 121: case 122: case 123: case 124: case 125: {   // highlight Color submenu, presets
            recolorHighlight(gMenuHl, id - 120);
            return 0;
        }
        case 126: case 127: case 128:                 // recolor a highlight from a custom slot
            recolorHighlight(gMenuHl, kPalPreset + (id - 126));
            return 0;
        case 146: case 147: case 148:                 // default highlight color from a custom slot
            gHlDefault = kPalPreset + (id - 146);
            writeAppPref();
            if (hasSelection()) applyHighlightFromSelection(gHlDefault);
            return 0;
        case 150: case 151: case 152: case 153: case 154: case 155:
            recolorPin(gMenuPin, id - 150);
            return 0;
        case 156: case 157: case 158:                 // recolor a pin from a custom slot
            recolorPin(gMenuPin, kPalPreset + (id - 156));
            return 0;
        case 160: case 161: case 162: case 163: case 164: case 165: {
            gPinDefault = id - 160;                  // set the default pin color
            writeAppPref();
            return 0;
        }
        case 166: case 167: case 168:                 // default pin color from a custom slot
            gPinDefault = kPalPreset + (id - 166);
            writeAppPref();
            return 0;
        // "Custom..." rows: one per colour menu, so map each id to its target
        case 136: gColorTarget = 0; startColorEntry(); return 0;   // recolor a highlight
        case 159: gColorTarget = 1; startColorEntry(); return 0;   // recolor a pin
        case 149: gColorTarget = 2; startColorEntry(); return 0;   // default highlight
        case 169: gColorTarget = 3; startColorEntry(); return 0;   // default pin
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
            return 0;
        case 110: PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0); return 0;
        case 111: PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0); return 0;
        case 112: PostMessageW(h, WM_CLOSE, 0, 0); return 0;
        case 113: toggleTitlebar(h); return 0;
        case 114: undoOp(); return 0;
        case 115: redoOp(); return 0;
        case 118: doSave(); return 0;
        case 119: doSaveAs(); return 0;
        case 130:                                   // add pin at the menu drop point
            if (gMenuPinPtPage >= 0) {
                addPinAt(gMenuPinPtPage, gMenuPinPtX, gMenuPinPtY);
            } else if (gDoc) {                      // fallback (posted id): center of the active page
                addPinAt(gPageIndex, gPageW[gPageIndex] / 2, gPageH[gPageIndex] / 2);
            }
            return 0;
        case 131:                                   // edit pin text
            if (gMenuPin >= 0) startPinEdit(gMenuPin, false);
            return 0;
        case 132: deletePin(gMenuPin); return 0;
        case 133: rotatePage(gMenuRotPage >= 0 ? gMenuRotPage : gPageIndex, +1); return 0;
        case 134: rotatePage(gMenuRotPage >= 0 ? gMenuRotPage : gPageIndex, -1); return 0;
        }
        if (id == 2) { toggleSearch(true); return 0; }   // Edit > Find
        if (id == 1 && HIWORD(wp) == EN_CHANGE && gSearchOpen) {
            GetWindowTextW(gSearchBox, gQuery, 128);
            runSearch();
        }
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT res = DefWindowProcW(h, m, wp, lp);
        if (!gTitlebar && res == HTCLIENT) {        // captionless: top strip drags
            POINT pt2 = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt2);
            if (pt2.y < 8) res = HTCAPTION;
        }
        return res;
    }
    case WM_TIMER:
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
            const int zone = 56;                        // the in-window margin
            int depth = gEdgeScroll > 0 ? gDragPt.y - (gClientH - zone) : zone - gDragPt.y;
            if (depth < 0) depth = 0;
            const int maxDepth = zone * 2;              // held well past the window: top speed
            if (depth > maxDepth) depth = maxDepth;
            double frac = (double)depth / zone;         // 1.0 at the window edge, 2.0 past it
            if (frac < 0.2) frac = 0.2;                 // never a dead stop
            int ph = pageHpx(gPageIndex);               // one page of scroll, in px
            if (ph < 64) ph = gClientH > 64 ? gClientH : 64;
            double step = ph * 0.017 * frac;            // ~1.8 s per page at the window edge
            gScrollY += gEdgeScroll * (int)step;
            clampScroll();
            ensureActivePage();
            markSave();
            int pg;
            int idx = charIndexClamped(gDragPt.x, gDragPt.y, &pg);
            if (idx >= 0) {
                gSelHeadPage = pg;
                gSelHead = idx;
            }
            renderPage();
        }
        return 0;
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
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
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
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    *wcsrchr(exeDir, L'\\') = 0;
    SetDllDirectoryW(exeDir);

    loadAppPref();                                 // titlebar preference
    gWnd = CreateWindowExW(0, L"mnpdf", L"mnpdf",
                           WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW | WS_VISIBLE & (gTitlebar ? ~0u : ~WS_CAPTION),
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
                           nullptr, nullptr, hInst, nullptr);

    gVerboseTitle = _wgetenv(L"MNPDF_VERBOSE") != nullptr;   // debug harness opt-in
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

    ShowWindow(gWnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    flushPageCache();                              // releases gPage/gTextPage too
    if (gDoc) FPDF_CloseDocument(gDoc);
    FPDF_DestroyLibrary();
    if (gNoteFont) { DeleteObject(gNoteFont); gNoteFont = nullptr; gNoteFontPx = 0; }
    if (gGdiOk) Gdiplus::GdiplusShutdown(gGdiToken);
    return 0;
}
