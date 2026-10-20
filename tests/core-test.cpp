// Headless functional test for the mnpdf Linux core.
//
// It links the platform-independent modules (doc.cpp, search.cpp, sidecar.cpp,
// opendoc.cpp, surgery.cpp) and supplies stubs for the ui_* seam, so the whole
// document engine - open, geometry, selection, marks, undo, sidecar, save,
// page surgery - is exercised with no display and no GTK. This is the Linux
// counterpart of the Windows PowerShell suites, which drive the real binary;
// here the seam lets the same logic be tested directly.
//
// Usage: mnpdf-core-test <path-to-arc.pdf> <scratch-dir>
#include "doc.h"
#include "ui.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

// ---- ui_* stubs ----
static std::string gStubOpenPath, gStubSavePath, gStubJpegPath;
static std::vector<std::string> gMessages;
static int gCopyCount = 0;
static std::string gLastCopy;

double ui_scale() { return 1.0; }
void ui_invalidate() {}
void ui_set_title(const std::string&) {}
void ui_toast(const std::string& m) { gMessages.push_back("toast:" + m); }
int ui_message(const std::string& text, const std::string&, bool) { gMessages.push_back(text); return UI_MSGBOX_YES; }
std::string ui_open_file() { return gStubOpenPath; }
std::string ui_save_file(const std::string& s) { return gStubSavePath.empty() ? s : gStubSavePath; }
std::string ui_pick_jpeg() { return gStubJpegPath; }
bool ui_prompt(const std::string&, std::string& v) { (void)v; return false; }
bool ui_prompt_range(std::string& r) { r = "2-3"; return true; }
bool ui_color_box(int& idx) { idx = kPalPreset; return false; }
void ui_copy_text(const std::string& t) { gCopyCount++; gLastCopy = t; }
void ui_note_editor(int, int, int, const std::wstring&) {}
void refreshDrawers() {}
extern int ui_matchActive();   // from search.cpp: which match is active
void ui_windowRect(int* x, int* y, int* w, int* h, int* maxed) { *x = 0; *y = 0; *w = 800; *h = 600; *maxed = 0; }

// render.cpp needs GTK/cairo; the test supplies the two render entry points.
void renderPage() { ui_invalidate(); }
bool renderPageToBuffer(int, int, int, std::vector<unsigned char>& out, int* stride) { out.clear(); return false; }
void ui_tabStripInfo(std::vector<std::wstring>& names, std::vector<int>& dirty, int& active) {
    active = gActiveDoc;
    for (int i = 0; i < gDocCount; i++) { names.push_back(gDocs[i]->path); dirty.push_back(gDocs[i]->dirty); }
}
bool ui_hasDoc() { return gDoc != nullptr; }

// ---- test harness ----
static int gPass = 0, gFail = 0;

// count annotations in a PDF file; ours=true counts only the ones mnpdf writes
// (their /Source starts with "mnpdf"), so a fixture's own annotations never
// inflate the count
static int countAnnots(const std::string& path, bool ours) {
    FPDF_DOCUMENT d = FPDF_LoadDocument(path.c_str(), nullptr);
    if (!d) return -1;
    int total = 0;
    int np = FPDF_GetPageCount(d);
    for (int i = 0; i < np; i++) {
        FPDF_PAGE p = FPDF_LoadPage(d, i);
        if (!p) continue;
        int na = FPDFPage_GetAnnotCount(p);
        if (!ours) { total += na; FPDF_ClosePage(p); continue; }
        for (int k = 0; k < na; k++) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, k);
            if (!a) continue;
            unsigned short src[64] = {0};
            FPDFAnnot_GetStringValue(a, "Source", (FPDF_WCHAR*)src, sizeof(src));
            std::wstring w = u16ToWide(src, 64);
            if (w.compare(0, 5, L"mnpdf") == 0) total++;
            FPDFPage_CloseAnnot(a);
        }
        FPDF_ClosePage(p);
    }
    FPDF_CloseDocument(d);
    return total;
}
#define CHECK(cond, name) do { \
    if (cond) { gPass++; printf("  ok   %s\n", name); } \
    else { gFail++; printf("  FAIL %s   (line %d)\n", name, __LINE__); } \
} while (0)

static void resetGlobals() {
    gClientW = 1000; gClientH = 800;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <arc.pdf> <scratch-dir> [tests-dir]\n", argv[0]); return 2; }
    std::string pdf = argv[1], scratch = argv[2];
    // the fixtures live next to the suite, which is not derivable from a bare
    // filename argument (npos + 1 wraps to 0), so the runner names the directory
    std::string dirOf = (argc > 3) ? argv[3] : "tests/";
    if (!dirOf.empty() && dirOf.back() != '/') dirOf += '/';
    mkdir(scratch.c_str(), 0755);
    setenv("XDG_CONFIG_HOME", scratch.c_str(), 1);   // isolate all state writes

    FPDF_InitLibrary();
    resetGlobals();
    gDocs[0] = new Doc();

    std::wstring wpdf = utf8ToWide(pdf);

    printf("== open ==\n");
    CHECK(openPath(wpdf), "openPath succeeds");
    CHECK(gDoc != nullptr, "document handle live");
    int totalPages = gDocPages;
    CHECK(gDocPages >= 4, "document has its full page count");
    printf("       page count: %d\n", gDocPages);
    CHECK(gDocPages > 0 && gPageW[0] > 0, "page sizes measured");
    CHECK(gMaxPageW > 0, "max page width known");
    CHECK(gZoom > 0.25 && gZoom <= 8.0, "fit-width zoom in range");
    CHECK(gFitWidth, "opens in fit-width mode");

    printf("== layout ==\n");
    int p0top = yTopPx(0), p1top = yTopPx(1);
    CHECK(p1top > p0top, "pages stack top to bottom");
    CHECK(docHpx() > 0, "document height positive");
    CHECK(pageInView(0), "page 0 in view");
    CHECK(clipPageRect(0, nullptr, nullptr, nullptr, nullptr), "page 0 clips to client");
    // geometry must be self-consistent: docTop + yTopPx == pageVy
    CHECK(pageVy(1) == docTop() + yTopPx(1), "pageVy = docTop + yTopPx");
    CHECK(pageVx(0) == docLeft() + pageLeftPx(0), "pageVx = docLeft + pageLeftPx");

    printf("== text layer and selection ==\n");
    FPDF_TEXTPAGE tp = textPageOf(0);
    CHECK(tp != nullptr, "text page loads");
    int nchars = tp ? FPDFText_CountChars(tp) : 0;
    CHECK(nchars > 0, "page has characters");
    // hit-test the middle of the page and check we resolve a char index
    int pg = -1;
    // the fixture's page 0 carries body text across its middle, so a hit-test at
    // the viewport centre must resolve a real character index, not a sentinel
    int idx = charIndexAtDoc(gClientW / 2, tabStripH() + docViewH() / 2, &pg);
    CHECK(pg >= 0 && pg < gDocPages, "a click resolves to a page in range");
    CHECK(idx >= 0, "a hit-test over body text resolves a real character");
    // page-space round trip
    {
        double px, py, qx, qy;
        pagePtToPx(0, 100, 200, &px, &py);
        pagePxToPt(0, px, py, &qx, &qy);
        CHECK(fabs(qx - 100) < 0.5 && fabs(qy - 200) < 0.5, "pt->px->pt round trips");
    }
    // select a run of characters and copy it
    {
        gSelAnchorPage = 0; gSelHeadPage = 0;
        gSelAnchor = 0; gSelHead = nchars - 1;
        CHECK(hasSelection(), "whole-page range is a selection");
        copySelection();
        CHECK(gCopyCount == 1, "copy reaches the clipboard seam");
        CHECK(!gLastCopy.empty(), "copied text is not empty");
        // the copied text must be real page text
        std::string first;
        for (char c : gLastCopy) { if (c == '\r' || c == '\n') break; first += c; }
        CHECK(!first.empty(), "first line of copied text is non-empty");
        printf("       copied: %.60s\n", first.c_str());
    }
    {
        clearSelection();
        CHECK(!hasSelection(), "clearSelection empties it");
        // word select at a character in the middle of the page text
        int mid = nchars / 2;
        selectWord(0, mid);
        CHECK(gSelAnchor <= mid && gSelHead >= mid, "selectWord spans the anchor");
        selectLine(0, mid);
        CHECK(gSelHead >= gSelAnchor, "selectLine is ordered");
        clearSelection();
    }

    printf("== search ==\n");
    {
        // a fixed literal the fixture is known to contain (page 0's body text),
        // not a word re-derived from text the code under test just produced
        std::string needle = "Independent";
        gQuery = utf8ToWide(needle);
        runSearch();
        CHECK(searchCount() > 0, "search finds the query");
        int firstActive = ui_matchActive();
        nextMatch(1);
        CHECK(searchCount() > 0, "nextMatch keeps results");
        CHECK(ui_matchActive() != firstActive, "nextMatch advances the active match");
        gQuery.clear();
        runSearch();
        CHECK(searchCount() == 0, "empty query clears matches");
    }

    printf("== marks: highlight, colour, undo, redo ==\n");
    {
        gSelAnchorPage = 0; gSelHeadPage = 0; gSelAnchor = 5; gSelHead = 40;
        applyHighlightFromSelection(0);
        CHECK(gHls.size() == 1, "one highlight created");
        CHECK(gDirty, "document is dirty after a mark");
        CHECK(gUndo.size() == 1, "one undo op");
        CHECK(hlAt(0, 10) == 0, "hlAt finds the mark inside its range");
        CHECK(hlAt(0, 2) == -1, "hlAt misses outside the range");

        recolorHighlight(0, 2);
        CHECK(gHls[0].color == 2, "recolor applies");
        deleteHighlight(0);
        CHECK(gHls.empty(), "delete removes it");
        undoOp();   // undo delete
        CHECK(gHls.size() == 1, "undo restores the highlight");
        undoOp();   // undo recolor
        CHECK(gHls.size() == 1 && gHls[0].color == 0, "undo restores the colour");
        undoOp();   // undo add
        CHECK(gHls.empty(), "undo removes the added highlight");
        redoOp();
        CHECK(gHls.size() == 1, "redo re-adds it");
        redoOp();
        CHECK(gHls[0].color == 2, "redo re-applies the colour");
        redoOp();
        CHECK(gHls.empty(), "redo re-deletes (delete was the undone op)");
    }

    printf("== undo/redo leave the document savable (regression: missing markDirty) ==\n");
    {
        // a user's real sequence: add a mark, undo it, save. The file must show
        // the undone state; without markDirty() the save was skipped entirely
        // and the mark silently stayed in the file on disk.
        gHls.clear(); gPins.clear(); gUndo.clear(); gRedo.clear(); gTomb.clear(); gDirty = false;
        gSelAnchorPage = gSelHeadPage = 0; gSelAnchor = 3; gSelHead = 30;
        applyHighlightFromSelection(0);
        CHECK(gDirty, "adding a mark dirties the document");
        undoOp();
        CHECK(gDirty, "undo leaves the document dirty, so Save still writes");
        CHECK(gHls.empty(), "the undo removed the mark");
        redoOp();
        CHECK(gDirty, "redo also leaves the document dirty");
        CHECK(gHls.size() == 1, "the redo brought the mark back");
    }

    printf("== marks: overlapping highlight replaces, never stacks ==\n");
    {
        gHls.clear(); gUndo.clear(); gRedo.clear();
        gSelAnchorPage = gSelHeadPage = 0; gSelAnchor = 10; gSelHead = 20;
        applyHighlightFromSelection(0);
        gSelAnchorPage = gSelHeadPage = 0; gSelAnchor = 15; gSelHead = 25;   // clearSelection emptied the pages too
        applyHighlightFromSelection(1);
        CHECK(gHls.size() == 1, "overlapping highlight replaces the old one");
        CHECK(gHls[0].color == 1, "the newer colour wins");
    }

    printf("== marks: pins ==\n");
    {
        gPins.clear(); gUndo.clear(); gRedo.clear();
        addPinAt(0, 100, 100);
        CHECK(gPins.size() == 1, "pin added");
        commitPinText(0, true, L"hello note");
        CHECK(gPins.size() == 1 && gPins[0].text == L"hello note", "pin text committed");
        CHECK(gUndo.size() == 1, "pin add is one undo op");
        recolorPin(0, 2);
        CHECK(gPins[0].color == 2, "pin recolored");
        undoOp(); CHECK(gPins[0].color == gPinDefault, "undo pin colour");
        deletePin(0);
        CHECK(gPins.empty(), "pin deleted");
        undoOp();
        CHECK(gPins.size() == 1, "undo restores pin");
        // an empty new pin cancels itself
        addPinAt(0, 200, 200);
        commitPinText((int)gPins.size() - 1, true, L"");
        CHECK(gPins.size() == 1, "an empty new pin is dropped");
        // out-of-range page is refused, not clamped
        size_t before = gPins.size();
        addPinAt(gDocPages + 5, 10, 10);
        CHECK(gPins.size() == before, "addPinAt refuses an out-of-range page");
    }

    printf("== page rotation ==\n");
    {
        gUndo.clear(); gRedo.clear();
        double w0 = gPageW[0], h0 = gPageH[0];
        int y1before = yTopPx(1);
        rotatePage(0, +1);
        CHECK(gRot[0] == 1, "rotate clockwise sets rot 1");
        CHECK(rotWpt(0) == (int)(h0 + 0.5), "rotated width is the old height");
        CHECK(yTopPx(1) != y1before, "layout shifts: page 1 moved");
        undoOp();
        CHECK(gRot[0] == 0 && yTopPx(1) == y1before, "undo restores rotation and layout");
        (void)w0;
    }

    printf("== zoom and scroll ==\n");
    {
        gFitWidth = false;
        double z = gZoom;
        zoomAt(1.2, 500, 400);
        CHECK(gZoom > z, "zoom in raises zoom");
        CHECK(!gFitWidth, "manual zoom ends fit-width");
        for (int i = 0; i < 40; i++) zoomAt(1.2, 500, 400);
        CHECK(gZoom == 8.0, "zoom clamps to exactly the ceiling");
        for (int i = 0; i < 80; i++) zoomAt(1 / 1.2, 500, 400);
        CHECK(gZoom == 0.25, "zoom clamps to exactly the floor");
        gZoom = 1.0;
        gScrollY = 999999;
        clampScroll();
        CHECK(gScrollY == docHpx() - docViewH(), "scroll clamps to exactly the document bottom");
        gScrollY = -500;
        clampScroll();
        CHECK(gScrollY == 0, "scroll clamps at the top");
    }

    printf("== sidecar: write and restore ==\n");
    std::string sidecarName;
    {
        gHls.clear(); gPins.clear(); gUndo.clear(); gRedo.clear(); gTomb.clear();
        gSelAnchorPage = 0; gSelHeadPage = 0; gSelAnchor = 3; gSelHead = 30;
        applyHighlightFromSelection(1);
        addPinAt(1, 120, 300);
        commitPinText(0, true, L"a note, with a comma\nand a newline");
        gZoom = 1.75; gFitWidth = false;
        loadPage(2);
        writeSidecarNow();
        sidecarName = sidecarPathFor_public(gPath);

        FILE* f = fopen(sidecarName.c_str(), "rb");
        CHECK(f != nullptr, "sidecar file exists");
        char buf[8192] = ""; 
        size_t got = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
        if (f) fclose(f);
        buf[got] = 0;
        CHECK(strstr(buf, "fmt=2") != nullptr, "sidecar carries the format version");
        CHECK(strstr(buf, "zoom=1.750000") != nullptr, "sidecar carries the exact zoom");
        CHECK(strstr(buf, "fit=0") != nullptr, "sidecar records fit-width off after a manual zoom");
        CHECK(strstr(buf, "page=3") != nullptr, "sidecar carries the page (1-based)");
        // the exact tuples ARE the contract the Windows reader parses; a key
        // without its fields would not round-trip
        CHECK(strstr(buf, "hl=0,3,28,1") != nullptr, "the highlight tuple is exact (page,start,count,colour)");
        CHECK(strstr(buf, "pin=1,120.00,300.00,c5,a note, with a comma\\nand a newline") != nullptr,
              "the pin tuple is exact and its newline is escaped \\n");
        printf("       sidecar:\n");
        for (char* ln = strtok(buf, "\n"); ln; ln = strtok(nullptr, "\n")) printf("         %s\n", ln);

        // reopen: the marks and the view come back
        size_t nhl = gHls.size(), npin = gPins.size();
        std::wstring pinText = gPins.empty() ? L"" : gPins[0].text;
        int pinPage = gPins.empty() ? -1 : gPins[0].page;
        double pinX = gPins.empty() ? 0 : gPins[0].x, pinY = gPins.empty() ? 0 : gPins[0].y;
        int pinColor = gPins.empty() ? -1 : gPins[0].color;
        CHECK(openPath(wpdf), "reopen the same file");
        CHECK(gHls.size() == nhl, "highlights restored from sidecar");
        CHECK(gPins.size() == npin, "pins restored from sidecar");
        CHECK(!gPins.empty() && gPins[0].text == pinText, "pin text survives the escape round trip");
        CHECK(fabs(gZoom - 1.75) < 1e-6, "zoom restored");
        CHECK(gPageIndex == 2, "page restored");
        // the pin's position and colour round trip too, not only its text
        CHECK(!gPins.empty() && gPins[0].page == pinPage, "the pin's page survives");
        CHECK(!gPins.empty() && fabs(gPins[0].x - pinX) < 0.01 && fabs(gPins[0].y - pinY) < 0.01,
              "the pin's position survives");
        CHECK(!gPins.empty() && gPins[0].color == pinColor, "the pin's colour survives");
    }

    printf("== save: bake marks into the PDF, then read them back ==\n");
    {
        std::string outpdf = scratch + "/saved.pdf";
        gStubSavePath = outpdf;
        CHECK(doSaveAs(), "save as succeeds");
        struct stat st;
        CHECK(stat(outpdf.c_str(), &st) == 0 && st.st_size > 1000, "saved file written to disk");
        CHECK(!gDirty, "document is clean after save");

        // the saved file must carry the app's OWN annotations: the fixture
        // already carries 56 of its own, so counting all annotations would pass
        // even if the bake wrote nothing
        int totalAnnots = countAnnots(outpdf, true);
        CHECK(totalAnnots == 2, "exactly the highlight and the pin are baked as real annotations");
        printf("       mnpdf annotations in saved file: %d\n", totalAnnots);

        // delete the sidecar so the marks can only come from the file itself
        remove(sidecarName.c_str());
        CHECK(openPath(utf8ToWide(outpdf)), "open the saved file");
        CHECK(gHls.size() >= 1, "highlight lifted back out of the file");
        CHECK(gPins.size() >= 1, "pin lifted back out of the file");
        CHECK(!gPins.empty() && !gPins[0].text.empty(), "the lifted pin kept its note");
        // saving again must not duplicate the marks
        std::string out2 = scratch + "/resaved.pdf";
        gStubSavePath = out2;
        // force dirty so doSaveImpl writes even though nothing else changed
        markDirty();
        CHECK(doSaveImpl(utf8ToWide(out2)), "second save succeeds");
        int dupes = countAnnots(out2, true);
        CHECK(dupes == totalAnnots, "re-saving does not pile up duplicate annotations");
        printf("       mnpdf annotations after re-save: %d (was %d)\n", dupes, totalAnnots);
    }

    printf("== page surgery: delete, move, split ==\n");
    {
        CHECK(openPath(wpdf), "reopen the pristine file");
        remove(sidecarPathFor_public(gPath).c_str());
        int n0 = gDocPages;
        CHECK(n0 >= 4, "the pristine document still has its pages");
        deletePage(0);
        CHECK(gDocPages == n0 - 1, "deletePage drops exactly one page");
        movePage(0, 2);
        CHECK(gDocPages == n0 - 1, "movePage keeps the count");

        std::string out = scratch + "/split.pdf";
        gStubSavePath = out;
        splitExport(utf8ToWide(out), "2-3", true);
        struct stat st;
        CHECK(stat(out.c_str(), &st) == 0 && st.st_size > 500, "split writes a new file");
        FPDF_DOCUMENT sp = FPDF_LoadDocument(out.c_str(), nullptr);
        CHECK(sp && FPDF_GetPageCount(sp) == 2, "split file holds the requested range");
        if (sp) FPDF_CloseDocument(sp);
        splitExport(utf8ToWide(scratch + "/bad.pdf"), "99-100", true);
        CHECK(stat((scratch + "/bad.pdf").c_str(), &st) != 0, "a bad range writes nothing");
    }

    printf("== merge ==\n");
    {
        CHECK(openPath(wpdf), "reopen for merge");
        remove(sidecarPathFor_public(gPath).c_str());
        int before = gDocPages;
        std::vector<std::wstring> files = { wpdf };
        mergeFiles(files);
        CHECK(gDocPages == before * 2, "merge doubles the page count");
    }

    printf("== tabs ==\n");
    {
        // one bundle per tab: the marks belong to the document, not the window
        CHECK(gDocCount == 1, "starts with one bundle");
        gDocs[1] = new Doc();
        gDocCount = 2;
        int idx = gActiveDoc;
        std::wstring p1 = gPath;
        gActiveDoc = 1;
        CHECK(gPath.empty(), "a second bundle starts empty");
        CHECK(gHls.empty(), "and holds no marks of its own");
        openPath(wpdf);
        CHECK(gDocPages == totalPages, "second bundle opens its own document");
        gActiveDoc = idx;
        CHECK(gPath == p1, "switching back restores the first bundle's view");
        delete gDocs[1];
        gDocs[1] = nullptr;
        gDocCount = 1;
        gActiveDoc = 0;
    }

    printf("== prefs ==\n");
    {
        gNight = true; gAutosave = true; gHlDefault = 3; gPinDefault = 1;
        gPalCustom[0] = RGB(0x12, 0x34, 0x56);
        writeAppPref();
        gNight = false; gHlDefault = 0; gPinDefault = 0; gPalCustom[0] = CLR_INVALID;
        loadAppPref();
        CHECK(gNight == true, "night mode remembered");
        CHECK(gHlDefault == 3, "default highlight colour remembered");
        CHECK(gPinDefault == 1, "default pin colour remembered");
        CHECK(gPalCustom[0] != CLR_INVALID, "custom palette slot remembered");
        COLORREF c = gPalCustom[0];
        CHECK(GetRValue(c) == 0x12 && GetGValue(c) == 0x34 && GetBValue(c) == 0x56, "custom colour bytes match");
    }

    printf("== format interop: marks baked by the Windows app lift on Linux ==\n");
    {
        // tests/mnpdf-baked.pdf carries a Highlight with Source "mnpdf 0 45" and
        // a Text note with Source "mnpdf" - exactly what doSave() writes on
        // Windows. Opening it here must lift both back into live marks and leave
        // any foreign annotation alone.
        std::string baked = dirOf + "mnpdf-baked.pdf";
        struct stat st;
        // a missing fixture is a failure, not a skip: these 12 checks are the
        // only test of the Windows bake contract, and a silent skip would leave
        // the suite green with them gone
        if (stat(baked.c_str(), &st) != 0) {
            gFail++; printf("  FAIL the interop fixture is missing (%s)\n", baked.c_str());
        } else {
            CHECK(openPath(utf8ToWide(baked)), "open the file the Windows app would write");
            CHECK(gHls.size() == 1, "the baked highlight lifts into a live mark");
            CHECK(!gHls.empty() && gHls[0].start == 0 && gHls[0].count == 45, "its char range survives");
            CHECK(!gHls.empty() && gHls[0].baked, "the lifted highlight is marked baked");
            CHECK(gPins.size() == 1, "the baked note lifts into a live pin");
            CHECK(!gPins.empty() && gPins[0].text == L"A note from the Windows app", "the note text is exact");
            CHECK(!gPins.empty() && gPins[0].color == kPalPreset - 1, "red maps back to the pin preset");
            // a fresh save must re-bake them without duplicating
            markDirty();
            std::string rt = scratch + "/interop.pdf";
            gStubSavePath = rt;
            CHECK(doSaveImpl(utf8ToWide(rt)), "re-save the lifted marks");
            remove(sidecarPathFor_public(utf8ToWide(rt)).c_str());
            CHECK(openPath(utf8ToWide(rt)), "reopen the re-saved file");
            CHECK(gHls.size() == 1, "still exactly one highlight (no duplication)");
            CHECK(gPins.size() == 1, "still exactly one pin");
            // and the highlight colour must map back to a palette entry
            CHECK(!gHls.empty() && gHls[0].color == 0, "yellow maps back to palette entry 0");
        }
    }

    printf("== refusal: a non-JPEG is never handed to pdfium ==\n");
    {
        std::wstring notjpeg = utf8ToWide(scratch + "/fake.jpg");
        FILE* f = fopen((scratch + "/fake.jpg").c_str(), "wb");
        if (f) { fputs("this is a text file pretending to be a picture", f); fclose(f); }
        CHECK(!looksLikeJpeg(notjpeg), "a fake .jpg is refused");
        CHECK(!looksLikeJpeg(utf8ToWide("/nonexistent/file.jpg")), "a missing file is refused");
        // A frame-less chain (SOI, SOS, entropy, EOI - no SOF anywhere) and a
        // scan naming a component the frame never declared both reach the loader
        // and die at first paint, which replaySigs would then repeat on every
        // open. Regression: an earlier port of the guard accepted both.
        {
            unsigned char frameless[] = {0xFF,0xD8,0xFF,0xDA,0x00,0x04,0x01,0x00,0x00,0x11,0x22,0x33,0xFF,0xD9};
            unsigned char badcomp[] = {0xFF,0xD8,0xFF,0xC0,0x00,0x0B,0x00,0x08,0x00,0x08,0x03,0x01,0x11,0x00,
                                       0x02,0x21,0x11,0xFF,0xDA,0x00,0x04,0x01,0x09,0x00,0xFF,0xD9};
            FILE* fp = fopen((scratch + "/frameless.jpg").c_str(), "wb");
            if (fp) { fwrite(frameless, 1, sizeof frameless, fp); fclose(fp); }
            fp = fopen((scratch + "/badcomp.jpg").c_str(), "wb");
            if (fp) { fwrite(badcomp, 1, sizeof badcomp, fp); fclose(fp); }
            CHECK(!looksLikeJpeg(utf8ToWide(scratch + "/frameless.jpg")),
                  "a frame-less JPEG chain is refused (would crash at first paint)");
            CHECK(!looksLikeJpeg(utf8ToWide(scratch + "/badcomp.jpg")),
                  "a scan naming an undeclared component is refused");
        }
        // the real sample the Windows suite ships must pass
        CHECK(looksLikeJpeg(utf8ToWide(dirOf + "sig-sample.jpg")),
              "the sample signature JPEG is accepted");
    }

    printf("\npass=%d fail=%d\n", gPass, gFail);
    flushPageCache();
    if (gDoc) FPDF_CloseDocument(gDoc);
    for (int i = 0; i < gDocCount; i++) delete gDocs[i];
    FPDF_DestroyLibrary();
    return gFail ? 1 : 0;
}
