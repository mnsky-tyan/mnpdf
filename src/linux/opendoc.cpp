// mnpdf for Linux - document open and save, plus the JPEG signature placement.
// Ported from the Windows src/main.cpp; the FPDF_* calls are identical, only the
// file I/O and path encoding differ (Linux wants UTF-8 bytes).
#include "doc.h"
#include "ui.h"
#include <cstdio>
#include <vector>

// from sidecar.cpp: write one bundle's own sidecar (used by openPath, so unsaved
// marks reach their sidecar before the bundle drops them)
void writeSidecarForBundle(Doc* d);

std::wstring gSigPlacing;   // a JPEG chosen, waiting for a click

struct SaveFW : FPDF_FILEWRITE { std::vector<unsigned char>* out; };

static int WriteBlockCb(FPDF_FILEWRITE* pThis, const void* data, unsigned long len) {
    auto* me = (SaveFW*)pThis;
    const unsigned char* d = (const unsigned char*)data;
    me->out->insert(me->out->end(), d, d + len);
    return 1;
}

static SaveFW makeSaveFW(std::vector<unsigned char>& out) {
    SaveFW fw = {};
    fw.version = 1;
    fw.WriteBlock = WriteBlockCb;
    fw.out = &out;
    return fw;
}

static void annotColor(FPDF_ANNOTATION a, unsigned& r, unsigned& g, unsigned& b) {
    unsigned cr = 0, cg = 0, cb = 0, ca = 255;
    if (FPDFAnnot_GetColor(a, FPDFANNOT_COLORTYPE_Color, &cr, &cg, &cb, &ca)) { r = cr; g = cg; b = cb; }
}

// read an annotation's Contents string (UTF-16) as a std::wstring
static std::wstring annotContents(FPDF_ANNOTATION a) {
    unsigned long need = FPDFAnnot_GetStringValue(a, "Contents", nullptr, 0);
    if (need <= 2) return L"";                    // need is bytes; 2 = the terminator
    size_t n16 = need / 2;
    std::vector<unsigned short> buf(n16 + 1, 0);
    if (FPDFAnnot_GetStringValue(a, "Contents", (FPDF_WCHAR*)buf.data(), need) <= 0) return L"";
    std::wstring txt = u16ToWide(buf.data(), n16);
    while (!txt.empty() && txt.back() == L'\0') txt.pop_back();
    return txt;
}

static std::wstring annotSource(FPDF_ANNOTATION a) {
    unsigned short src[64] = {0};
    FPDFAnnot_GetStringValue(a, "Source", (FPDF_WCHAR*)src, sizeof(src));
    return u16ToWide(src, 64);
}

void writeSidecarNow();   // from sidecar.cpp

// ---- open ----
static void clearDocState() {
    gHls.clear(); gPins.clear(); gSigs.clear(); gTomb.clear();
    gUndo.clear(); gRedo.clear();
    gDirty = false;
    gSigPlacing.clear();                        // a half-finished placement dies here
}

bool openPath(const std::wstring& path) {
    if (gDirty) writeSidecarForBundle(gViewTab);   // unsaved marks reach their sidecar before this bundle drops them
    std::string u8 = wideToUtf8(path);
    FPDF_DOCUMENT nd = FPDF_LoadDocument(u8.c_str(), nullptr);
    if (!nd) { ui_message("Could not open PDF", "mnpdf", false); return false; }
    flushPageCache();
    if (gDoc) FPDF_CloseDocument(gDoc);
    gDoc = nd;
    gPath = path;
    clearDocState();
    gPageCount = FPDF_GetPageCount(gDoc);
    allocPageTables(gPageCount);
    for (int i = 0; i < gDocPages; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        double w = p ? FPDF_GetPageWidthF(p) : 612;
        double h = p ? FPDF_GetPageHeightF(p) : 792;
        gRot[i] = p ? FPDFPage_GetRotation(p) & 3 : 0;
        int na = p ? FPDFPage_GetAnnotCount(p) : 0;
        FPDF_TEXTPAGE tp = nullptr;
        if (p && na) tp = FPDFText_LoadPage(p);
        for (int k = na - 1; k >= 0; k--) {
            FPDF_ANNOTATION a = p ? FPDFPage_GetAnnot(p, k) : nullptr;
            if (!a) continue;
            int ty = FPDFAnnot_GetSubtype(a);
            if (ty != FPDF_ANNOT_HIGHLIGHT && ty != FPDF_ANNOT_TEXT) { FPDFPage_CloseAnnot(a); continue; }
            std::wstring src = annotSource(a);
            bool ours = src.size() > 2 && src.compare(0, 5, L"mnpdf") == 0;
            if (!ours) { FPDFPage_CloseAnnot(a); continue; }
            bool lifted = false;
            if (ty == FPDF_ANNOT_HIGHLIGHT && tp) {
                unsigned R = 255, G = 230, B = 128;
                annotColor(a, R, G, B);
                int color = nearestPalColor(R, G, B);
                int qs = -1, qc = 0;
                if (swscanf(src.c_str(), L"mnpdf %d %d", &qs, &qc) == 2 && qs >= 0 && qc > 0) {
                    bool have = false;
                    for (const Hl& h : gHls)
                        if (h.baked && h.page == i && h.start == qs && h.count == qc) { have = true; break; }
                    if (!have) gHls.push_back({ i, qs, qc, color, true });
                    lifted = true;
                }
            } else if (ty == FPDF_ANNOT_TEXT && p) {
                FS_RECTF rc;
                unsigned PR = 230, PG = 71, PB = 77;
                annotColor(a, PR, PG, PB);
                if (FPDFAnnot_GetRect(a, &rc)) {
                    std::wstring txt = annotContents(a);
                    if (!txt.empty()) {
                        int pcol = nearestPalColor(PR, PG, PB);
                        gPins.push_back({ i, ((double)rc.left + rc.right) / 2, ((double)rc.top + rc.bottom) / 2, txt, pcol, true });
                        lifted = true;
                    }
                }
            }
            FPDFPage_CloseAnnot(a);
            if (lifted) FPDFPage_RemoveAnnot(p, k);
        }
        if (tp) FPDFText_ClosePage(tp);
        if (p) FPDF_ClosePage(p);
        if (gRot[i] & 1) { double t2 = w; w = h; h = t2; }
        gPageW[i] = w;
        gPageH[i] = h;
    }
    normalizeHls();
    normalizePins();
    relayoutPages();
    gPageWpt = 612; gPageHpt = 792;
    gScrollX = gScrollY = 0;
    gRestoredPage = false;
    gFitWidth = true;
    applyFitWidth();
    loadPage(0);
    std::vector<Hl> hlFile = gHls;
    std::vector<Pin> pinFile = gPins;
    restoreSidecar(path);
    replaySigs();
    if (!gTomb.empty()) {
        markDirty();
    } else {
        for (const Hl& h : gHls) {
            bool found = false;
            for (const Hl& f : hlFile)
                if (h.page == f.page && h.start == f.start && h.count == f.count && h.color == f.color) found = true;
            if (!found) markDirty();
        }
        for (const Pin& p : gPins) {
            bool found = false;
            for (const Pin& f : pinFile)
                if (f.page == p.page && fabs(f.x - p.x) < kSpotTolPt && fabs(f.y - p.y) < kSpotTolPt
                    && f.text == p.text && f.color == p.color) found = true;
            if (!found) markDirty();
        }
    }
    clampScroll();
    renderPage();
    writeSidecarNow();
    return true;
}

// ---- save ----
bool bakeAndWrite(const std::wstring& target) {
    if (!gDoc || !gDocPages) return false;
    flushPageCache();
    bool ok = true;
    for (int i = 0; i < gDocPages && ok; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        if (!p) { ok = false; break; }
        for (int k = FPDFPage_GetAnnotCount(p) - 1; k >= 0; k--) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, k);
            if (!a) continue;
            int ty = FPDFAnnot_GetSubtype(a);
            std::wstring src = annotSource(a);
            bool ours = src.size() > 2 && src.compare(0, 5, L"mnpdf") == 0;
            bool replaced = false;
            if (ours && ty == FPDF_ANNOT_HIGHLIGHT) {
                int qs = -1, qc = 0;
                if (swscanf(src.c_str(), L"mnpdf %d %d", &qs, &qc) == 2 && qs >= 0 && qc > 0)
                    for (const Hl& hl : gHls)
                        if (hl.page == i && hl.start == qs && hl.count == qc) { replaced = true; break; }
            } else if (ours && ty == FPDF_ANNOT_TEXT) {
                FS_RECTF arc;
                if (FPDFAnnot_GetRect(a, &arc))
                    for (const Pin& pn : gPins)
                        if (pn.page == i && !pn.text.empty()
                            && fabs(((double)arc.left + arc.right) / 2 - pn.x) < kSpotTolBakePt
                            && fabs(((double)arc.top + arc.bottom) / 2 - pn.y) < kSpotTolBakePt) { replaced = true; break; }
            }
            FPDFPage_CloseAnnot(a);
            if (replaced) FPDFPage_RemoveAnnot(p, k);
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
                    FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, GetRValue(bc), GetGValue(bc), GetBValue(bc), 255);
                    std::vector<unsigned short> src = wideToU16(L"mnpdf " + std::to_wstring(hl.start) + L" " + std::to_wstring(hl.count));
                    FPDFAnnot_SetStringValue(a, "Source", (FPDF_WIDESTRING)src.data());
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
                std::vector<unsigned short> contents = wideToU16(pn.text);
                FPDFAnnot_SetStringValue(a, "Contents", (FPDF_WIDESTRING)contents.data());
                COLORREF pcol2;
                if (!palColor(pn.color, pcol2)) pcol2 = gPalPreset[kPalPreset - 1];
                FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, GetRValue(pcol2), GetGValue(pcol2), GetBValue(pcol2), 255);
                std::vector<unsigned short> src = wideToU16(L"mnpdf");
                FPDFAnnot_SetStringValue(a, "Source", (FPDF_WIDESTRING)src.data());
                FPDFPage_CloseAnnot(a);
            }
        }
        if (tp) FPDFText_ClosePage(tp);
        FPDF_ClosePage(p);
    }
    if (!ok) return false;
    std::vector<unsigned char> out;
    SaveFW fw = makeSaveFW(out);
    if (!FPDF_SaveAsCopy(gDoc, &fw, 0) || out.empty()) return false;
    FILE* fp = fopen(wideToUtf8(target).c_str(), "wb");
    if (!fp) return false;
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return true;
}

bool doSaveImpl(const std::wstring& target) {
    if (!gDoc) return false;
    if (!gDirty) return true;
    if (bakeAndWrite(target)) {
        gHls.clear(); gPins.clear(); gTomb.clear(); gSigs.clear();
        gDirty = false;
        writeSidecarNow();
        openPath(target);
        return true;
    }
    ui_message("Save failed", "mnpdf", false);
    return false;
}

bool doSave() {
    if (!gDoc) return false;
    if (!gDirty) return true;
    if (gPath.empty()) return doSaveAs();
    return doSaveImpl(gPath);
}

bool doSaveAs() {
    if (!gDoc) return false;
    std::wstring def = gPath.empty() ? L"" : gPath;
    std::string chosen = ui_save_file(wideToUtf8(def));
    if (chosen.empty()) return false;
    gPath = utf8ToWide(chosen);
    return doSaveImpl(gPath);
}

// ---- signature stamps ----
struct JpegBlob { std::vector<unsigned char> bytes; };

static int jpegGetBlock(void* param, unsigned long position, unsigned char* buf, unsigned long size) {
    auto* me = (JpegBlob*)param;
    if (position >= me->bytes.size()) return 0;
    unsigned long avail = (unsigned long)me->bytes.size() - position;
    unsigned long n = size < avail ? size : avail;
    memcpy(buf, me->bytes.data() + position, n);
    return (int)n;
}

static bool readWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    FILE* f = fopen(wideToUtf8(path).c_str(), "rb");
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

bool looksLikeJpeg(const std::wstring& path) {
    std::vector<unsigned char> b;
    if (!readWholeFile(path, b)) return false;
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return false;
    bool sawFrame = false;                 // a SOF: nothing else says what the image is
    int frameComps = 0;
    unsigned char compIds[4] = { 0, 0, 0, 0 };
    bool sawScan = false;
    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF) return false;
        // any number of 0xFF fill bytes may precede a marker (ITU T.81 B.1.1.2)
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
        size_t seg = i;
        // SOF0-SOF15 less DHT/JPG/DAC: the frame header, which is what makes the
        // stream a picture rather than a shape. A frame-less chain (SOI, a scan,
        // entropy, EOI) reaches the loader, which reports success on it and dies
        // at the first paint - and replaySigs re-places every stamp on every open,
        // so it would crash again on every launch. The frame has to be here, and
        // it has to describe something a decoder can hold.
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (len < 8) return false;
            int nf = b[seg + 7];
            if (nf < 1 || nf > 4 || len < (size_t)8 + 3 * (size_t)nf) return false;
            unsigned h = ((unsigned)b[seg + 3] << 8) | b[seg + 4];
            unsigned w = ((unsigned)b[seg + 5] << 8) | b[seg + 6];
            if (w == 0 || h == 0 || (unsigned long long)w * h > 100000000ULL) return false;
            sawFrame = true;
            frameComps = nf;
            for (int c = 0; c < nf; c++) compIds[c] = b[seg + 8 + 3 * c];
        }
        i += len;
        if (m != 0xDA) continue;
        // SOS: the scan names the components it reads, and every name has to be
        // one the frame declared - a scan describing components the frame never
        // declared decodes into nothing and dies the same way. A progressive
        // picture scans a subset per SOS, so the counts need not be equal, only
        // in range and correctly named.
        if (!sawFrame || len < 6) return false;
        int ns = b[seg + 2];
        if (ns < 1 || ns > frameComps || len < (size_t)6 + 2 * (size_t)ns) return false;
        for (int c = 0; c < ns; c++) {
            unsigned char id = b[seg + 3 + 2 * c];
            bool known = false;
            for (int f = 0; f < frameComps; f++) if (compIds[f] == id) known = true;
            if (!known) return false;
        }
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

static bool placeSigObj(const Sig& s) {
    if (!gDoc || s.page < 0 || s.page >= gDocPages) return false;
    if (!looksLikeJpeg(s.path)) return false;
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
        FPDFPageObj_Destroy(obj);
        return false;
    }
    FPDFImageObj_SetMatrix(obj, s.w, 0, 0, s.h, s.cx - s.w / 2, s.cy - s.h / 2);
    if (!FPDFPage_InsertObject(pg, obj)) return false;
    FPDFPage_GenerateContent(pg);
    return true;
}

void replaySigs() {
    bool any = false;
    for (size_t i = 0; i < gSigs.size(); ) {
        if (placeSigObj(gSigs[i])) { i++; any = true; }
        else gSigs.erase(gSigs.begin() + i);
    }
    if (any) markDirty();
}

void commitSigPlacement(int page, double sx, double sy) {
    double x, y;
    pagePxToPt(page, sx, sy, &x, &y);
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
        ui_message("That image could not be placed.", "mnpdf", false);
        return;
    }
    gSigs.push_back(s);
    gSigPlacing.clear();
    markDirty();
    writeSidecarNow();
    renderPage();
}

void clearSignatures() {
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
                if (fabs((double)bl - (s.cx - s.w / 2)) < kStampBoundsTol
                    && fabs((double)bb - (s.cy - s.h / 2)) < kStampBoundsTol
                    && fabs((double)br - (s.cx + s.w / 2)) < kStampBoundsTol
                    && fabs((double)bt - (s.cy + s.h / 2)) < kStampBoundsTol) {
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

void pickSignatureImage() {
    std::string chosen = ui_pick_jpeg();
    if (chosen.empty()) return;
    if (!looksLikeJpeg(utf8ToWide(chosen))) {
        ui_message("A signature has to be a JPEG image. That file is not one.", "Place signature", false);
        return;
    }
    gSigPlacing = utf8ToWide(chosen);
    ui_message("Click the page where the signature should sit. Esc cancels.", "Place signature", false);
}
