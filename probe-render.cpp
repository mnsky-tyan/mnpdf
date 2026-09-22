// probe-render: diagnostic only. Renders page 0 of a PDF three ways and
// prints pixel samples so we can see who owns the page background:
//   A) render into a black-filled bitmap (pdfium default state)
//   B) render into a dark-letterbox-filled bitmap (mnpdf's fill)
//   C) render into a white-filled bitmap
// Build: cl /nologo /O2 /MT /EHsc /DUNICODE /utf-8 probe-render.cpp /I third_party\pdfium\include /link third_party\pdfium\lib\pdfium.dll.lib /OUT:build\probe-render.exe
#include <windows.h>
#include "fpdfview.h"
#include <stdio.h>
#include <stdint.h>

static void samples(const char* tag, FPDF_BITMAP bmp, int W, int H) {
    printf("%s:\n", tag);
    uint8_t* buf = (uint8_t*)FPDFBitmap_GetBuffer(bmp);
    int stride = (int)FPDFBitmap_GetStride(bmp);
    int pts[][2] = { { W / 2, H / 2 }, { 8, 8 }, { W - 9, 8 }, { W / 2, 30 }, { 40, H / 2 } };
    for (int i = 0; i < 5; i++) {
        uint8_t* p = buf + (size_t)pts[i][1] * stride + pts[i][0] * 4; // BGRA
        printf("  (%4d,%4d) = %02X%02X%02X%02X (BGRA)\n", pts[i][0], pts[i][1], p[0], p[1], p[2], p[3]);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: probe-render <pdf>\n"); return 1; }
    FPDF_LIBRARY_CONFIG cfg = { 2, nullptr, nullptr };
    FPDF_InitLibraryWithConfig(&cfg);
    FPDF_DOCUMENT doc = FPDF_LoadDocument(argv[1], nullptr);
    if (!doc) { printf("load failed\n"); return 1; }
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    double wpt = FPDF_GetPageWidthF(page), hpt = FPDF_GetPageHeightF(page);
    printf("page 0: %.1f x %.1f pt, count=%d\n", wpt, hpt, FPDF_GetPageCount(doc));
    int W = (int)wpt, H = (int)hpt;

    UINT fills[3] = { 0xFF000000, 0xFF202020, 0xFFFFFFFF };
    const char* names[3] = { "A black fill", "B dark letterbox fill (mnpdf)", "C white fill" };
    for (int i = 0; i < 3; i++) {
        FPDF_BITMAP bmp = FPDFBitmap_Create(W, H, 0);
        FPDFBitmap_FillRect(bmp, 0, 0, W, H, fills[i]);
        FPDF_RenderPageBitmap(bmp, page, 0, 0, W, H, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
        samples(names[i], bmp, W, H);
        FPDFBitmap_Destroy(bmp);
    }
    // D: the FIXED viewer behavior - dark letterbox, white paper under the
    // page rect, then render
    {
        FPDF_BITMAP bmp = FPDFBitmap_Create(W, H, 0);
        FPDFBitmap_FillRect(bmp, 0, 0, W, H, 0xFF202020);
        FPDFBitmap_FillRect(bmp, 0, 0, W, H, 0xFFFFFFFF); // page rect = whole bitmap at zoom 1
        FPDF_RenderPageBitmap(bmp, page, 0, 0, W, H, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
        samples("D letterbox + white paper + render (the fix)", bmp, W, H);
        FPDFBitmap_Destroy(bmp);
    }
    FPDF_ClosePage(page);
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return 0;
}
