// Toolchain probe: open a PDF with PDFium, rasterize page 1 to a PPM file.
// Not shipped - it exists to prove third_party/pdfium-linux works before the
// real app is built on it (build-linux.sh runs it first).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "fpdfview.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "tests/arc.pdf";
    const char* out = argc > 2 ? argv[2] : "/tmp/mnpdf-probe.ppm";
    FPDF_InitLibrary();
    FPDF_DOCUMENT doc = FPDF_LoadDocument(path, nullptr);
    if (!doc) { fprintf(stderr, "probe: cannot open %s\n", path); return 1; }
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    if (!page) { fprintf(stderr, "probe: cannot load page\n"); return 2; }
    double w = FPDF_GetPageWidth(page), h = FPDF_GetPageHeight(page);
    int scale = 2;
    int pw = (int)(w * scale), ph = (int)(h * scale);
    FPDF_BITMAP bmp = FPDFBitmap_Create(pw, ph, 0);
    FPDFBitmap_FillRect(bmp, 0, 0, pw, ph, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, pw, ph, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
    unsigned char* buf = (unsigned char*)FPDFBitmap_GetBuffer(bmp);
    int stride = FPDFBitmap_GetStride(bmp);
    FILE* fp = fopen(out, "wb");
    if (!fp) { fprintf(stderr, "probe: cannot write %s\n", out); return 3; }
    fprintf(fp, "P6\n%d %d\n255\n", pw, ph);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            unsigned char* p = buf + y * stride + x * 4;
            fputc(p[2], fp); fputc(p[1], fp); fputc(p[0], fp);  // BGRA -> RGB
        }
    fclose(fp);
    printf("probe: OK %.0fx%.0f pt -> %dpx wide -> %s\n", w, h, pw, out);
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return 0;
}
