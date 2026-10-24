// mnpdf for Linux - rendering. Mirrors the Windows renderPage pipeline: a
// screen-sized BGRA buffer holds the composited frame (pages -> overlays ->
// night inversion), and GTK paints that buffer on the draw signal. Overlays
// (highlights, pins, matches, selection) are drawn with cairo over the buffer,
// which gives antialiased pins and easy alpha the way GDI+ did.
//
// RAM discipline is the same as the Windows build: the only allocation that
// scales with anything is one client-sized buffer, and the print path allocates
// exactly one page raster at a time and frees it.
#include "doc.h"
#include "ui.h"

#include <cairo.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// supplied by search.cpp / main.cpp
extern int ui_matchCount();
extern int ui_matchActive();
extern void ui_matchAt(int i, int* page, int* start, int* count);

// ---- the back buffer ----
static unsigned char* gBits = nullptr;
static int gBufW = 0, gBufH = 0;
static FPDF_BITMAP gPdfBitmap = nullptr;

// The frame is a DEVICE-pixel buffer: the client area is reported in logical
// pixels by GTK, and every geometry function works in device pixels, so the
// buffer is the client size times the device scale. Without this a fractional
// desktop scale paints a 1x frame that the compositor upscales - the "low
// resolution" report.
static void ensureBuffer() {
    int w = gClientW, h = gClientH;
    if (w <= 0 || h <= 0) return;
    if (gPdfBitmap && gBufW == w && gBufH == h) return;
    if (gPdfBitmap) { FPDFBitmap_Destroy(gPdfBitmap); gPdfBitmap = nullptr; }
    free(gBits);
    gBits = (unsigned char*)calloc((size_t)w * h, 4);
    if (!gBits) { gBufW = gBufH = 0; return; }
    gBufW = w; gBufH = h;
    gPdfBitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, gBits, w * 4);
}

static void fillRectsAlpha(int page, FPDF_TEXTPAGE tp, int start, int count,
                           double r, double g, double b, double alpha, cairo_t* cr) {
    int nr = FPDFText_CountRects(tp, start, count);
    int pl = pageVx(page), pt2 = pageVy(page);
    for (int i = 0; i < nr; i++) {
        double l, t, rr, bo;
        if (!FPDFText_GetRect(tp, i, &l, &t, &rr, &bo)) continue;
        double ax, ay, bx, by;
        pagePtToPx(page, l, t, &ax, &ay);
        pagePtToPx(page, rr, bo, &bx, &by);
        double x = pl + (ax < bx ? ax : bx);
        double y = pt2 + (ay < by ? ay : by);
        double w = fabs(ax - bx);
        double h = fabs(ay - by);
        cairo_set_source_rgba(cr, r, g, b, alpha);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    }
}

static void drawHighlights(cairo_t* cr) {
    for (const Hl& h : gHls) {
        if (!pageInView(h.page)) continue;
        FPDF_TEXTPAGE tp = textPageOf(h.page);
        if (!tp) continue;
        COLORREF hc;
        if (!palColor(h.color, hc)) hc = gPalPreset[0];
        fillRectsAlpha(h.page, tp, h.start, h.count,
                       GetRValue(hc) / 255.0, GetGValue(hc) / 255.0, GetBValue(hc) / 255.0, 0.35, cr);
    }
}

static void drawMatches(cairo_t* cr) {
    int mc = ui_matchCount();
    if (!mc) return;
    int active = ui_matchActive();
    for (int m = 0; m < mc; m++) {
        int page, s, c; ui_matchAt(m, &page, &s, &c);
        if (page != gPageIndex) continue;
        FPDF_TEXTPAGE tp = textPageOf(page);
        if (!tp) continue;
        bool act = m == active;
        fillRectsAlpha(page, tp, s, c, 1.0, act ? 150 / 255.0 : 208 / 255.0, 0.0, act ? 0.55 : 0.4, cr);
    }
}

static void drawSelection(cairo_t* cr) {
    if (!hasSelection()) return;
    int p0, i0, p1, i1;
    selRange(&p0, &i0, &p1, &i1);
    for (int p = p0; p <= p1; p++) {
        FPDF_TEXTPAGE tp; int s, e;
        if (!pageSlice(p, p0, i0, p1, i1, &tp, &s, &e)) continue;
        fillRectsAlpha(p, tp, s, e - s + 1, 0.0, 120 / 255.0, 215 / 255.0, 0.35, cr);
    }
}

static void drawPins(cairo_t* cr) {
    for (const Pin& pn : gPins) {
        if (!pageInView(pn.page)) continue;
        double ax, ay;
        pagePtToPx(pn.page, pn.x, pn.y, &ax, &ay);
        int x = pageVx(pn.page) + (int)ax;
        int y = pageVy(pn.page) + (int)ay;
        int r = pinRadius();
        COLORREF col;
        if (!palColor(pn.color, col)) col = gPalPreset[kPalPreset - 1];
        cairo_set_source_rgba(cr, GetRValue(col) / 255.0, GetGValue(col) / 255.0, GetBValue(col) / 255.0, 1.0);
        cairo_arc(cr, x, y, r, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

static void drawEmptyHint(cairo_t* cr) {
    if (gDoc) return;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 15.0 * ui_scale());   // device pixels
    cairo_set_source_rgb(cr, 0.72, 0.72, 0.75);
    const char* t = "Open a PDF  (Ctrl+O)";
    cairo_text_extents_t ext;
    cairo_text_extents(cr, t, &ext);
    cairo_move_to(cr, (gClientW - ext.width) / 2, gClientH / 2);
    cairo_show_text(cr, t);
}

void renderPage() {
    ui_set_title("");                 // the platform recomputes the title and tab labels
    ensureBuffer();
    if (!gPdfBitmap || !gBits) return;

    FPDFBitmap_FillRect(gPdfBitmap, 0, 0, gClientW, gClientH, 0xFF202020);

    if (gDoc) {
        int top = gScrollY, bottom = gScrollY + gClientH;
        for (int i = 0; i < gDocPages; i++) {
            int y0 = yTopPx(i);
            if (y0 >= bottom) break;
            int ph = pageHpx(i);
            if (y0 + ph <= top) continue;
            FPDF_PAGE p = acquirePage(i);
            if (!p) continue;
            int pw = pageWpx(i), vx = pageVx(i), vy = pageVy(i);
            int px0, py0, px1, py1;
            // pdfium paints no page background, so a bare page would read as
            // faint text on the letterbox. Paint the paper first.
            if (clipPageRect(i, &px0, &py0, &px1, &py1))
                FPDFBitmap_FillRect(gPdfBitmap, px0, py0, px1 - px0, py1 - py0, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(gPdfBitmap, p, vx, vy, pw, ph, gRot[i], FPDF_ANNOT | FPDF_LCD_TEXT);
        }
    }

    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        gBits, CAIRO_FORMAT_ARGB32, gBufW, gBufH, gBufW * 4);
    cairo_t* cr = cairo_create(surf);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        drawHighlights(cr);
        drawPins(cr);
        drawMatches(cr);
        drawSelection(cr);
        drawEmptyHint(cr);
    }

    // night mode: invert the paper, not the surround, after the marks so they
    // invert with it. The buffer is flushed to a fresh cairo surface on paint,
    // so the inversion has to happen before the paint - the cairo path above
    // has already been destroyed by then.
    if (gNight && gDoc) {
        int top = gScrollY, bottom = gScrollY + gClientH;
        for (int i = 0; i < gDocPages; i++) {
            int ty = yTopPx(i);
            if (ty >= bottom) break;
            int th = pageHpx(i);
            if (ty + th <= top) continue;
            int x0, ny0, x1, ny1;
            clipPageRect(i, &x0, &ny0, &x1, &ny1);
            for (int y = ny0; y < ny1; y++) {
                unsigned char* row = gBits + (size_t)y * gBufW * 4;
                for (int x = x0; x < x1; x++) {
                    row[x * 4 + 0] = 255 - row[x * 4 + 0];
                    row[x * 4 + 1] = 255 - row[x * 4 + 1];
                    row[x * 4 + 2] = 255 - row[x * 4 + 2];
                }
            }
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    ui_invalidate();
}

// GTK's draw signal: blit the current back buffer.
// The buffer holds DEVICE pixels; the draw context works in the widget's own
// coordinates. Setting the surface's device scale makes cairo map device pixels
// to logical ones, so the compositor receives a native-resolution frame instead
// of upscaling a 1x one (the "low resolution" report at a fractional scale).
void ui_paint(cairo_t* cr) {
    if (!gBits || gBufW <= 0) return;
    cairo_surface_t* src = cairo_image_surface_create_for_data(
        gBits, CAIRO_FORMAT_ARGB32, gBufW, gBufH, gBufW * 4);
    double s = ui_scale() > 0 ? ui_scale() : 1.0;
    if (s != 1.0) cairo_surface_set_device_scale(src, s, s);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(src);
}

// accessors for the test seam (main.cpp's MNPDF_SHOT hook)
const unsigned char* ui_backbuf_bits() { return gBits; }
int ui_backbuf_w() { return gBufW; }
int ui_backbuf_h() { return gBufH; }

// print path: one page raster, no marks and no annotations (a print is a view
// operation, exactly as on Windows). The caller sizes the raster, so the budget
// it caps is applied where the size is chosen (see main.cpp's printDrawPage).
bool renderPageToBuffer(int page, int w, int h, std::vector<unsigned char>& out, int* strideOut) {
    if (!gDoc || page < 0 || page >= gDocPages) return false;
    if (w <= 0 || h <= 0) return false;
    FPDF_PAGE p = acquirePage(page);
    if (!p) return false;
    out.assign((size_t)w * h * 4, 0);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, out.data(), w * 4);
    if (!bmp) { out.clear(); return false; }
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, p, 0, 0, w, h, gRot[page], 0);   // flags = 0: clean pages
    FPDFBitmap_Destroy(bmp);
    if (strideOut) *strideOut = w * 4;
    return true;
}
