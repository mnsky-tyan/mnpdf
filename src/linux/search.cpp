// mnpdf for Linux - find. Ported from the Windows runSearch/nextMatch.
#include "doc.h"
#include "ui.h"
#include <climits>
#include <string>

static int gMatchCount = 0;
static int gMatchActive = -1;
static struct { int page, start, count; } gMatches[kMatchCap];

int ui_matchCount() { return gMatchCount; }
int ui_matchActive() { return gMatchActive; }
void ui_matchAt(int i, int* page, int* start, int* count) {
    if (i < 0 || i >= gMatchCount) { *page = -1; *start = *count = 0; return; }
    *page = gMatches[i].page; *start = gMatches[i].start; *count = gMatches[i].count;
}
void ui_matchReset() { gMatchCount = 0; gMatchActive = -1; }

// scan the whole document; case-insensitive (flags = 0)
void runSearch() {
    gMatchCount = 0;
    gMatchActive = -1;
    if (!gQuery.empty() && gDoc) {
        std::vector<unsigned short> uq = wideToU16(gQuery);
        for (int i = 0; i < gDocPages && gMatchCount < kMatchCap; i++) {
            FPDF_TEXTPAGE tp = textPageOf(i);
            if (!tp) continue;
            FPDF_SCHHANDLE sh = FPDFText_FindStart(tp, (FPDF_WIDESTRING)uq.data(), 0, 0);
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
    renderPage();
}

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
    *ry0 = lo; *ry1 = hi;
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
    else if (y1 > gScrollY + docViewH()) gScrollY = y1 - docViewH() + 8;
    clampScroll();
}

void nextMatch(int dir) {
    if (gQuery.empty() || !gMatchCount) return;
    int i = gMatchActive + dir;
    if (i < 0) i = gMatchCount - 1;
    if (i >= gMatchCount) i = 0;
    gMatchActive = i;
    if (gMatches[i].page != gPageIndex) loadPage(gMatches[i].page);
    scrollMatchIntoView();
    renderPage();
}

// search hits need the same page remapping the surgery code applies
void searchRemapPages(const std::vector<int>& back, int oldCount) {
    bool hitKept = false;
    int w = 0;
    for (int m = 0; m < gMatchCount; m++) {
        int np = (gMatches[m].page >= 0 && gMatches[m].page < oldCount) ? back[gMatches[m].page] : -1;
        if (np < 0) continue;
        gMatches[w] = gMatches[m];
        gMatches[w].page = np;
        if (m == gMatchActive) hitKept = true;
        w++;
    }
    gMatchCount = w;
    if (!hitKept) gMatchActive = gMatchCount ? 0 : -1;
}

int searchCount() { return gMatchCount; }
