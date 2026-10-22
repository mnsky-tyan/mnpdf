// mnpdf for Linux - shared core state.
//
// The document model, marks, undo engine, geometry and palette are ported
// verbatim from the Windows src/main.cpp so the two readers behave the same and
// read each other's saved state. What changes is the platform surface: GTK3 +
// cairo replace Win32 + GDI, and the manual continuous-scroll model is kept
// exactly (gScrollX/gScrollY drive the same yTopPx/docHpx geometry), so the
// well-tested layout, search and selection math port over unchanged.
//
// The Windows app writes to %APPDATA%\mnpdf; this one uses XDG config home
// (~/.config/mnpdf) but the FILE FORMATS are byte-identical, so state written
// by either reader is understood by the other (the only difference is where the
// directory lives). See sidecar.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "fpdf_ppo.h"
#include "fpdf_doc.h"

// A portable colour: the Windows side uses COLORREF (0x00BBGGRR). Keeping the
// same packing means the palette constants and %02x%02x%02x labels port with
// no surprises.
using COLORREF = uint32_t;
#define RGB(r, g, b) ((COLORREF)(((unsigned char)(r)) | ((unsigned char)(g) << 8) | ((unsigned char)(b) << 16)))
#define GetRValue(c) ((unsigned char)((c) & 0xff))
#define GetGValue(c) ((unsigned char)(((c) >> 8) & 0xff))
#define GetBValue(c) ((unsigned char)(((c) >> 16) & 0xff))
static const COLORREF CLR_INVALID = 0xFFFFFFFFu;

// ---- constants (mirrors main.cpp) ----
static const double kSpotTolPt = 2.0;
static const double kSpotTolBakePt = 1.5;
static const double kStampBoundsTol = 0.25;
static const int kPalPreset = 6;
static const int kPalCustom = 3;
static const int kPalCount = kPalPreset + kPalCustom;
static const int MARGIN = 8;
static const int GAP = 16;
static const int kMatchCap = 4096;
static const int kSidecarFmt = 2;
static const int kNoteMaxChars = 4000;
static const int kMaxTabs = 16;

// ---- marks ----
struct Hl { int page, start, count, color; bool baked; };
struct Pin { int page; double x, y; std::wstring text; int color; bool baked; };
struct Sig {
    int page = 0;
    double cx = 0, cy = 0;
    double w = 150, h = 60;
    std::wstring path;
};
struct Tomb { int kind, page, start, count; double x, y; };

// ---- per-document state: the bundle ----
struct Doc {
    std::wstring path;
    // the path this tab showed before the current one: "Reopen last document"
    // goes back to it. Mirrors Windows' gViewTab->prevPath (src/main.cpp:244).
    std::wstring prevPath;
    FPDF_DOCUMENT doc = nullptr;
    FPDF_PAGE page = nullptr;
    FPDF_TEXTPAGE textPage = nullptr;
    int pageIndex = 0;
    int pageCount = 0;
    double pageWpt = 612, pageHpt = 792;
    bool restoredPage = false;
    double* pageW = nullptr;
    double* pageH = nullptr;
    double* prefixPt = nullptr;
    int* rot = nullptr;
    int docPages = 0;
    double maxPageW = 0;
    double zoom = 1.0;
    int scrollX = 0, scrollY = 0;
    bool fitWidth = false;
    std::vector<Tomb> tomb;
    bool dirty = false;
    std::vector<Hl> hls;
    std::vector<Pin> pins;
    std::vector<Sig> sigs;

    struct Op {
        int kind;
        std::vector<Hl> added, replaced;
        Hl hlVal = {};
        int hlFrom = 0, hlTo = 0;
        Pin pinVal = {};
        std::wstring textFrom, textTo;
        int pinIdx = -1;
        int rotPage = 0, rotFrom = 0, rotTo = 0;
    };
    std::vector<Op> undo, redo;
    int selAnchor = -1, selHead = -1;
    int selAnchorPage = -1, selHeadPage = -1;

    ~Doc() {
        delete[] pageW;
        delete[] pageH;
        delete[] prefixPt;
        delete[] rot;
    }
    // non-copyable (owns raw arrays); movable is not needed - tabs hold pointers
    Doc() = default;
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};
using Op = Doc::Op;

// ---- the open documents: one bundle per tab, one active ----
extern Doc* gDocs[kMaxTabs];
extern int gDocCount;
extern int gActiveDoc;
#define gViewTab (gDocs[gActiveDoc])
#define gPath     gViewTab->path
// the document this tab showed before the current one, for "Reopen last
// document" (Windows: gViewTab->prevPath, src/main.cpp:244)
#define gPrevPath gViewTab->prevPath
#define gDoc      gViewTab->doc
#define gPage     gViewTab->page
#define gTextPage gViewTab->textPage
#define gPageIndex gViewTab->pageIndex
#define gRestoredPage gViewTab->restoredPage
#define gPageCount gViewTab->pageCount
#define gPageWpt  gViewTab->pageWpt
#define gPageHpt  gViewTab->pageHpt
#define gTomb     gViewTab->tomb
#define gHls      gViewTab->hls
#define gPins     gViewTab->pins
#define gSigs     gViewTab->sigs
#define gUndo     gViewTab->undo
#define gRedo     gViewTab->redo
#define gDirty    gViewTab->dirty
#define gSelAnchor gViewTab->selAnchor
#define gSelHead  gViewTab->selHead
#define gSelAnchorPage gViewTab->selAnchorPage
#define gSelHeadPage gViewTab->selHeadPage
#define gPageW   gViewTab->pageW
#define gPageH   gViewTab->pageH
#define gPrefixPt gViewTab->prefixPt
#define gRot     gViewTab->rot
#define gDocPages gViewTab->docPages
#define gMaxPageW gViewTab->maxPageW
#define gZoom    gViewTab->zoom
#define gScrollX gViewTab->scrollX
#define gScrollY gViewTab->scrollY
#define gFitWidth gViewTab->fitWidth

// ---- palette ----
extern const COLORREF gPalPreset[kPalPreset];
extern const char* const gPalName[kPalPreset];
extern COLORREF gPalCustom[kPalCustom];
extern int gPalNext;
extern int gHlDefault;
extern int gPinDefault;
extern std::wstring gSigPlacing;

bool palColor(int idx, COLORREF& out);
std::wstring palLabel(int idx);
int nearestPalColor(unsigned r, unsigned g, unsigned b);

// ---- view / geometry (client px) ----
extern int gClientW, gClientH;
extern bool gNight;
extern bool gTabStrip;
extern bool gAutosave;
extern bool gTitlebar;
extern bool gSearchOpen;
extern std::wstring gQuery;

// the platform layer supplies these (GTK): invalidate the view, show a message,
// request a file, etc. Declared in ui.h, implemented by main.cpp.
void relayoutPages();
void clampScroll();
void ensureActivePage();
void renderPage();
double clampZoom(double z);

int tabStripH();
int docWpx();
int docHpx();
int rotWpt(int i);
int rotHpt(int i);
int pageWpx(int i);
int pageHpx(int i);
int yTopPx(int i);
int docViewH();
int docTop();
int docLeft();
int pageLeftPx(int i);
int pageVx(int i);
int pageVy(int i);
bool pageInView(int page);
bool clipPageRect(int page, int* x0, int* y0, int* x1, int* y1);

// page coordinate transforms
void pagePtToPx(int page, double x, double y, double* ox, double* oy);
void pagePxToPt(int page, double sx, double sy, double* ox, double* oy);

// page cache + text
FPDF_PAGE acquirePage(int i);
FPDF_TEXTPAGE textPageOf(int page);
void flushPageCache();
bool loadPage(int index);

// text resolution / selection
int pageAtDocY(int docY);
int charIndexAtDoc(int cx, int cy, int* pageOut);
int charIndexClamped(int cx, int cy, int* pageOut);
void clearSelection();
bool hasSelection();
void selRange(int* p0, int* i0, int* p1, int* i1);
bool isWordSep(unsigned int u);
void selectWord(int page, int idx);
void selectLine(int page, int idx);
void copySelection();
void copyTextRange(int p0, int i0, int p1, int i1);
bool pageSlice(int p, int p0, int i0, int p1, int i1, FPDF_TEXTPAGE* tpOut, int* sOut, int* eOut);

// marks
int hlAt(int page, int idx);
void normalizePins();
void normalizeHls();
void applyHighlightFromSelection(int color);
void recolorHighlight(int idx, int color);
void deleteHighlight(int idx);
void addPinAt(int page, double px, double py);
void deletePin(int idx);
void recolorPin(int idx, int color);
void rotatePage(int page, int dir);
int pinAt(int cx, int cy);
int pinRadius();

// undo
void pushOp(const Op& op);
void undoOp();
void redoOp();
void markDirty();
void markSave();

// the shared-preference role: a window that does not own it writes neither
// app.txt nor last.txt, so a second reader cannot overwrite the first one's
// preferences. The Windows build claims a named mutex for the same purpose
// (src/main.cpp:6138).
extern bool gPrimaryInstance;
bool claimPrimaryInstance();
void releasePrimaryInstance();

// the update check's own state: the unix time of the last attempt and the
// newest tag it found. It is kept in the Linux pair in app.txt
// (lnxupdcheck=/lnxupdtag=): updcheck=/updtag= are the Windows reader's own
// stamp and carry a different meaning there (the newest non-prerelease
// overall, src/main.cpp:2435) than this check's answer (the newest release
// carrying a Linux archive). They live here because the cooldown spans the
// whole app, not one module.
extern long long gLastUpdateCheck;
extern std::string gLastUpdateTag;

// Does a release carry the Linux archive? The answer is three-valued: yes, no,
// and "the host did not answer". The third is not the second - a timeout or a
// refused connection is not evidence that the asset is missing.
enum class AssetAnswer { Yes, No, Unreachable };

// The newest release that carries a Linux archive, from the feed's tags in
// release order (newest first). An Unreachable answer stops the search and
// sets `unknown`, because the answer is then unknown, not "not there":
// falling through to the next tag would name an OLDER release as the newest,
// telling a reader on that older build it is up to date. `unknown` is false
// when every tag really answered No.
std::string pickLinuxRelease(const std::vector<std::string>& tags,
                             const std::function<AssetAnswer(const std::string&)>& probe,
                             bool* unknown);

// layout helpers
void allocPageTables(int pages);
bool applyFitWidth();
void fitWidth();
void zoomAt(double factor, int cx, int cy);

// document open/save
// persistence (sidecar.cpp)
std::string appDir();
std::string sidecarPathFor_public(const std::wstring& path);
void writeSidecarNow();
void writeAllSidecars();
void writeLastDocument(const std::wstring& path);
std::wstring lastDocumentPath();
void restoreSidecar(const std::wstring& path);
void loadAppPref();
void writeAppPref();

// search (search.cpp)
void runSearch();
void nextMatch(int dir);
int searchCount();
void searchRemapPages(const std::vector<int>& back, int oldCount);

// signatures and surgery (opendoc.cpp / surgery.cpp)
bool looksLikeJpeg(const std::wstring& path);
void commitSigPlacement(int page, double sx, double sy);
void pickSignatureImage();
void clearSignatures();
void deletePage(int idx);
void movePage(int from, int to);
void mergeFiles(const std::vector<std::wstring>& files);
void splitExport(const std::wstring& target, const std::string& range, bool quiet);

// rendering (render.cpp)
bool renderPageToBuffer(int page, int w, int h, std::vector<unsigned char>& out, int* strideOut);

// undo helpers shared with the platform layer
void commitPinText(int idx, bool fresh, const std::wstring& text);

bool openPath(const std::wstring& path);
bool doSave();
bool doSaveAs();
bool doSaveImpl(const std::wstring& target);
bool bakeAndWrite(const std::wstring& target);
void replaySigs();

// misc
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
// std::wstring (code points) <-> PDFium's UTF-16 (unsigned short), NUL-terminated
std::vector<unsigned short> wideToU16(const std::wstring& s);
std::wstring u16ToWide(const unsigned short* p, size_t n);
// PDFium's string getters (FPDFBookmark_GetTitle, FPDFAnnot_GetStringValue)
// count BYTES including the terminating NUL, while the rest of the code works in
// UTF-16 units: this is the one place that conversion is made.
std::wstring pdfBytesToWide(const unsigned short* p, unsigned long bytes);

