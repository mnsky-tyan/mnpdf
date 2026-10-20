// mnpdf for Linux - persistence: per-document sidecar + app prefs + last file.
// The FILE FORMATS are byte-identical to the Windows app's, so state written by
// either reader is understood by the other. Only the directory differs:
// Windows uses %APPDATA%\mnpdf, this uses XDG config home (~/.config/mnpdf).
//
// The sidecar name is FNV-1a over the path, the same hash the Windows app uses
// (it hashes the UTF-16 bytes of the wide path), so a shared config directory
// would still line the two readers up on the same document.
#include "doc.h"
#include "ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

// the sidecar field escape pairs, identical to the Windows writer: a backslash
// always becomes "\\", and each table maps one source byte to one code byte.
// Pin text escapes newlines; a stamp path escapes commas (its \r is dropped).
struct SidecarEsc { char src; char code; };
static const SidecarEsc kPinEsc[] = { { '\n', 'n' } };
static const SidecarEsc kSigEsc[] = { { ',', 'c' } };

static std::string escField(const std::string& raw, const SidecarEsc* table, size_t n) {
    std::string esc;
    esc.reserve(raw.size());
    for (char p : raw) {
        if (p == '\\') { esc += "\\\\"; continue; }
        bool hit = false;
        for (size_t i = 0; i < n; i++)
            if (p == table[i].src) { esc += '\\'; esc += table[i].code; hit = true; break; }
        if (!hit) esc += p;
    }
    return esc;
}

static std::string unescField(const std::string& e, const SidecarEsc* table, size_t n) {
    std::string un;
    un.reserve(e.size());
    for (size_t q = 0; q < e.size(); q++) {
        if (e[q] == '\\' && q + 1 < e.size()) {
            if (e[q + 1] == '\\') { un += '\\'; q++; continue; }
            bool hit = false;
            for (size_t i = 0; i < n; i++)
                if (e[q + 1] == table[i].code) { un += table[i].src; q++; hit = true; break; }
            if (hit) continue;
        }
        un += e[q];
    }
    return un;
}

extern bool sidecarPending();
extern void sidecarClearPending();

std::string appDir() {
    const char* x = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (x && *x) base = x;
    else {
        const char* home = getenv("HOME");
        if (!home) { struct passwd* pw = getpwuid(getuid()); home = pw ? pw->pw_dir : "."; }
        base = std::string(home) + "/.config";
    }
    std::string dir = base + "/mnpdf";
    mkdir(base.c_str(), 0755);                // ok if it exists
    mkdir(dir.c_str(), 0755);                 // ok if it exists
    return dir;
}

// FNV-1a over the UTF-16 bytes of the path, as the Windows app hashes it: a
// wchar_t is 16-bit there, so a non-BMP code point contributes its surrogate
// PAIR (4 bytes), and this must hash the same units or the two platforms would
// name the same document's sidecar differently.
static void hashUnit(uint64_t& h, unsigned short c) {
    h = (h ^ (unsigned char)(c & 0xff)) * 1099511628211ULL;
    h = (h ^ (unsigned char)((c >> 8) & 0xff)) * 1099511628211ULL;
}
static uint64_t pathHash(const std::wstring& path) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < path.size(); i++) {
        uint32_t cp = (uint32_t)path[i];
        if (cp < 0x10000) { hashUnit(h, (unsigned short)cp); }
        else {
            cp -= 0x10000;
            hashUnit(h, (unsigned short)(0xD800 + (cp >> 10)));
            hashUnit(h, (unsigned short)(0xDC00 + (cp & 0x3FF)));
        }
    }
    hashUnit(h, 0);                              // the terminator Windows hashes too
    return h;
}

static std::string sidecarPathFor(const std::wstring& path) {
    char name[80];
    snprintf(name, sizeof(name), "/doc-%016llx.txt", (unsigned long long)pathHash(path));
    return appDir() + name;
}

// exposed for the headless test (and nothing else needs it)
std::string sidecarPathFor_public(const std::wstring& path) { return sidecarPathFor(path); }

static bool readLine(FILE* fp, std::vector<char>& buf) {
    size_t len = 0;
    int c;
    bool any = false;
    while ((c = fgetc(fp)) != EOF) {
        any = true;
        if (c == '\n') break;
        if (len + 1 >= buf.size()) buf.resize(buf.size() * 2);
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = 0;
    return any;
}

static void writeSidecarFor(Doc* d);
void writeSidecarForBundle(Doc* d) { writeSidecarFor(d); }

static void writeSidecarFor(Doc* d) {
    if (!d || d->path.empty() || !d->doc) return;
    FILE* fp = fopen(sidecarPathFor(d->path).c_str(), "wb");
    if (!fp) return;
    fprintf(fp, "fmt=%d\nzoom=%.6f\nfit=%d\npage=%d\n", kSidecarFmt, d->zoom, d->fitWidth ? 1 : 0, d->pageIndex + 1);
    for (const Hl& h : d->hls) fprintf(fp, "hl=%d,%d,%d,%d\n", h.page, h.start, h.count, h.color);
    for (int i = 0; i < d->docPages; i++) if (d->rot && d->rot[i]) fprintf(fp, "rot=%d,%d\n", i, d->rot[i]);
    for (const Tomb& tb : d->tomb) {
        if (tb.kind == 0) fprintf(fp, "dl=h,%d,%d,%d\n", tb.page, tb.start, tb.count);
        else fprintf(fp, "dl=p,%d,%.2f,%.2f\n", tb.page, tb.x, tb.y);
    }
    for (const Pin& pn : d->pins) {
        if (pn.text.empty()) continue;
        std::string esc = escField(wideToUtf8(pn.text), kPinEsc, sizeof(kPinEsc) / sizeof(SidecarEsc));
        for (size_t r = esc.find('\r'); r != std::string::npos; r = esc.find('\r')) esc.erase(r, 1);
        fprintf(fp, "pin=%d,%.2f,%.2f,c%d,%s\n", pn.page, pn.x, pn.y, pn.color, esc.c_str());
    }
    for (const Sig& sg : d->sigs) {
        std::string esc = escField(wideToUtf8(sg.path), kSigEsc, sizeof(kSigEsc) / sizeof(SidecarEsc));
        fprintf(fp, "sig=%d,%.2f,%.2f,%.2f,%.2f,%s\n", sg.page, sg.cx, sg.cy, sg.w, sg.h, esc.c_str());
    }
    fclose(fp);
}

void writeSidecarNow() {
    if (gPath.empty() || !gDoc) return;
    writeSidecarFor(gViewTab);
    writeLastDocument(gPath);      // last.txt names the document just written: a plain launch reopens it
    sidecarClearPending();
}

void writeAllSidecars() {
    for (int i = 0; i < gDocCount; i++) writeSidecarFor(gDocs[i]);
    // the last file a tab held is the one a plain launch brings back
    if (gViewTab && !gViewTab->path.empty()) writeLastDocument(gViewTab->path);
}

// last.txt is the document a plain launch reopens
void writeLastDocument(const std::wstring& path) {
    if (path.empty()) return;
    FILE* fp = fopen((appDir() + "/last.txt").c_str(), "wb");
    if (!fp) return;
    std::string u8 = wideToUtf8(path);
    fwrite(u8.data(), 1, u8.size(), fp);
    fputc('\n', fp);
    fclose(fp);
}

std::wstring lastDocumentPath() {
    FILE* fp = fopen((appDir() + "/last.txt").c_str(), "rb");
    if (!fp) return L"";
    char buf[8192] = "";
    size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) got--;
    buf[got] = 0;
    if (!got) return L"";
    return utf8ToWide(std::string(buf, got));
}

void restoreSidecar(const std::wstring& path) {
    FILE* fp = fopen(sidecarPathFor(path).c_str(), "rb");
    if (!fp) return;
    double zoom = -1;
    int fit = -1, page = -1, fmt = 0;
    std::vector<char> lineBuf(512);
    while (readLine(fp, lineBuf)) {
        char* line = lineBuf.data();
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
            int q; double xx, yy;
            if (sscanf(line + 5, "%d,%lf,%lf", &q, &xx, &yy) == 3 && q >= 0 && q < gDocPages)
                gTomb.push_back({ 1, q, 0, 0, xx, yy });
        } else if (sscanf(line, "hl=%d,%d,%d,%d", &a, &b, &c, &d2) == 4) {
            if (a >= 0 && a < gDocPages && b >= 0 && c > 0) {
                int col = d2 >= 0 && d2 < kPalCount ? d2 : 0;
                bool have = false;
                for (int k = (int)gHls.size() - 1; k >= 0; k--) {
                    Hl& h = gHls[k];
                    if (h.baked && h.page == a && h.start == b && h.count == c) {
                        have = true;
                        if (h.color != col) { gHls.erase(gHls.begin() + k); have = false; }
                        break;
                    }
                }
                if (!have) gHls.push_back({ a, b, c, col, false });
            }
        } else if (sscanf(line, "rot=%d,%d", &a, &b) == 2) {
            if (a >= 0 && a < gDocPages && b >= 0 && b < 4) gRot[a] = b;
        } else if (strncmp(line, "sig=", 4) == 0) {
            int pg; double cx, cy, ww, hh; int after = 0;
            if (sscanf(line + 4, "%d,%lf,%lf,%lf,%lf,%n", &pg, &cx, &cy, &ww, &hh, &after) == 5
                && after > 0 && pg >= 0 && pg < gDocPages) {
                std::string un = unescField(std::string(line + 4 + after), kSigEsc, sizeof(kSigEsc) / sizeof(SidecarEsc));
                Sig sg; sg.page = pg; sg.cx = cx; sg.cy = cy;
                sg.w = ww > 4 ? ww : 150; sg.h = hh > 4 ? hh : 60;
                sg.path = utf8ToWide(un);
                gSigs.push_back(sg);
            }
        } else if (strncmp(line, "pin=", 4) == 0) {
            int c1 = -1, c2 = -1, c3 = -1;
            for (int k = 4; line[k]; k++) {
                if (line[k] == ',') { if (c1 < 0) c1 = k; else if (c2 < 0) c2 = k; else { c3 = k; break; } }
            }
            int pp; double xx, yy;
            if (c3 > 0 && sscanf(line + 4, "%d,%lf,%lf", &pp, &xx, &yy) == 3 && pp >= 0 && pp < gDocPages) {
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
                std::string un = unescField(std::string(txt), kPinEsc, sizeof(kPinEsc) / sizeof(SidecarEsc));
                std::wstring wtxt = utf8ToWide(un);
                bool have = false;
                for (int k = (int)gPins.size() - 1; k >= 0; k--) {
                    const Pin& p = gPins[k];
                    if (p.baked && p.page == pp && fabs(p.x - xx) < kSpotTolPt && fabs(p.y - yy) < kSpotTolPt) {
                        have = true;
                        if (p.color != pcol || p.text != wtxt) { gPins.erase(gPins.begin() + k); have = false; }
                        break;
                    }
                }
                if (!have) gPins.push_back({ pp, xx, yy, wtxt, pcol, false });
            }
        }
    }
    fclose(fp);
    normalizeHls();
    normalizePins();
    for (const Tomb& tb : gTomb) {
        if (tb.kind == 0) {
            for (int k = (int)gHls.size() - 1; k >= 0; k--)
                if (gHls[k].page == tb.page && gHls[k].start == tb.start && gHls[k].count == tb.count)
                    { gHls.erase(gHls.begin() + k); break; }
        } else {
            for (int k = (int)gPins.size() - 1; k >= 0; k--)
                if (gPins[k].page == tb.page && fabs(gPins[k].x - tb.x) < kSpotTolPt && fabs(gPins[k].y - tb.y) < kSpotTolPt)
                    { gPins.erase(gPins.begin() + k); break; }
        }
    }
    relayoutPages();
    if (fit == 1) { gFitWidth = true; applyFitWidth(); }
    else if (zoom > 0) { gZoom = clampZoom(zoom); gFitWidth = false; }
    if (page >= 1 && page <= gDocPages) {
        loadPage(page - 1);
        gScrollY = yTopPx(page - 1) - MARGIN;
        gRestoredPage = true;
    }
}

// exposed for the headless test (and nothing else needs it)

// ---- saved window geometry (app.txt) ----
// It lives here because sidecar.cpp owns app.txt; main.cpp only consumes it.
int gWinX = -1, gWinY = -1, gWinW = 1100, gWinH = 800;
bool gWinMax = false;

// ---- app prefs (app.txt) ----
void loadAppPref() {
    FILE* fp = fopen((appDir() + "/app.txt").c_str(), "rb");
    if (!fp) return;
    std::vector<char> lineBuf(128);
    while (readLine(fp, lineBuf)) {
        char* line = lineBuf.data();
        int iv;
        if (sscanf(line, "tabstrip=%d", &iv) == 1) gTabStrip = iv != 0;
        else if (sscanf(line, "autosave=%d", &iv) == 1) gAutosave = iv != 0;
        else if (sscanf(line, "night=%d", &iv) == 1) gNight = iv != 0;
        else if (sscanf(line, "hlcolor=%d", &iv) == 1) gHlDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "pincolor=%d", &iv) == 1) gPinDefault = (iv >= 0 && iv < kPalCount) ? iv : 0;
        else if (sscanf(line, "palnext=%d", &iv) == 1) gPalNext = (iv >= 0 && iv < kPalCustom) ? iv : 0;
        // geometry is bounded the same way the Windows reader bounds it: a value
        // a window cannot have leaves the field at its default
        else if (sscanf(line, "winx=%d", &iv) == 1) { if (iv >= -100000 && iv <= 100000) gWinX = iv; }
        else if (sscanf(line, "winy=%d", &iv) == 1) { if (iv >= -100000 && iv <= 100000) gWinY = iv; }
        else if (sscanf(line, "winw=%d", &iv) == 1) { if (iv > 0 && iv <= 32767) gWinW = iv; }
        else if (sscanf(line, "winh=%d", &iv) == 1) { if (iv > 0 && iv <= 32767) gWinH = iv; }
        else if (sscanf(line, "winmax=%d", &iv) == 1) gWinMax = iv != 0;
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

// window geometry is written by the platform layer through ui_windowRect()
extern void ui_windowRect(int* x, int* y, int* w, int* h, int* maxed);

// The keys this writer owns. Everything else in app.txt belongs to another
// reader of the same shared directory (Windows writes titlebar=, updcheck= and
// updtag=), so those lines are carried forward unchanged and in their original
// order instead of being dropped by the rewrite.
static bool linuxAppKey(const char* line) {
    static const char* kOwned[] = { "tabstrip=", "autosave=", "hlcolor=", "pincolor=", "night=",
                                    "winx=", "winy=", "winw=", "winh=", "winmax=", "palnext=" };
    for (const char* k : kOwned)
        if (strncmp(line, k, strlen(k)) == 0) return true;
    return strncmp(line, "pal", 3) == 0 && line[3] >= '0' && line[3] <= '9' && line[4] == '=';
}

void writeAppPref() {
    std::string path = appDir() + "/app.txt";
    std::vector<std::string> foreign;
    FILE* in = fopen(path.c_str(), "rb");
    if (in) {
        std::vector<char> lineBuf(128);
        while (readLine(in, lineBuf))
            if (lineBuf[0] && !linuxAppKey(lineBuf.data())) foreign.push_back(lineBuf.data());
        fclose(in);
    }
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return;
    for (const std::string& line : foreign) fprintf(fp, "%s\n", line.c_str());
    fprintf(fp, "tabstrip=%d\nautosave=%d\nhlcolor=%d\npincolor=%d\nnight=%d\n",
            gTabStrip ? 1 : 0, gAutosave ? 1 : 0, gHlDefault, gPinDefault, gNight ? 1 : 0);
    int x = 0, y = 0, w = 0, h = 0, mx = 0;
    ui_windowRect(&x, &y, &w, &h, &mx);
    if (w > 0 && h > 0) fprintf(fp, "winx=%d\nwiny=%d\nwinw=%d\nwinh=%d\nwinmax=%d\n", x, y, w, h, mx);
    for (int c = 0; c < kPalCustom; c++)
        if (gPalCustom[c] != CLR_INVALID)
            fprintf(fp, "pal%d=%02x%02x%02x\n", c, GetRValue(gPalCustom[c]), GetGValue(gPalCustom[c]), GetBValue(gPalCustom[c]));
    fprintf(fp, "palnext=%d\n", gPalNext);
    fclose(fp);
}
