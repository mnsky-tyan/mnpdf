// mnpdf for Linux - core document model: state, geometry, palette, undo, marks.
// Ported from the Windows src/main.cpp; the platform surface (GTK) lives in
// main.cpp and render.cpp. See doc.h for the shape of the split.
#include "doc.h"
#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>

// ---- state ----
Doc* gDocs[kMaxTabs] = {};
int gDocCount = 1;
int gActiveDoc = 0;

int gClientW = 0, gClientH = 0;
bool gNight = false;
bool gTabStrip = true;
bool gAutosave = true;
// the OS caption strip is shown, mirroring Windows' gTitlebar (src/main.cpp:341).
// Hiding it on Linux uses gtk_window_set_decorated, which is the honest analogue
// of clearing WS_CAPTION: the window manager still owns resizing and snapping,
// so no WM_NCHITTEST reimplementation is needed as it was on Windows.
bool gTitlebar = true;
bool gSearchOpen = false;
std::wstring gQuery;

int gHlDefault = 0;
int gPinDefault = kPalPreset - 1;

const COLORREF gPalPreset[kPalPreset] = {
    RGB(255, 230, 128), RGB(195, 242, 189), RGB(184, 220, 255),
    RGB(255, 201, 212), RGB(255, 217, 173), RGB(230, 71, 77),
};
const char* const gPalName[kPalPreset] = {
    "Yellow", "Green", "Blue", "Pink", "Orange", "Red" };
COLORREF gPalCustom[kPalCustom] = { CLR_INVALID, CLR_INVALID, CLR_INVALID };
int gPalNext = 0;

static bool gSaveDirty = false;   // sidecar write pending (flushed by a timer)

// ---- palette ----
bool palColor(int idx, COLORREF& out) {
    if (idx < 0 || idx >= kPalCount) return false;
    if (idx < kPalPreset) { out = gPalPreset[idx]; return true; }
    if (gPalCustom[idx - kPalPreset] == CLR_INVALID) return false;
    out = gPalCustom[idx - kPalPreset];
    return true;
}

std::wstring palLabel(int idx) {
    if (idx >= 0 && idx < kPalPreset) {
        std::wstring n; for (const char* p = gPalName[idx]; *p; p++) n += (wchar_t)*p; return n;
    }
    COLORREF c;
    if (palColor(idx, c)) {
        wchar_t b[16];
        swprintf(b, 16, L"#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
        return b;
    }
    return L"";
}

int nearestPalColor(unsigned r, unsigned g, unsigned b) {
    int color = -1, best = 1 << 30;
    for (int c = 0; c < kPalCount; c++) {
        COLORREF pc;
        if (!palColor(c, pc)) continue;
        int dr = (int)r - GetRValue(pc), dg = (int)g - GetGValue(pc), db = (int)b - GetBValue(pc);
        int d = dr * dr + dg * dg + db * db;
        if (d < best) { best = d; color = c; }
    }
    return color;
}

// ---- string helpers ----
std::wstring utf8ToWide(const std::string& s) {
    std::wstring out;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4; }
        else { cp = 0xFFFD; len = 1; }
        i += len;
        out += (wchar_t)cp;            // one wchar_t per code point (no surrogates)
    }
    return out;
}

// std::wstring (code points) <-> PDFium's UTF-16 (unsigned short)
std::vector<unsigned short> wideToU16(const std::wstring& s) {
    std::vector<unsigned short> out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = (uint32_t)s[i];
        if (cp < 0x10000) out.push_back((unsigned short)cp);
        else {
            cp -= 0x10000;
            out.push_back((unsigned short)(0xD800 + (cp >> 10)));
            out.push_back((unsigned short)(0xDC00 + (cp & 0x3FF)));
        }
    }
    out.push_back(0);
    return out;
}

std::wstring u16ToWide(const unsigned short* p, size_t n) {
    std::wstring out;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = p[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && p[i+1] >= 0xDC00 && p[i+1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (p[++i] - 0xDC00);
        }
        out += (wchar_t)c;
    }
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = (uint32_t)s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)s[++i] - 0xDC00);
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// ---- geometry ----
double clampZoom(double z) { return z < 0.25 ? 0.25 : (z > 8.0 ? 8.0 : z); }
int docWpx() { return (int)(gMaxPageW * gZoom); }
int rotWpt(int i) { return (gRot[i] & 1) ? (int)(gPageH[i] + 0.5) : (int)(gPageW[i] + 0.5); }
int rotHpt(int i) { return (gRot[i] & 1) ? (int)(gPageW[i] + 0.5) : (int)(gPageH[i] + 0.5); }
int pageWpx(int i) { return (int)(rotWpt(i) * gZoom); }
int pageHpx(int i) { return (int)(rotHpt(i) * gZoom); }
int yTopPx(int i) { return (int)(gPrefixPt[i] * gZoom) + i * GAP + MARGIN; }
int docHpx() { return gPrefixPt ? (int)(gPrefixPt[gDocPages] * gZoom) + gDocPages * GAP + 2 * MARGIN : 0; }

int tabStripH() {
    // The Windows app draws its own tab strip inside the client area and the
    // document sits below it; here GtkNotebook draws the strip outside the
    // drawing area, so the canvas never reserves a band for it. The function
    // stays because the ported hit-test and layout code calls it.
    return 0;
}
int docViewH() { int h = gClientH - tabStripH(); return h > 0 ? h : 0; }
int docLeft() { int slack = gClientW - (docWpx() + 2 * MARGIN); return slack > 0 ? slack / 2 : -gScrollX; }
int docTop() { int slack = docViewH() - docHpx(); return slack > 0 ? tabStripH() + slack / 2 : tabStripH() - gScrollY; }
int pageLeftPx(int i) { return MARGIN + (docWpx() - pageWpx(i)) / 2; }
int pageVx(int i) { return docLeft() + pageLeftPx(i); }
int pageVy(int i) { return docTop() + yTopPx(i); }

bool pageInView(int page) {
    int vy = pageVy(page), vh = pageHpx(page);
    return !(vy + vh < 0 || vy > gClientH);
}

bool clipPageRect(int page, int* x0, int* y0, int* x1, int* y1) {
    int vx = pageVx(page), vy = pageVy(page);
    int w = pageWpx(page), h = pageHpx(page);
    int lx = vx > 0 ? vx : 0, ly = vy > 0 ? vy : 0;
    int hx = vx + w < gClientW ? vx + w : gClientW;
    int hy = vy + h < gClientH ? vy + h : gClientH;
    if (x0) *x0 = lx;
    if (y0) *y0 = ly;
    if (x1) *x1 = hx;
    if (y1) *y1 = hy;
    return hx > lx && hy > ly;
}

void clampScroll() {
    if (!gPrefixPt || !gDocPages) { gScrollX = gScrollY = 0; return; }
    int maxX = docWpx() + 2 * MARGIN - gClientW;
    int maxY = docHpx() - docViewH();
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    if (gScrollX > maxX) gScrollX = maxX;
    if (gScrollY > maxY) gScrollY = maxY;
    if (gScrollX < 0) gScrollX = 0;
    if (gScrollY < 0) gScrollY = 0;
}

void relayoutPages() {
    if (!gPrefixPt || !gDocPages) return;
    gRestoredPage = false;
    double acc = 0;
    gMaxPageW = 0;
    for (int i = 0; i < gDocPages; i++) {
        gPrefixPt[i] = acc;
        acc += rotHpt(i);
        if (rotWpt(i) > (int)gMaxPageW) gMaxPageW = rotWpt(i);
    }
    gPrefixPt[gDocPages] = acc;
}

void allocPageTables(int pages) {
    delete[] gPageW; delete[] gPageH; delete[] gPrefixPt; delete[] gRot;
    gDocPages = pages;
    gPageW = new double[gDocPages ? gDocPages : 1];
    gPageH = new double[gDocPages ? gDocPages : 1];
    gPrefixPt = new double[gDocPages + 1];
    gRot = new int[gDocPages ? gDocPages : 1];
}

void pagePtToPx(int page, double x, double y, double* ox, double* oy) {
    double W = gPageW[page], H = gPageH[page];
    switch (gRot[page] & 3) {
    case 0:  *ox = x * gZoom;       *oy = (H - y) * gZoom; break;
    case 1:  *ox = y * gZoom;       *oy = x * gZoom;       break;
    case 2:  *ox = (W - x) * gZoom; *oy = y * gZoom;       break;
    case 3:  *ox = (H - y) * gZoom; *oy = (W - x) * gZoom; break;
    }
}

void pagePxToPt(int page, double sx, double sy, double* ox, double* oy) {
    double W = gPageW[page], H = gPageH[page];
    switch (gRot[page] & 3) {
    case 0:  *ox = sx / gZoom;      *oy = H - sy / gZoom;  break;
    case 1:  *ox = sy / gZoom;      *oy = sx / gZoom;      break;
    case 2:  *ox = W - sx / gZoom;  *oy = sy / gZoom;      break;
    case 3:  *ox = W - sy / gZoom;  *oy = H - sx / gZoom;  break;
    }
}

// ---- page cache ----
#define PCACHE 6
static int gPcIndex[PCACHE];
static FPDF_PAGE gPcPage[PCACHE];
static FPDF_TEXTPAGE gPcText[PCACHE];
static unsigned gPcTick[PCACHE];
static unsigned gPcClock;
static const Doc* gPcOwner[PCACHE] = {};
static bool gPcInit = false;

static void pcInitOnce() {
    if (gPcInit) return;
    for (int k = 0; k < PCACHE; k++) { gPcIndex[k] = -1; gPcPage[k] = nullptr; gPcText[k] = nullptr; }
    gPcInit = true;
}

void flushPageCache() {
    pcInitOnce();
    for (int k = 0; k < PCACHE; k++) {
        if (gPcText[k]) { FPDFText_ClosePage(gPcText[k]); gPcText[k] = nullptr; }
        if (gPcPage[k]) FPDF_ClosePage(gPcPage[k]);
        gPcPage[k] = nullptr;
        gPcIndex[k] = -1;
        gPcOwner[k] = nullptr;
    }
    for (int d = 0; d < gDocCount; d++) {       // every bundle aliased the shared cache
        gDocs[d]->page = nullptr;
        gDocs[d]->textPage = nullptr;
    }
}

FPDF_PAGE acquirePage(int i) {
    pcInitOnce();
    if (!gDoc || i < 0 || i >= gDocPages) return nullptr;
    int slot = -1, oldest = -1;
    for (int k = 0; k < PCACHE; k++) {
        if (gPcIndex[k] == i && gPcOwner[k] == (const Doc*)gViewTab) { gPcTick[k] = ++gPcClock; return gPcPage[k]; }
        if (gPcIndex[k] < 0) { slot = k; break; }
        if (gPcIndex[k] != gPageIndex && (oldest < 0 || gPcTick[k] < gPcTick[oldest])) oldest = k;
    }
    if (slot < 0) slot = oldest;
    if (slot < 0) return gPage;
    if (gPcText[slot]) { FPDFText_ClosePage(gPcText[slot]); gPcText[slot] = nullptr; }
    if (gPcPage[slot]) FPDF_ClosePage(gPcPage[slot]);
    gPcPage[slot] = nullptr;
    gPcIndex[slot] = i;
    gPcOwner[slot] = (const Doc*)gViewTab;
    gPcPage[slot] = FPDF_LoadPage(gDoc, i);
    if (!gPcPage[slot]) gPcIndex[slot] = -1;
    gPcTick[slot] = ++gPcClock;
    return gPcPage[slot];
}

FPDF_TEXTPAGE textPageOf(int page) {
    if (!acquirePage(page)) return nullptr;
    for (int k = 0; k < PCACHE; k++)
        if (gPcIndex[k] == page && gPcOwner[k] == (const Doc*)gViewTab) {
            if (!gPcText[k]) gPcText[k] = FPDFText_LoadPage(gPcPage[k]);
            return gPcText[k];
        }
    return nullptr;
}

bool loadPage(int index) {
    if (!gDoc || gDocPages <= 0) { gPageIndex = 0; gPage = nullptr; gTextPage = nullptr; return false; }
    if (index < 0) index = 0;
    if (index >= gDocPages) index = gDocPages - 1;
    FPDF_PAGE p = acquirePage(index);
    if (!p) {
        gPage = nullptr; gTextPage = nullptr;
        gPageIndex = index; gPageWpt = gPageW[index]; gPageHpt = gPageH[index];
        return false;
    }
    FPDF_TEXTPAGE t = textPageOf(index);
    gPage = p; gTextPage = t; gPageIndex = index;
    gPageWpt = gPageW[index]; gPageHpt = gPageH[index];
    return true;
}

void ensureActivePage() {
    if (!gDoc || gRestoredPage) return;
    int cy = gScrollY + docViewH() / 2;
    for (int i = 0; i < gDocPages; i++) {
        if (cy < yTopPx(i) + pageHpx(i)) { if (i != gPageIndex) loadPage(i); return; }
    }
}

bool applyFitWidth() {
    int cw = gClientW - 2 * MARGIN;
    if (cw <= 0 || gMaxPageW <= 0) return false;
    double nz = clampZoom(cw / gMaxPageW);
    if (nz == gZoom) return false;
    gZoom = nz;
    if (gRestoredPage) gScrollY = yTopPx(gPageIndex) - MARGIN;
    return true;
}

void fitWidth() { gFitWidth = true; applyFitWidth(); clampScroll(); markSave(); renderPage(); }

void zoomAt(double factor, int cx, int cy) {
    double nz = clampZoom(gZoom * factor);
    if (nz == gZoom) return;
    gRestoredPage = false;
    gFitWidth = false;
    double oldH = docHpx(), oldW = docWpx() + 2 * MARGIN;
    gZoom = nz;
    double newH = docHpx(), newW = docWpx() + 2 * MARGIN;
    gScrollX = (int)((gScrollX + cx) * newW / oldW) - cx;
    gScrollY = (int)((gScrollY + cy - tabStripH()) * newH / oldH) - cy + tabStripH();
    clampScroll();
    markSave();
    renderPage();
}

// ---- undo engine ----
void markDirty() { if (!gDirty) gDirty = true; }
void markSave() { if (gAutosave) gSaveDirty = true; }
bool sidecarPending() { return gSaveDirty; }
void sidecarClearPending() { gSaveDirty = false; }

static bool hlOverlap(const Hl& a, const Hl& b) {
    return a.page == b.page && a.start < b.start + b.count && b.start < a.start + a.count;
}
static int hlIndexOf(const Hl& v) {
    for (size_t k = 0; k < gHls.size(); k++)
        if (gHls[k].page == v.page && gHls[k].start == v.start && gHls[k].count == v.count && gHls[k].color == v.color)
            return (int)k;
    return -1;
}
static bool pinSame(const Pin& a, const Pin& b) {
    return a.page == b.page && fabs(a.x - b.x) < kSpotTolPt && fabs(a.y - b.y) < kSpotTolPt && a.text == b.text;
}
static int pinIndexOf(const Pin& v, int color) {
    for (size_t k = 0; k < gPins.size(); k++)
        if (pinSame(gPins[k], v) && gPins[k].color == color) return (int)k;
    return -1;
}
static void pinEraseAt(int idx) { if (idx >= 0 && idx < (int)gPins.size()) gPins.erase(gPins.begin() + idx); }
static void pinInsertAt(int idx, const Pin& pin) { if (idx >= 0 && idx <= (int)gPins.size()) gPins.insert(gPins.begin() + idx, pin); }
static void pinSetTextAt(int idx, const std::wstring& text) { if (idx >= 0 && idx < (int)gPins.size()) gPins[idx].text = text; }
static void hlRecolorAt(int page, int start, int count, int color) {
    for (Hl& h : gHls) if (h.page == page && h.start == start && h.count == count) { h.color = color; break; }
}
static void rotApply(int page, int rot) {
    if (gRot && page >= 0 && page < gDocPages) {
        gRot[page] = rot; relayoutPages(); clampScroll(); ensureActivePage();
        refreshDrawers();          // that drawer row shows a different picture now
    }
}

void pushOp(const Op& op) { gUndo.push_back(op); gRedo.clear(); markDirty(); markSave(); }

void undoOp() {
    if (gUndo.empty()) return;
    Op op = gUndo.back(); gUndo.pop_back();
    switch (op.kind) {
    case 0: for (const Hl& a : op.added) { int i = hlIndexOf(a); if (i >= 0) gHls.erase(gHls.begin() + i); }
            for (const Hl& r : op.replaced) { gHls.push_back(r); }
            break;
    case 1: gHls.push_back(op.hlVal); break;
    case 2: hlRecolorAt(op.hlVal.page, op.hlVal.start, op.hlVal.count, op.hlFrom); break;
    case 3: pinEraseAt(op.pinIdx); break;
    case 4: pinInsertAt(op.pinIdx, op.pinVal); break;   // pindel: restore
    case 5: pinSetTextAt(op.pinIdx, op.textFrom); break;
    case 6: rotApply(op.rotPage, op.rotFrom); break;
    case 7: { int i = pinIndexOf(op.pinVal, op.hlTo); if (i >= 0) { gPins[i].color = op.hlFrom; op.pinIdx = i; } }
        break;
    }
    gRedo.push_back(op);      // the op moves to the redo stack: undo/redo walk it back and forth
    markDirty();                                   // the undone state is unsaved work too
    markSave();                                    // sidecar follows the undone state
    renderPage();
}

void redoOp() {
    if (gRedo.empty()) return;
    Op op = gRedo.back(); gRedo.pop_back();
    switch (op.kind) {
    case 0:                                       // hladd: drop the displaced, re-add the new
        for (const Hl& r : op.replaced) { int i = hlIndexOf(r); if (i >= 0) gHls.erase(gHls.begin() + i); }
        for (const Hl& a : op.added) gHls.push_back(a);
        break;
    case 1:                                       // hldel: remove again
        { int i = hlIndexOf(op.hlVal); if (i >= 0) gHls.erase(gHls.begin() + i); }
        break;
    case 2: hlRecolorAt(op.hlVal.page, op.hlVal.start, op.hlVal.count, op.hlTo); break;
    case 3: pinInsertAt(op.pinIdx, op.pinVal); break;
    case 4: pinEraseAt(op.pinIdx); break;
    case 5: pinSetTextAt(op.pinIdx, op.textTo); break;
    case 6: rotApply(op.rotPage, op.rotTo); break;
    case 7: {                                     // pinrecolor: the pin undo repainted, else that colour
            int i = -1;
            if (op.pinIdx >= 0 && op.pinIdx < (int)gPins.size()
                && pinSame(gPins[op.pinIdx], op.pinVal) && gPins[op.pinIdx].color == op.hlFrom) i = op.pinIdx;
            if (i < 0) i = pinIndexOf(op.pinVal, op.hlFrom);
            if (i >= 0) gPins[i].color = op.hlTo;
        }
        break;
    }
    gUndo.push_back(op);       // symmetrically, redo hands the op back to the undo stack
    markDirty();                                   // the redone state is unsaved work too
    markSave();                                    // sidecar follows the redone state
    renderPage();
}

// ---- marks ----
int hlAt(int page, int idx) {
    if (idx < 0) return -1;
    for (size_t k = 0; k < gHls.size(); k++) {
        const Hl& h = gHls[k];
        if (h.page == page && idx >= h.start && idx < h.start + h.count) return (int)k;
    }
    return -1;
}

void normalizePins() {
    for (int k = (int)gPins.size() - 1; k >= 0; k--) {
        for (int j = k + 1; j < (int)gPins.size(); j++) {
            if (pinSame(gPins[j], gPins[k])) { gPins.erase(gPins.begin() + k); break; }
        }
    }
}

void normalizeHls() {
    for (int k = (int)gHls.size() - 1; k >= 0; k--) {
        for (int j = k + 1; j < (int)gHls.size(); j++) {
            if (hlOverlap(gHls[k], gHls[j])) { gHls.erase(gHls.begin() + k); break; }
        }
    }
}

void applyHighlightFromSelection(int color) {
    if (!hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    Op op = {}; op.kind = 0;
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        Hl seg = { p, s, e - s + 1, color, false };
        for (int k = (int)gHls.size() - 1; k >= 0; k--) {
            if (hlOverlap(seg, gHls[k])) { op.replaced.push_back(gHls[k]); gHls.erase(gHls.begin() + k); }
        }
        gHls.push_back(seg);
        op.added.push_back(seg);
    }
    clearSelection();
    normalizeHls();
    if (!op.added.empty()) pushOp(op);
    renderPage();
}

void recolorHighlight(int idx, int color) {
    if (idx < 0 || idx >= (int)gHls.size() || gHls[idx].color == color) return;
    Op op = {}; op.kind = 2; op.hlVal = gHls[idx]; op.hlFrom = gHls[idx].color; op.hlTo = color;
    gHls[idx].color = color;
    pushOp(op);
    renderPage();
}

void deleteHighlight(int idx) {
    if (idx < 0 || idx >= (int)gHls.size()) return;
    Op op = {}; op.kind = 1; op.hlVal = gHls[idx];
    if (gHls[idx].baked) gTomb.push_back({ 0, gHls[idx].page, gHls[idx].start, gHls[idx].count, 0, 0 });
    gHls.erase(gHls.begin() + idx);
    pushOp(op);
    renderPage();
}

int pinRadius() { int r = (int)(3.0 * gZoom); if (r < 3) r = 3; if (r > 14) r = 14; return r; }

void addPinAt(int page, double px, double py) {
    if (gDocPages <= 0 || page < 0 || page >= gDocPages) return;
    gPins.push_back({ page, px, py, L"", gPinDefault, false });
    renderPage();
}

void deletePin(int idx) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    Op op = {}; op.kind = 4; op.pinIdx = idx; op.pinVal = gPins[idx];
    if (gPins[idx].baked) gTomb.push_back({ 1, gPins[idx].page, 0, 0, gPins[idx].x, gPins[idx].y });
    gPins.erase(gPins.begin() + idx);
    pushOp(op);
    renderPage();
}

void recolorPin(int idx, int color) {
    if (idx < 0 || idx >= (int)gPins.size() || gPins[idx].color == color) return;
    COLORREF probe; if (!palColor(color, probe)) return;
    Op op = {}; op.kind = 7; op.pinVal = gPins[idx]; op.hlFrom = gPins[idx].color; op.hlTo = color; op.pinIdx = idx;
    gPins[idx].color = color;
    pushOp(op);
    renderPage();
}

void rotatePage(int page, int dir) {
    if (!gRot || page < 0 || page >= gDocPages) return;
    Op op = {}; op.kind = 6; op.rotPage = page; op.rotFrom = gRot[page];
    gRot[page] = (gRot[page] + (dir > 0 ? 1 : 3)) & 3;
    op.rotTo = gRot[page];
    relayoutPages(); clampScroll(); ensureActivePage();
    pushOp(op);
    refreshDrawers();              // the Windows build drops that row's picture here
    renderPage();
}

int pinAt(int cx, int cy) {
    if (cy < tabStripH()) return -1;
    if (!gPrefixPt || !gDocPages) return -1;
    int i = pageAtDocY(cy - docTop());
    if (i < 0) return -1;
    int docY = cy - docTop();
    double px, py;
    pagePxToPt(i, cx - docLeft() - pageLeftPx(i), docY - yTopPx(i), &px, &py);
    double hr = (double)pinRadius() / gZoom;
    double minHr = 8.0 / gZoom;
    if (hr < minHr) hr = minHr;
    for (int k = 0; k < (int)gPins.size(); k++) {
        const Pin& pn = gPins[k];
        if (pn.page == i && fabs(pn.x - px) < hr && fabs(pn.y - py) < hr) return k;
    }
    return -1;
}

// ---- selection ----
int pageAtDocY(int docY) {
    for (int i = 0; i < gDocPages; i++) {
        int top = yTopPx(i);
        if (docY >= top && docY < top + pageHpx(i)) return i;
    }
    return -1;
}

static int charIndexWithTol(FPDF_TEXTPAGE tp, double px, double py, int maxPx) {
    for (int px10 = 4; px10 <= maxPx; px10 *= 2) {
        double tol = px10 / gZoom;
        int idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
        if (idx >= 0) return idx;
    }
    return -1;
}

static int snapIndexNear(int page, int cx, int cy) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    int n = FPDFText_CountChars(tp);
    if (n <= 0) return -1;
    double ppx = cx - docLeft() - pageLeftPx(page);
    double ppy = cy - docTop() - yTopPx(page);
    double px, py;
    pagePxToPt(page, ppx, ppy, &px, &py);
    double H = gPageH[page];
    int rot = gRot[page] & 3;
    if (rot == 0) { if (py >= H) return 0; if (py <= 0) return n - 1; }
    else if (rot == 2) { if (py <= 0) return n - 1; if (py >= H) return 0; }
    int idx = charIndexWithTol(tp, px, py, 128);
    if (idx >= 0) return idx;
    int best = 0, bestEnd = 0;
    double bestDist = 1e18;
    for (int i = 0; i < n; i += 8) {
        double l, r, b, t;
        if (!FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) continue;
        double mid = (t + b) / 2;
        double d = mid > py ? mid - py : py - mid;
        if (d < bestDist) {
            bestDist = d; best = i;
            int j = i; double l2, r2, b2, t2;
            while (j + 1 < n && FPDFText_GetCharBox(tp, j + 1, &l2, &r2, &b2, &t2) && (t2 + b2) / 2 <= mid + 1 && (t2 + b2) / 2 >= mid - 1) j++;
            bestEnd = j;
        }
    }
    double bl, br, bb, bt;
    if (!FPDFText_GetCharBox(tp, best, &bl, &br, &bb, &bt)) return n - 1;
    return px * 2 < bl + br ? best : bestEnd;
}

int charIndexAtDoc(int cx, int cy, int* pageOut) {
    *pageOut = -1;
    if (cy < tabStripH()) return -1;
    if (!gPrefixPt || !gDocPages) return -1;
    int docY = cy - docTop();
    int page = pageAtDocY(docY);
    if (page < 0) {
        if (docY < yTopPx(0)) { *pageOut = 0; return 0; }
        int last = gDocPages - 1;
        int lastBottom = yTopPx(last) + pageHpx(last);
        if (docY >= lastBottom) { *pageOut = last; return -2; }
        for (int i = 0; i < gDocPages - 1; i++) {
            int bot = yTopPx(i) + pageHpx(i);
            int nxtTop = yTopPx(i + 1);
            if (docY >= bot && docY < nxtTop) { *pageOut = i; return (docY - bot <= nxtTop - docY) ? -2 : -3; }
        }
        return -1;
    }
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp) return -1;
    double ppx = cx - docLeft() - pageLeftPx(page);
    double ppy = docY - yTopPx(page);
    double px, py;
    pagePxToPt(page, ppx, ppy, &px, &py);
    int idx = charIndexWithTol(tp, px, py, 32);
    if (idx < 0) idx = snapIndexNear(page, cx, cy);
    if (idx < 0) return -1;
    *pageOut = page;
    return idx;
}

int charIndexClamped(int cx, int cy, int* pageOut) {
    int idx = charIndexAtDoc(cx, cy, pageOut);
    if (*pageOut < 0) return -1;
    if (idx == -2) { FPDF_TEXTPAGE tp = textPageOf(*pageOut); idx = tp ? FPDFText_CountChars(tp) - 1 : -1; }
    else if (idx == -3) { (*pageOut)++; idx = 0; }
    return idx;
}

void clearSelection() { gSelAnchor = gSelHead = -1; gSelAnchorPage = gSelHeadPage = -1; }
bool hasSelection() {
    if (gSelAnchor < 0 || gSelHead < 0) return false;
    if (gSelAnchorPage == gSelHeadPage) return gSelAnchor != gSelHead;
    return true;
}
void selRange(int* p0, int* i0, int* p1, int* i1) {
    *p0 = gSelAnchorPage; *i0 = gSelAnchor; *p1 = gSelHeadPage; *i1 = gSelHead;
    if (*p0 > *p1 || (*p0 == *p1 && *i0 > *i1)) { std::swap(*p0, *p1); std::swap(*i0, *i1); }
}
bool isWordSep(unsigned int u) { return u == ' ' || u == '\t' || u == '\r' || u == '\n' || u == 0xA0; }

void selectWord(int page, int idx) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp || idx < 0) return;
    int n = FPDFText_CountChars(tp);
    int a = idx, b = idx;
    while (a > 0 && !isWordSep(FPDFText_GetUnicode(tp, a - 1))) a--;
    while (b + 1 < n && !isWordSep(FPDFText_GetUnicode(tp, b + 1))) b++;
    gSelAnchorPage = gSelHeadPage = page; gSelAnchor = a; gSelHead = b;
}

void selectLine(int page, int idx) {
    FPDF_TEXTPAGE tp = textPageOf(page);
    if (!tp || idx < 0) return;
    double al, ar, ab, at;
    if (!FPDFText_GetCharBox(tp, idx, &al, &ar, &ab, &at)) return;
    int n = FPDFText_CountChars(tp);
    int a = idx, b2 = idx;
    for (int i = idx - 1; i >= 0; i--) { double l, r, cb, ct; if (!FPDFText_GetCharBox(tp, i, &l, &r, &cb, &ct)) break; if (cb >= at || ct <= ab) break; a = i; }
    for (int i = idx + 1; i < n; i++) { double l, r, cb, ct; if (!FPDFText_GetCharBox(tp, i, &l, &r, &cb, &ct)) break; if (cb >= at || ct <= ab) break; b2 = i; }
    gSelAnchorPage = gSelHeadPage = page; gSelAnchor = a; gSelHead = b2;
}

bool pageSlice(int p, int p0, int i0, int p1, int i1, FPDF_TEXTPAGE* tpOut, int* sOut, int* eOut) {
    FPDF_TEXTPAGE tp = textPageOf(p);
    if (!tp) return false;
    int n = FPDFText_CountChars(tp);
    *sOut = (p == p0) ? i0 : 0;
    *eOut = (p == p1) ? i1 : n - 1;
    if (*eOut < *sOut || *eOut >= n) return false;
    *tpOut = tp;
    return true;
}

void copySelection() {
    if (!hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    copyTextRange(p0, i0, p1, i1);
}

void copyTextRange(int p0, int i0, int p1, int i1) {
    std::vector<unsigned short> buf;
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        size_t base = buf.size();
        buf.resize(base + (size_t)(e - s + 2));
        int got = FPDFText_GetText(tp, s, e - s + 1, (unsigned short*)buf.data() + base);
        if (got <= 1) { buf.resize(base); continue; }
        buf.resize(base + got - 1);
        if (p < p1 && !buf.empty()) { buf.push_back('\r'); buf.push_back('\n'); }
    }
    if (buf.empty()) return;
    std::wstring w;
    for (unsigned short cu : buf) {
        if (cu == 0) break;
        w += (wchar_t)cu;
    }
    ui_copy_text(wideToUtf8(w));
}

// commit a pin note (fresh = the pin was just created; empty text cancels it)
void commitPinText(int idx, bool fresh, const std::wstring& text) {
    if (idx < 0 || idx >= (int)gPins.size()) return;
    if (fresh) {
        if (text.empty()) gPins.erase(gPins.begin() + idx);
        else {
            Op op = {};
            op.kind = 3;
            op.pinIdx = idx;
            op.pinVal = gPins[idx];
            op.pinVal.text = text;
            gPins[idx].text = text;
            pushOp(op);
        }
    } else {
        std::wstring before = gPins[idx].text;
        if (text != before) {
            Op op = {};
            op.kind = 5;
            op.pinIdx = idx;
            op.textFrom = before;
            op.textTo = text;
            gPins[idx].text = text;
            pushOp(op);
        }
    }
    renderPage();
}
