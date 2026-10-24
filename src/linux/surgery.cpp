// mnpdf for Linux - document surgery: adopt, reorder/delete/move pages, merge,
// split. Ported from the Windows src/main.cpp; the FPDF_* calls are identical.
#include "doc.h"
#include "ui.h"
#include <cstdio>
#include <vector>

extern void writeAppPref();
extern void writeSidecarNow();
extern void searchRemapPages(const std::vector<int>& back, int oldCount);

struct SvcSaveFW : FPDF_FILEWRITE { std::vector<unsigned char>* out; };
static int SvcWriteBlock(FPDF_FILEWRITE* p, const void* data, unsigned long len) {
    auto* me = (SvcSaveFW*)p;
    const unsigned char* d = (const unsigned char*)data;
    me->out->insert(me->out->end(), d, d + len);
    return 1;
}

void refreshDrawers();   // the drawers follow the page list (main.cpp)

static void adoptDoc(FPDF_DOCUMENT nd, int oldCount) {
    flushPageCache();
    if (gDoc) FPDF_CopyViewerPreferences(nd, gDoc);
    if (gDoc) FPDF_CloseDocument(gDoc);
    gDoc = nd;
    gDocPages = FPDF_GetPageCount(gDoc);
    gPageCount = gDocPages;
    int* oldRot = new int[oldCount ? oldCount : 1];
    for (int i = 0; i < oldCount; i++) oldRot[i] = (i < gDocPages) ? gRot[i] : 0;
    allocPageTables(gDocPages);
    for (int i = 0; i < gDocPages; i++) {
        FPDF_PAGE p = FPDF_LoadPage(gDoc, i);
        double w = p ? FPDF_GetPageWidthF(p) : 612;
        double h = p ? FPDF_GetPageHeightF(p) : 792;
        int fileRot = p ? (FPDFPage_GetRotation(p) & 3) : 0;
        if (fileRot & 1) { double t2 = w; w = h; h = t2; }
        gRot[i] = (i < oldCount) ? oldRot[i] : fileRot;
        if (p) FPDF_ClosePage(p);
        gPageW[i] = w;
        gPageH[i] = h;
    }
    delete[] oldRot;
    flushPageCache();
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

static void replaceDocByOrder(const std::vector<int>& order) {
    int n = (int)order.size();
    if (n <= 0) return;
    std::vector<int> back(gDocPages, -1);
    for (int j = 0; j < n; j++) back[order[j]] = j;
    std::vector<int> rot(gDocPages, 0);
    for (int j = 0; j < n; j++) rot[j] = gRot[order[j]];
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    if (!FPDF_ImportPagesByIndex(nd, gDoc, order.data(), (unsigned long)n, 0)) {
        FPDF_CloseDocument(nd);
        ui_message("Those pages could not be reordered.", "mnpdf", false);
        return;
    }
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
    if (gSelAnchorPage >= 0 || gSelHeadPage >= 0) {
        int na = (gSelAnchorPage >= 0 && gSelAnchorPage < gDocPages) ? back[gSelAnchorPage] : -1;
        int nh = (gSelHeadPage >= 0 && gSelHeadPage < gDocPages) ? back[gSelHeadPage] : -1;
        if (na < 0 || nh < 0) clearSelection();
        else { gSelAnchorPage = na; gSelHeadPage = nh; }
    }
    searchRemapPages(back, gDocPages);
    delete[] gRot;
    gRot = new int[gDocPages ? gDocPages : 1];
    for (int j = 0; j < n; j++) gRot[j] = rot[j];
    for (int j = n; j < gDocPages; j++) gRot[j] = 0;
    adoptDoc(nd, gDocPages);
}

void deletePage(int idx) {
    if (idx < 0 || idx >= gDocPages || gDocPages <= 1) return;
    std::vector<int> order;
    for (int i = 0; i < gDocPages; i++) if (i != idx) order.push_back(i);
    replaceDocByOrder(order);
}

void movePage(int from, int to) {
    if (from < 0 || from >= gDocPages || to < 0 || to >= gDocPages || from == to) return;
    std::vector<int> order;
    for (int i = 0; i < gDocPages; i++) order.push_back(i);
    int v = order[from];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, v);
    replaceDocByOrder(order);
    // The drawer's selection follows the page that moved. It cannot stay on the
    // index the page moved AWAY from: that index now holds a different page, so
    // the ring would name a page the reader did not pick and the next drawer
    // command would act on that one. The Windows build sets gThumbSel = to here
    // for the same reason (src/main.cpp:4615-4620, "a second Ctrl+Up would move
    // that one instead"). This is the one shared boundary every move passes
    // through - the drawer's Ctrl+Up/Ctrl+Down in onThumbKey and the context
    // menu's Move page up/down (cases 311/312) - so all of them follow at once.
    gThumbSel = to;
}

void mergeFiles(const std::vector<std::wstring>& files) {
    if (!gDoc || files.empty()) return;
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    int at = 0;
    if (!FPDF_ImportPages(nd, gDoc, nullptr, at)) {
        FPDF_CloseDocument(nd);
        ui_message("The current document could not be merged.", "mnpdf", false);
        return;
    }
    at = FPDF_GetPageCount(nd);
    for (const std::wstring& f : files) {
        std::string u8 = wideToUtf8(f);
        FPDF_DOCUMENT src = FPDF_LoadDocument(u8.c_str(), nullptr);
        if (!src) continue;
        int n = FPDF_GetPageCount(src);
        if (n > 0) FPDF_ImportPages(nd, src, nullptr, at);
        at = FPDF_GetPageCount(nd);
        FPDF_CloseDocument(src);
    }
    if (FPDF_GetPageCount(nd) <= gDocPages) {
        FPDF_CloseDocument(nd);
        ui_message("No pages could be added.", "mnpdf", false);
        return;
    }
    adoptDoc(nd, gDocPages);
    writeAppPref();
}

void splitExport(const std::wstring& target, const std::string& range, bool quiet) {
    if (!gDoc) return;
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!nd) return;
    if (!FPDF_ImportPages(nd, gDoc, range.c_str(), 0)) {
        FPDF_CloseDocument(nd);
        if (!quiet) ui_message("That page range could not be read. Try 2-5 or 1,3,8.", "Split pages", false);
        return;
    }
    std::vector<unsigned char> bytes;
    SvcSaveFW fw = {};
    fw.version = 1;
    fw.WriteBlock = SvcWriteBlock;
    fw.out = &bytes;
    bool wrote = FPDF_SaveAsCopy(nd, &fw, 0) && !bytes.empty();
    if (wrote) {
        FILE* f = fopen(wideToUtf8(target).c_str(), "wb");
        if (!f) wrote = false;
        else { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
    }
    FPDF_CloseDocument(nd);
    if (quiet) return;
    if (wrote) ui_message("Pages exported.", "Split pages", false);
    else ui_message("These pages could not be written to that file.", "Split pages", false);
}
