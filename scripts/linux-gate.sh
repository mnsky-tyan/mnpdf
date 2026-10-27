#!/usr/bin/env bash
# mnpdf Linux functional test: drives the REAL binary on a virtual display and
# asserts on the app's own state files and on the pixels it paints.
#
# This is the Linux counterpart of the Windows PowerShell suites (test-*.ps1).
# It has two layers and runs BOTH:
#   1. the headless engine suite (tests/core-test.cpp, run as mnpdf-core-test),
#      which links the platform-independent core and drives the document engine
#      with no display;
#   2. the checks below, which drive the REAL binary on this virtual display and
#      assert on the pixels it paints and the state files it writes.
# It runs on Xvfb, so no user-desktop interference. Usage:
#   scripts/linux-gate.sh [path-to-arc.pdf]
set -uo pipefail
cd "$(dirname "$0")/.."

PDF="${1:-tests/arc.pdf}"
BIN=./build-linux/mnpdf
STATE=$(mktemp -d /tmp/mnpdf-linux-gate.XXXXXX)
trap 'rm -rf "$STATE"' EXIT
OUT=$STATE/shot.png
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; }
chk() { if [ "$1" = "0" ]; then ok "$2"; else bad "$2"; fi; }

if [ ! -x "$BIN" ]; then echo "gate: $BIN not built (run ./build-linux.sh)" >&2; exit 1; fi
echo "== layer 1: the headless engine suite =="
if [ ! -x "$BIN-core-test" ]; then
    # the gate may run before build-linux.sh has run cmake's test target; build
    # it once here so the suite is never silently missing
    bash ./build-linux.sh >/dev/null 2>&1
fi
if [ ! -x "$BIN-core-test" ]; then
    bad "the headless engine suite binary is missing (run ./build-linux.sh)"
else
    ST2=$(mktemp -d /tmp/mnpdf-gate-core.XXXXXX)
    if LD_LIBRARY_PATH=./build-linux timeout 300 "$BIN-core-test" "$PDF" "$ST2" ./tests \
         >"$ST2/log.txt" 2>&1; then
        ok "the headless engine suite passes ($(grep -o 'pass=[0-9]*' "$ST2/log.txt" | tail -1))"
        rm -rf "$ST2"
    else
        bad "the headless engine suite failed; log kept at $ST2/log.txt"
        tail -15 "$ST2/log.txt"
    fi
fi

if ! command -v xvfb-run >/dev/null; then echo "gate: xvfb-run missing (apt install xvfb)" >&2; exit 1; fi
if ! command -v xwininfo >/dev/null; then echo "gate: xwininfo missing (apt install x11-utils)" >&2; exit 1; fi
if ! command -v xprop >/dev/null; then echo "gate: xprop missing (apt install x11-utils)" >&2; exit 1; fi
if ! command -v curl >/dev/null; then echo "gate: curl missing (apt install curl)" >&2; exit 1; fi
# xdotool drives the menu/touchpad probes below with REAL X server events -
# the synthetic-event paths cannot test what the event mask filters.
if ! command -v xdotool >/dev/null; then echo "gate: xdotool missing (apt install xdotool)" >&2; exit 1; fi

echo "== launch, render, and write a pixel shot =="
D=$STATE/launch; mkdir -p "$D"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D" MNPDF_SHOT="$OUT" MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$D/log.txt" 2>&1
grep -qF -- "$OUT" "$D/log.txt"; chk $? "the app writes its own back buffer"
[ -s "$OUT" ]; chk $? "the shot file has content"
grep -q "Gtk-CRITICAL" "$D/log.txt"; [ $? -ne 0 ]
chk $? "the MNPDF_SHOT_EXIT quit path emits no Gtk-CRITICAL"

if ! command -v python3 >/dev/null; then
    bad "python3 missing (apt install python3 python3-pil) - pixel checks skipped"
fi
if [ -s "$OUT" ] && command -v python3 >/dev/null; then
    python3 - "$OUT" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
# the paper must dominate: a rendered page is mostly white
white = sum(1 for p in im.getdata() if p[0] > 240 and p[1] > 240 and p[2] > 240)
frac = white / (w * h)
print("       shot %dx%d white fraction %.3f" % (w, h, frac))
sys.exit(0 if frac > 0.15 else 1)
PY
    chk $? "the rendered page is mostly paper (a real render, not a black frame)"
    python3 - "$OUT" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
# the document area holds dark text: black-ish pixels exist well inside the page
w, h = im.size
dark = 0
for y in range(0, h, 3):
    for x in range(0, w, 3):
        r, g, b = im.getpixel((x, y))
        if r < 100 and g < 100 and b < 100: dark += 1
print("       dark sampled pixels: %d" % dark)
sys.exit(0 if dark > 200 else 1)
PY
    chk $? "the page carries text (dark pixels inside the paper)"
fi

echo "== state files: sidecar, prefs, last-document =="
D=$STATE/state; mkdir -p "$D"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
  timeout 30 "$BIN" "$PDF" >"$D/log.txt" 2>&1
CFG=$D/mnpdf
ls "$CFG" >/dev/null 2>&1; chk $? "the app creates its config directory"
[ -f "$CFG/last.txt" ]; chk $? "last.txt is written (the document a plain launch reopens)"
SC=$(ls "$CFG"/doc-*.txt 2>/dev/null | head -1)
[ -n "$SC" ]; chk $? "a per-document sidecar is written"
if [ -n "$SC" ]; then
    grep -q "^fmt=2$" "$SC"; chk $? "the sidecar carries the schema version (interop with the Windows app)"
    grep -q "^page=1$" "$SC"; chk $? "the sidecar records the current page"
    grep -qE "^zoom=[0-9]+\\.[0-9]+$" "$SC"; chk $? "the sidecar records the zoom"
    grep -q "^fit=1$" "$SC"; chk $? "the sidecar records fit-width mode"
fi
[ -f "$CFG/app.txt" ]; chk $? "app.txt (preferences) is written"
if [ -f "$CFG/app.txt" ]; then
    grep -q "^autosave=1$" "$CFG/app.txt"; chk $? "prefs record autosave on"
    grep -qE "^winw=[0-9]+$" "$CFG/app.txt"; chk $? "prefs record the window width"
fi

echo "== a second launch restores the same document =="
D=$STATE/restore; mkdir -p "$D"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
  timeout 30 "$BIN" "$PDF" >"$D/log.txt" 2>&1
FIRST=$(cat "$D/mnpdf/last.txt" 2>/dev/null | tr -d '\n')
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
  timeout 30 "$BIN" >"$D/log2.txt" 2>&1
SECOND=$(cat "$D/mnpdf/last.txt" 2>/dev/null | tr -d '\n')
# the second launch's own sidecar must exist for the remembered path, so a
# launch that crashed before restoring cannot pass on an unchanged file
ls "$D"/mnpdf/doc-*.txt >/dev/null 2>&1
RESTORED=$?
[ "$FIRST" = "$SECOND" ] && [ -n "$FIRST" ] && [ "$RESTORED" = "0" ]
chk $? "a plain launch reopens the remembered document and re-reads its sidecar"

echo "== a refused file never becomes a sidecar =="
D=$STATE/badpdf; mkdir -p "$D"
echo "this is not a pdf at all" >"$D/broken.pdf"
# the app refuses the file with a message box, exactly as the Windows build does;
# the suite drives the open without a clickable dialog, so the process is given a
# short timeout and is expected to still be alive at that point
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D" \
  timeout 4 "$BIN" "$D/broken.pdf" >"$D/log.txt" 2>&1
RC=$?
# the app refuses with a message box, so the process is expected to still be
# alive at the timeout; the refusal itself is asserted by the (fixed) check
# below, which is the guard that matters
[ "$RC" -eq 124 ] || [ "$RC" -eq 0 ]; chk $? "a refused document does not crash the app"
ls "$D"/mnpdf/doc-*.txt >/dev/null 2>&1
[ $? -ne 0 ]; chk $? "no sidecar is written for a file that never opened"

echo "== the Advanced cascade: thumbnails, outline, night mode =="
D=$STATE/advanced; mkdir -p "$D"
# MNPDF_CMD drives one command by its menu id, the way the Windows suites post
# WM_COMMAND, so a suite can assert on chrome the page back buffer never holds.
shot_for() {   # shot_for <id> <file> -> 0 on success
    local id=$1 out=$2
    rm -rf "$D/cfg$id"; mkdir -p "$D/cfg$id"
    xvfb-run -a --server-args="-screen 0 1280x900x24" \
      env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D/cfg$id" MNPDF_CMD="$id" \
          MNPDF_SHOT="$out" MNPDF_SHOT_WINDOW=1 MNPDF_SHOT_EXIT=1 \
      timeout 30 "$BIN" "$PDF" >"$D/log$id.txt" 2>&1
}
shot_for 202 "$D/thumbs.png"
[ -s "$D/thumbs.png" ]; chk $? "the thumbnails drawer opens (Advanced > Thumbnails)"
if [ -s "$D/thumbs.png" ] && command -v python3 >/dev/null; then
    # A preview is a whole page scaled down: white paper (255) with grey ink on
    # it, sitting on the drawer's own background (240). The test must not pass
    # on the drawer alone, so it requires PAPER rows carrying ink: a drawer
    # whose previews all failed to rasterize has background but no paper, and
    # a closed drawer has neither. Measured: 619 paper-with-ink rows open, 0
    # closed.
    python3 - "$D/thumbs.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
band = im.crop((0, 30, min(130, w), h))
bw, bh = band.size
px = band.load()
paper_rows = ink_rows = 0
for y in range(bh):
    paper = ink = 0
    for x in range(bw):
        m = min(px[x, y])
        if m >= 250:
            paper += 1
        else:
            ink += 1
    if paper > 60:            # this row is mostly page paper
        paper_rows += 1
        if ink > 3:           # ... and it carries something printed on it
            ink_rows += 1
# the drawer's own background: 240 grey, which the page beside it is not
grey = 0
for y in range(bh):
    for x in range(min(20, bw)):
        r, g, b = px[x, y]
        if abs(r - 240) < 4 and abs(g - 240) < 4 and abs(b - 240) < 4:
            grey += 1
print("       thumbnail paper rows: %d, of them with ink: %d, drawer grey: %d"
      % (paper_rows, ink_rows, grey))
# A closed drawer gives 0 paper rows AND 0 grey. The page alone gives paper rows
# but no drawer grey, so requiring both is what makes a missing drawer fail.
sys.exit(0 if (ink_rows > 200 and grey > 5000) else 1)
PY
    chk $? "the thumbnails drawer paints page previews (paper with ink on it)"
fi
shot_for 201 "$D/outline.png"
[ -s "$D/outline.png" ]; chk $? "the outline drawer opens (Advanced > Outline panel)"
if [ -s "$D/outline.png" ] && command -v python3 >/dev/null; then
    # The outline drawer is a light panel with dark bookmark text on it, so an
    # OPEN drawer has FEWER dark pixels in that band than the page alone: the
    # drawer covers the page. Measured: an open drawer gives ~9800 dark
    # pixels, a closed one ~192000 (the whole page sits in the band).
    #
    # So the check is that the band went LIGHT, not that it went dark. The
    # old check asked for dark pixels, which the closed drawer supplies in
    # abundance - it passed whether or not the drawer was open, and proved
    # nothing.
    python3 - "$D/outline.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
band = im.crop((max(0, w - 240), 0, w, h))
dark = sum(1 for p in band.getdata() if p[0] < 200 and p[1] < 200 and p[2] < 200)
print("       outline band dark pixels: %d" % dark)
# an open drawer: the band is mostly the drawer's light background
sys.exit(0 if dark < 60000 else 1)
PY
    chk $? "the outline drawer covers the page beside it"
fi

echo "== the drawer's selection follows the page that moved =="
# Windows re-points the ring inside its own movePage (src/main.cpp:4615-4620):
# the index the ring happens to hold now shows a different page, so a ring
# that stayed put would make the next move act on that one instead. The
# reader proves it by moving the page its own ring selects: the ring is a
# grey filled band (RGB 199,199,199) behind the selected preview, and the
# shot locates the band. Measured: with the ring on the first page the band
# starts at y=29; after the page moves down one, y=175 - a ring that stayed
# put would still be at 29.
M=$STATE/ringfollow; mkdir -p "$M/cfg/mnpdf"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$M/cfg" MNPDF_CMD=202,312 \
      MNPDF_SHOT="$M/follow.png" MNPDF_SHOT_WINDOW=1 MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$M/log.txt" 2>&1
if [ -s "$M/follow.png" ] && command -v python3 >/dev/null; then
python3 - "$M/follow.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
top = None
for y in range(im.height):
    cnt = sum(1 for x in range(10, 120)
              if abs(px[x, y][0] - 199) < 6 and abs(px[x, y][1] - 199) < 6
              and abs(px[x, y][2] - 199) < 6)
    if cnt >= 90:
        top = y
        break
print("       selection band top: %s" % top)
sys.exit(0 if top is not None and top > 100 else 1)
PY
    chk $? "the ring moved with the page it selected"
else
    bad "the ring-follow shot is missing"
fi

echo "== the frame renders at the device scale, not at 1x =="
# At a fractional desktop scale GTK3 reports 1, so the old build painted a 1x
# buffer that the compositor stretched - the "low resolution" report. The reader
# derives the scale from the screen's physical DPI, with MNPDF_SCALE as the
# documented override. Same screen, same window, one variable: the scale. A
# reader that honours it renders a buffer 1.6x larger; the old code ignored it
# and produced the same buffer size either way (measured: both 798 px, which is
# why this check compares ONE dpi with and without the override - comparing two
# screen DPIs proves nothing, the window itself grows with the screen).
S=$STATE/scale; mkdir -p "$S"
run_scale_probe() {
    rm -rf "$S/$1"; mkdir -p "$S/$1/cfg"
    xvfb-run -a --server-args="-screen 0 800x600x24 -dpi 96" \
      env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$S/$1/cfg" \
      ${2:+MNPDF_SCALE=$2} MNPDF_SHOT="$S/$1/shot.png" MNPDF_SHOT_EXIT=1 \
      timeout 40 "$BIN" "$PDF" >"$S/$1/log.txt" 2>&1
}
run_scale_probe base ""
run_scale_probe hi "1.6"
# The PAGE, not the buffer: a bigger buffer alone proves nothing (the window
# grows with the screen). The page is a fixed physical size, so at 1.6x it must
# paint 1.6x wider in the frame. Measure the white page's width on one scanline.
page_width() {
    python3 - "$1" <<'PY' 2>/dev/null
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
row = h // 2
px = im.load()
best = cur = 0
for x in range(w):
    r, g, b = px[x, row]
    if r > 240 and g > 240 and b > 240:
        cur += 1
        best = max(best, cur)
    else:
        cur = 0
print(best)
PY
}
W1=$(page_width "$S/base/shot.png")
W2=$(page_width "$S/hi/shot.png")
if [ -n "$W1" ] && [ -n "$W2" ] && [ "$W1" -gt 0 ]; then
    [ "$W2" -gt "$((W1 * 3 / 2))" ] && [ "$W2" -lt "$((W1 * 17 / 10))" ]
    chk $? "MNPDF_SCALE=1.6 paints the page 1.6x wider (default ${W1}px, scaled ${W2}px)"
else
    bad "the scale probe produced no measurable page (default '${W1:-none}', scaled '${W2:-none}')"
fi

echo "== the titlebar toggle, and it survives a relaunch =="
D=$STATE/titlebar; mkdir -p "$D"
rm -rf "$D/cfg"; mkdir -p "$D/cfg"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D/cfg" MNPDF_CMD=113 MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$D/log1.txt" 2>&1
grep -q "^titlebar=0$" "$D/cfg/mnpdf/app.txt"; chk $? "hiding the titlebar is recorded in app.txt"
# a relaunch must come back captionless, not with the default caption
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D/cfg" MNPDF_SHOT="$D/again.png" MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$D/log2.txt" 2>&1
grep -q "^titlebar=0$" "$D/cfg/mnpdf/app.txt"; chk $? "the captionless state survives a relaunch"
# The menu bar is built once, so its entry's wording is state the reader has to
# refresh: before this check the bar kept saying "Hide titlebar" after the
# window was already captionless, which is what a reader reads as "the toggle
# did nothing". The context menu is rebuilt per open and can never go stale, so
# the assertion reads the bar's own section of the dump. Drive the toggle and
# dump the menu in the same run.
rm -rf "$D/lbl"; mkdir -p "$D/lbl/cfg/mnpdf"
printf 'titlebar=1\ntabstrip=1\n' > "$D/lbl/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D/lbl/cfg" \
  MNPDF_MENU_DUMP=1 MNPDF_CMD=113 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$D/lbl/log.txt" 2>&1
grep -q "^titlebar=0$" "$D/lbl/cfg/mnpdf/app.txt"
chk $? "the titlebar toggle flips the stored state"
bar=$(awk '/^== menu bar ==$/{f=1; next} /^== /{f=0} f' "$D/lbl/log.txt")
echo "$bar" | grep -q "Show titlebar: 113" && ! echo "$bar" | grep -q "Hide titlebar: 113"
chk $? "and the menu bar's own entry says Show titlebar afterwards, not Hide titlebar"

echo "== the Windows app.txt field order is kept =="
D=$STATE/order; mkdir -p "$D"
# a hand-written app.txt with the Windows keys, including the ones Linux does
# not own, so the merge is exercised the way a shared directory exercises it
mkdir -p "$D/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\nupdcheck=1700000000\nupdtag=v2.6.0\n' > "$D/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$D/cfg" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
  timeout 30 "$BIN" "$PDF" >"$D/log.txt" 2>&1
# titlebar= must land in the Linux block (right after tabstrip=), and the
# foreign keys must be carried after it in their original order
python3 - "$D/cfg/mnpdf/app.txt" <<'PY'
import sys
lines = [l.strip() for l in open(sys.argv[1]) if l.strip()]
def idx(k):
    for i, l in enumerate(lines):
        if l.startswith(k):
            return i
    return -1
order = [idx("tabstrip="), idx("titlebar="), idx("autosave="), idx("updcheck="), idx("updtag=")]
print("       app.txt order: %s" % order)
sys.exit(0 if all(o >= 0 for o in order) and order == sorted(order) else 1)
PY
chk $? "app.txt keeps the Windows field order and carries the foreign keys"
grep -q "^updtag=v2.6.0$" "$D/cfg/mnpdf/app.txt"; chk $? "the Windows update keys survive a Linux run"

echo "== the shared-preference role: a second window writes nothing =="
# One app.txt is shared between the readers, so a second window must not
# overwrite the first one's preferences. The Windows build claims a named mutex
# for this (src/main.cpp:6138) and a secondary window writes neither app.txt
# nor last.txt (src/main.cpp:3276, 3344).
I=$STATE/instance; mkdir -p "$I/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$I/cfg/mnpdf/app.txt"
# the first instance holds the role for as long as it runs. It is given a
# lifetime of its own and waited for afterwards - a reader with no window to
# close would otherwise run forever. It is NOT killed: xvfb-run takes its X
# server down in an EXIT trap, which a signal does not run, so killing the
# wrapper would leave the server (and its temp directory) behind on every run.
# Letting it run out its own lifetime ends it the ordinary way, and the
# server goes with it.
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$I/cfg" timeout 12 "$BIN" "$PDF" >"$I/first.log" 2>&1 &
holder=$!
sleep 6
# the second one toggles the titlebar, which the primary would persist
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$I/cfg" MNPDF_CMD=113 MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$I/second.log" 2>&1
# the holder ends on its own timeout, a few seconds after the second one is
# done; waiting for it is what lets xvfb-run clean up after itself
wait $holder 2>/dev/null
grep -q "^titlebar=1$" "$I/cfg/mnpdf/app.txt"
chk $? "a second window does not overwrite the shared app.txt"

echo "== a plain second launch starts empty, and no second window writes the shared files =="
# README.txt's promise ("SEVERAL WINDOWS"): later windows "start empty and write
# none of them". Windows keeps it two ways: a plain second launch opens
# nothing (src/main.cpp:6241), and the two SHARED files are gated on the primary
# role - last.txt (src/main.cpp:3276) and app.txt (:3345). A document's SIDECAR
# is not one of them: it belongs to whichever window owns that document
# (:3243-3245), so a second window's marks must persist.
#
# Three halves, all against a live primary: the plain second launch maps an
# EMPTY window (titled "mnpdf", never "arc.pdf - mnpdf - page 1/13"); a second
# launch with a file argument opens its document; and that window's rotation
# reaches the document's own sidecar while the shared app.txt the section above
# covers stays the primary's.
E=$STATE/secondwin; mkdir -p "$E/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$E/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$E/cfg" timeout 22 "$BIN" "$PDF" >"$E/first.log" 2>&1 &
holder=$!
sleep 6
SC="$E/cfg/mnpdf/$(ls "$E/cfg/mnpdf" | grep '^doc-' | head -1)"
# a PLAIN second launch (no argument): its window must be the empty reader
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$E/cfg" \
  sh -c 'timeout 6 "$0" > "$1/plain.log" 2>&1 & sleep 5; \
         xwininfo -root -children 2>/dev/null | grep "mnpdf"' \
  "$BIN" "$E" > "$E/plain-names.txt" 2>&1
if grep -q "page 1/13" "$E/plain-names.txt"; then bad "a plain second launch reopened the primary's document"; else
    chk 0 "a plain second launch starts empty (no document window)"
fi
grep -q '"mnpdf".*800x600' "$E/plain-names.txt"
chk $? "its window is the empty reader"
# Windows ends a launch that opened nothing with the Open dialog
# (src/main.cpp:6251) - a bare second launch included, because it opens nothing
# by design (src/main.cpp:6244). The chooser is a toplevel with its own title,
# so the same dump names it.
grep -q '"Open PDF"' "$E/plain-names.txt"
chk $? "a plain second launch offers the Open dialog, like Windows"
# a second launch WITH a file argument opens it, and it writes THAT document's
# own sidecar - Windows gates only the two SHARED files on the primary role
# (last.txt src/main.cpp:3276, app.txt :3345); a document's sidecar belongs to
# whichever window owns it (:3243-3245), so a second window's marks must
# persist rather than be dropped at its exit. Proven by rotating in the second
# window: the rotation must reach the sidecar (the shared app.txt staying
# untouched is what the section above proves).
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$E/cfg" MNPDF_CMD=133 \
      MNPDF_SHOT="$E/second.png" MNPDF_SHOT_EXIT=1 \
  timeout 20 "$BIN" "$PDF" > "$E/file.log" 2>&1
SC="$E/cfg/mnpdf/$(ls "$E/cfg/mnpdf" | grep '^doc-' | head -1)"
grep -q '^rot=0,' "$SC" 2>/dev/null
chk $? "a second window keeps its own document's marks"
wait $holder 2>/dev/null

echo "== the autosave toggle cancels and re-arms the pending write =="
# Windows case 109 does not only flip the flag (src/main.cpp:5702-5711): it
# cancels the pending sidecar write when it turns autosave off, and re-arms
# the flush when it turns back on. The port's tick runs for the whole session,
# so the arm and the cancel are the pending flag. Both halves are proven by
# runs killed by timeout: no quit path may write the sidecar, so only the
# 800 ms tick can have written anything.
A=$STATE/autosave; mkdir -p "$A/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$A/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$A/cfg" MNPDF_CMD=133,109 \
      timeout 3 "$BIN" "$PDF" > "$A/off.log" 2>&1
SC="$A/cfg/mnpdf/$(ls "$A/cfg/mnpdf" | grep '^doc-' | head -1)"
[ -z "$(grep 'rot=' "$SC" 2>/dev/null)" ]
chk $? "autosave turned off cancels the write the change had armed"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$A/cfg/mnpdf/app.txt"
rm -f "$SC"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$A/cfg" MNPDF_CMD=109,133,109 \
      timeout 3 "$BIN" "$PDF" > "$A/on.log" 2>&1
SC="$A/cfg/mnpdf/$(ls "$A/cfg/mnpdf" | grep '^doc-' | head -1)"
grep -q '^rot=0,' "$SC" 2>/dev/null
chk $? "autosave turned back on flushes what changed while it was off"

echo "== the quit paths ask about unsaved changes, the way the Windows build does =="
# Windows routes every user quit through WM_CLOSE (menu Quit src/main.cpp:5715,
# the last tab :5325, Escape :5494), and WM_CLOSE is where the unsaved-changes
# prompt lives (src/main.cpp:6098-6112). The port routes the same three paths
# through its window's delete flow, so the prompt runs for them too. Proven by
# counting toplevels: the reader maps two Mnpdf toplevels alone (measured),
# and a modal is a third. A clean quit leaves none.
Q=$STATE/quitprompt; mkdir -p "$Q/dirty/mnpdf" "$Q/clean/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=0\nhlcolor=0\npincolor=5\nnight=0\n' > "$Q/dirty/mnpdf/app.txt"
cp "$Q/dirty/mnpdf/app.txt" "$Q/clean/mnpdf/app.txt"
# a change then menu Quit: the prompt must be up and the reader still alive
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$Q/dirty" MNPDF_CMD=133,112 \
  sh -c 'timeout 8 "$0" "$1" > "$2/log.txt" 2>&1 & sleep 5; \
         echo TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")' \
  "$BIN" "$PDF" "$Q/dirty" > "$Q/dirty/tops.txt" 2>&1
TOPS=$(sed -n 's/^TOPS //p' "$Q/dirty/tops.txt")
[ "$TOPS" = "3" ]
chk $? "quitting with unsaved changes asks first, and stays up (tops: $TOPS)"
# a clean doc then menu Quit: no prompt, and the reader is gone
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$Q/clean" MNPDF_CMD=112 \
  sh -c 'timeout 8 "$0" "$1" > "$2/log.txt" 2>&1 & sleep 5; \
         echo TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")' \
  "$BIN" "$PDF" "$Q/clean" > "$Q/clean/tops.txt" 2>&1
TOPS=$(sed -n 's/^TOPS //p' "$Q/clean/tops.txt")
[ "$TOPS" = "0" ]
chk $? "quitting a clean reader asks nothing and leaves (tops: $TOPS)"

echo "== the update check honours the cooldown =="
# The Windows build asks GitHub at most once an hour and stamps the attempt
# into app.txt (kUpdateCheckIntervalMin, src/main.cpp:67). A check inside that
# window must not make a request. The stamp this reader honours is its OWN
# pair (lnxupdcheck=/lnxupdtag=): updcheck=/updtag= are the Windows reader's
# stamp and mean the newest non-prerelease overall there, not the newest
# release carrying a Linux archive, so a fresh Windows stamp must NOT stop the
# Linux check from asking - that is the check below this one.
U=$STATE/updcheck; mkdir -p "$U/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\nlnxupdcheck=%s\nlnxupdtag=v2.6.0\n' \
    "$(date +%s)" > "$U/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$U/cfg" MNPDF_CMD=170 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$U/log.txt" 2>&1
# a cooled-down check must not have replaced the tag it read
if grep -q "releases\\.atom" "$U/log.txt"; then
    bad "a click inside the cooldown made a request"
else ok "a click inside the cooldown made no request"; fi
grep -q "^lnxupdtag=v2.6.0$" "$U/cfg/mnpdf/app.txt"
chk $? "an update check inside the hour does not ask again"

echo "== a Windows stamp is not this reader's cooldown =="
# One key, two meanings: updtag= is the Windows reader's cached answer (the
# newest non-prerelease overall, src/main.cpp:2435) and lnxupdtag= is this
# reader's (the newest release carrying a Linux archive). A Windows-written
# app.txt with a fresh updcheck= must not stop the Linux check from asking,
# and the Linux check must carry the Windows pair forward untouched instead
# of overwriting it with its own answer.
W=$STATE/updwin; mkdir -p "$W/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\nupdcheck=%s\nupdtag=v2.6.0\n' \
    "$(date +%s)" > "$W/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$W/cfg" MNPDF_CMD=170 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$W/log.txt" 2>&1
grep -q "releases\\.atom" "$W/log.txt"
chk $? "a fresh Windows stamp does not stop the Linux check"
grep -q "^lnxupdcheck=" "$W/cfg/mnpdf/app.txt"
chk $? "the Linux check stamps its own pair"
grep -q "^updtag=v2.6.0$" "$W/cfg/mnpdf/app.txt"
chk $? "and carries the Windows pair forward untouched"

# The cooled-down check answers with a modal, and the app quits while it is up:
# a modal is destroyed with its window, so the reader must not destroy it a
# second time. GTK answers that with a critical, which the log carries.
# Measured: before the guard, the log carried
#   gtk_widget_destroy: assertion 'GTK_IS_WIDGET (widget)' failed
#
# The same run also covers the quit path: a window destroyed under a modal
# must leave the app's own loop, not gtk_main_quit's, because gtk_main_quit
# leaves the INNERMOST loop and a modal runs one of its own. That answers
# "gtk_main_quit: assertion 'main_loops != NULL' failed".
if grep -q "Gtk-CRITICAL" "$U/log.txt"; then
    bad "the update answer's modal leaves a GTK critical behind"
    grep -m2 "Gtk-CRITICAL" "$U/log.txt" | sed 's/^/       /'
else ok "the update answer's modal leaves no GTK critical behind"; fi
echo "== the reader checks for updates at launch, silently =="
# The Windows build starts a silent check before its message loop
# (startUpdateCheck(false), src/main.cpp:6286): a reader is told about a
# newer release at launch, and nothing else - a check that found nothing to
# say, or that failed, is silent, and a launch inside the cooldown is silent
# even when the cached answer names a newer one (the cached answer is only
# surfaced by a click, because a click is the reader asking). Both halves are
# proven by RUNNING the reader and counting the toplevels it maps: a modal is
# a toplevel of its own, countable with xwininfo. Measured: the reader maps
# exactly two Mnpdf-classed toplevels - its window and GTK's 10x10 utility
# window - and a dialog makes a third.
L=$STATE/updlaunch; mkdir -p "$L/one/mnpdf" "$L/two/mnpdf"
# run one: no stamp, no click, no seam. The launch check must ask on its own
# and stamp what it found. The run outlives the check's own budget
# (kUpdateCheckBudgetSec, src/linux/update.cpp:74): a reader killed first would
# look like a check that found nothing.
NOW=$(date +%s)
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$L/one" \
  sh -c 'timeout 30 "$0" "$1" > "$2/log.txt" 2>&1 & sleep 26; \
         if grep -q "update check: outcome newer" "$2/log.txt"; then echo outcome-newer-already-in-log; fi; \
         echo TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")' \
  "$BIN" "$PDF" "$L/one" >"$L/one/tops.txt" 2>&1
TOPS=$(sed -n 's/^TOPS //p' "$L/one/tops.txt")
# The silence is conditional on the outcome: a check that found nothing newer,
# or could not ask, says nothing, while a check that found a newer release
# must open the update dialog - a third toplevel. The outcome the check
# reached is in the run's own log, so the assertion reads it instead of
# assuming today's answer.
if grep -q 'update check: outcome newer' "$L/one/log.txt"; then
    # the count is taken at 26 s while the check's own budget is 20 s, so a
    # check the machine slowed past the count cannot have opened its dialog
    # yet, and the count stays 2. The run itself records which case it was in:
    # it looks for the outcome line BEFORE the count, in the same subshell, and
    # prints a marker when it was already there. That order is what makes the
    # marker mean something - the reply handler prints the outcome line and
    # opens the dialog in the same callback, so a line that predates the count
    # means the dialog had to be mapped before the count ran, and the count must
    # be 3. A marker that is missing means the answer landed at or after the
    # grep, so it may have opened its dialog in the moment between the grep and
    # the xwininfo read, and either count is honest. The file's mtime cannot
    # answer this: any later write to log.txt moves it.
    if grep -q '^outcome-newer-already-in-log$' "$L/one/tops.txt"; then
        [ "$TOPS" = "3" ]
        chk $? "a launch whose check found a newer release offers it (tops: $TOPS)"
    else
        [ "$TOPS" = "2" ] || [ "$TOPS" = "3" ]
        chk $? "a launch whose check answered at or after the count cannot be counted yet (tops: $TOPS)"
    fi
else
    [ "$TOPS" = "2" ]
    chk $? "a launch whose check found nothing to say, or could not ask, says nothing (tops: $TOPS)"
fi
# a host that cannot reach GitHub is not a code failure: the silence above is
# the behaviour under test, and the stamp only exists when the check answered.
# The reader prints the outcome it reached on stdout ("update check: outcome
# current <tag>" / "newer <tag>", or "outcome error" plus the reason) whether
# or not it opens a dialog - a silent launch check says nothing to the reader,
# so the dialog is not where a suite can read it.
if grep -q 'update check: error The update check could not reach GitHub' "$L/one/log.txt"; then
    ok "the launch check could not reach GitHub from here - the stamp parts are untested"
elif grep -q 'update check: outcome error' "$L/one/log.txt"; then
    bad "the launch check failed for a reason other than the network (see $L/one/log.txt)"
else
    grep -q '^lnxupdcheck=' "$L/one/mnpdf/app.txt"
    chk $? "the launch check asks on its own, with no click"
    STAMP=$(sed -n 's/^lnxupdcheck=//p' "$L/one/mnpdf/app.txt" | head -1)
    [ -n "$STAMP" ] && [ "$STAMP" -ge "$NOW" ]
    chk $? "and stamps the attempt it made"
fi
# run two: a fresh stamp whose cached answer names a NEWER release. The
# launch must stay silent - no modal, no re-stamp, no request.
SEEDED=$(date +%s)
printf 'lnxupdcheck=%s\nlnxupdtag=v9.9.9\n' "$SEEDED" > "$L/two/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 800x600x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$L/two" \
  sh -c 'timeout 30 "$0" "$1" > "$2/log.txt" 2>&1 & sleep 26; \
         echo TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")' \
  "$BIN" "$PDF" "$L/two" >"$L/two/tops.txt" 2>&1
TOPS=$(sed -n 's/^TOPS //p' "$L/two/tops.txt")
[ "$TOPS" = "2" ]
chk $? "a launch inside the cooldown says nothing, even with a newer release cached (tops: $TOPS)"
grep -q "^lnxupdcheck=$SEEDED\$" "$L/two/mnpdf/app.txt"
chk $? "and spends no request doing it"

echo "== two update clicks in a row make one request =="
# The reader refuses a second check while the first one is still in flight,
# the way the Windows build does before it spawns anything (src/main.cpp:3188).
# The app prints the URL of every request it makes, so the feed it asked for
# is the evidence. MNPDF_NO_UPDATE skips the launch check (a clicked check runs
# regardless), so the only requests that can appear are the clicks' own - one
# of the two must reach the network and the other must be refused in flight,
# whichever way the timing falls.
T=$STATE/updtwice; mkdir -p "$T/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' \
    > "$T/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$T/cfg" MNPDF_CMD=170,170 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$T/log.txt" 2>&1
FEED=$(grep -c "releases\.atom" "$T/log.txt")
[ "$FEED" = "1" ]
chk $? "a second click inside the request's own window makes no second request"

echo "== the update check finds the release that carries the Linux archive =="
# The check has to answer one question: which release carries the Linux
# archive? The Windows build reads the tag off the web host, never
# api.github.com, because that host's unauthenticated quota is shared with
# every other client behind the same IP and answers "API rate limit exceeded"
# once it is spent (src/main.cpp:2430-2432). The port must not inherit that
# problem.
#
# The check below RUNS the reader rather than reading its source: the reader
# is told to check for updates, and the tag it found is compared with the
# release that really carries the archive. A check that greps the source can
# only prove the source says something; this one proves the reader says it.
# The launch check may answer before the click reaches the network - the
# stamp names the release either way.
V=$STATE/updlive; mkdir -p "$V/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' \
    > "$V/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$V/cfg" MNPDF_CMD=170 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$V/log.txt" 2>&1
# the tag the check stamped is the newest release that carries the archive.
# GitHub is outside this machine's control, so a run that could not reach it
# says so instead of blaming the code: the reader's own answer (found / not
# found) is only meaningful when the host answered at all.
FOUND=$(sed -n 's/^lnxupdtag=//p' "$V/cfg/mnpdf/app.txt" | head -1)
UNREACHED=0
# a request was opened, and the answer was not a host that failed to answer
grep -q 'update check: GET' "$V/log.txt" || UNREACHED=1
grep -q 'update check: error The update check could not reach GitHub' "$V/log.txt" && UNREACHED=1
if [ "$UNREACHED" = "1" ]; then
    ok "the update check could not reach GitHub from here - the network parts are untested"
elif [ -n "$FOUND" ]; then
    ok "the update check found a release ($FOUND)"
else
    bad "the update check found no release"
fi
# and that release really carries the archive: the download URL answers 302
# when the file is there and 404 when it is not
if [ -n "$FOUND" ] && [ "$UNREACHED" = "0" ]; then
    CODE=$(curl -s -o /dev/null -w '%{http_code}' -I \
        "https://github.com/mnsky-tyan/mnpdf/releases/download/$FOUND/mnpdf-linux-x64-$FOUND.tar.gz")
    if [ "$CODE" = "302" ] || [ "$CODE" = "200" ]; then
        ok "the release it found really carries the Linux archive"
    elif [ "$CODE" = "000" ]; then
        ok "could not ask the download URL from here (HTTP $CODE) - untested"
    else
        bad "the release it found ($FOUND) does not carry the archive (HTTP $CODE)"
    fi
fi
# and the reader asked the web host, not the API: the log carries the URL each
# request actually opened, so this is what the reader did rather than what its
# source says. The web host is required because api.github.com's unauthenticated
# quota is shared and answers 403 once it is spent.
if grep -q 'https://github.com/mnsky-tyan/mnpdf/releases' "$V/log.txt" \
     && ! grep -q 'api\.github\.com' "$V/log.txt"; then
    ok "the update check asks the web host, not the API"
else
    bad "the update check did not ask the web host (see $V/log.txt)"
fi

echo "== the command ids match the Windows command table =="
# The parity claim is "the same command under the same menu id". The
# authoritative table is tests/lib.ps1 (the Windows suites' CMD_* constants),
# which is byte-identical on both platforms, so the ids are read from there
# rather than from a regex over src/main.cpp: the AppendMenuW calls spell their
# flags as expressions, so most do not match a literal pattern and such a check
# would be vacuous.
E=$STATE/ids; mkdir -p "$E/cfg/mnpdf"
# The menu has shapes: a pin under the cursor, a highlight, a live selection,
# and neither, and the mark-specific commands exist only in their own shape. A
# sidecar seeds one highlight and one pin so every shape is built. The name is
# the FNV-1a hash of the fixture's path, which the run below prints if it ever
# changes.
seed_marks() {
    local hash=$1
    printf 'fmt=2\nzoom=1.5\nfit=1\npage=1\nhl=0,3,28,1\npin=1,120.00,300.00,c5,a note\n' \
        > "$E/cfg/mnpdf/doc-$hash.txt"
    # a custom palette slot makes the "Clear custom colours" entry (180) exist;
    # without one the submenu correctly omits it, so the id would look missing
    printf 'pal0=336699\n' >> "$E/cfg/mnpdf/app.txt"
}
# ask the app for the hash it actually uses, so the seed cannot drift
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$E/cfg" MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >/dev/null 2>&1
hash=$(ls "$E/cfg/mnpdf/" 2>/dev/null | sed -n 's/^doc-\(.*\)\.txt$/\1/p' | head -1)
[ -n "$hash" ] || { echo "gate: could not learn the sidecar hash" >&2; exit 1; }
seed_marks "$hash"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$E/cfg" MNPDF_MENU_DUMP=1 \
  timeout 30 "$BIN" "$PDF" >"$E/menu.txt" 2>&1
[ -s "$E/menu.txt" ]; chk $? "the menu dump runs"
python3 - "$E/menu.txt" "$PWD/tests/lib.ps1" <<'PY'
import re, sys
menu = open(sys.argv[1], encoding="utf-8", errors="replace").read()
lib = open(sys.argv[2], encoding="utf-8", errors="replace").read()
# the Windows table: ${CMD_NAME} = 113   # comment
table = {}
for m in re.finditer(r'\$\{(CMD_[A-Z0-9_]+)\}\s*=\s*(\d+)', lib):
    table[m.group(1)] = int(m.group(2))
# what the Linux menu actually carries: "label: id", and " [greyed]" after the
# id when the entry is there but cannot run. A greyed entry still carries its
# id - it is in the menu - so the id scan has to accept the marker.
seen = set()
for line in menu.splitlines():
    m = re.match(r'\s*.*?: (\d+)(?: \[greyed\])?\s*$', line)
    if m:
        seen.add(int(m.group(1)))
# The commands both readers must carry, by the name the Windows table gives
# them. Linux-only entries (the page surgery at 310-312) are not listed and are
# not forbidden. CMD_COPY_HL_TEXT is here because a highlight's "Copy text" must
# copy the highlight, a different command from copying the selection.
required = ["CMD_TITLEBAR", "CMD_UNDO", "CMD_REDO", "CMD_SAVE", "CMD_SAVE_AS",
            "CMD_COPY_HL_TEXT", "CMD_DELETE_HL", "CMD_ROTATE_CW", "CMD_ROTATE_CCW",
            "CMD_ADD_PIN", "CMD_EDIT_PIN", "CMD_DELETE_PIN",
            "CMD_PRINT", "CMD_CHECK_UPDATES", "CMD_CLEAR_CUSTOM",
            "CMD_NIGHT", "CMD_OUTLINE", "CMD_THUMBS", "CMD_MERGE", "CMD_SPLIT",
            "CMD_INSERT_SIG", "CMD_CLEAR_SIGS", "CMD_REOPEN_LAST",
            "CMD_OPEN", "CMD_QUIT", "CMD_MINIMIZE", "CMD_MAX_RESTORE",
            "CMD_HIDE_TABSTRIP", "CMD_NEW_TAB", "CMD_CLOSE_TAB",
            "CMD_ZOOM_IN", "CMD_ZOOM_OUT", "CMD_FIT_WIDTH", "CMD_FIND",
            "CMD_AUTOSAVE",
            # the pin colour family, which the pin shape makes visible: presets
            # at 160-165 and "Custom..." at 169. The dump only sees these
            # because buildColorSubmenu records the id too.
            "CMD_CUSTOM_PIN_COLOR",
            # the selection family, which the selection shape makes visible:
            # Copy (101), Highlight (135) and the default-highlight colour
            # family (140-149).
            "CMD_COPY", "CMD_HIGHLIGHT", "CMD_DEFAULT_HL_YELLOW",
            "CMD_CUSTOM_HL_COLOR"]
# The dump builds every menu shape, so the commands that need a state of their
# own are visible anyway: the selection shape carries Copy (101), Highlight
# (135) and the default-highlight family (140-149), the highlight shape carries
# the highlight's own Copy text (116), and 180 (clear custom colours) needs a
# custom colour to exist, which the seed above provides.
missing = []
for name in required:
    if name not in table:
        missing.append("%s is not in tests/lib.ps1" % name)
    elif table[name] not in seen:
        missing.append("%s (%d) is a Windows command with no Linux menu entry"
                       % (name, table[name]))
# The menu bar is dumped as its own section. Its entries post commands, so
# every one of them carries the id it posts - the menu holders (File, View,
# Advanced) post nothing and carry none.
bar = []
inbar = False
for line in menu.splitlines():
    if line.startswith("== "):
        inbar = (line.strip() == "== menu bar ==")
        continue
    if inbar and line.strip():
        bar.append(line)
noId = [l for l in bar if l.startswith(" ") and not re.search(r': \d+(?: \[greyed\])?$', l)]
print("       menu bar entries: %d, command entries without an id: %d"
      % (len(bar), len(noId)))
for l in noId:
    missing.append("the menu bar's %r carries no command id" % l.strip())
print("       windows ids carried by the menu: %d of %d"
      % (sum(1 for n in required if n in table and table[n] in seen), len(required)))
for m in missing:
    print("       " + m)
sys.exit(1 if missing else 0)
PY
chk $? "the menu carries the Windows command ids (tests/lib.ps1)"
# The two page targets a right-click computes must name the SAME page once the
# view is scrolled: the pin's page is computed in buildContextMenu and the
# rotate capture through charIndexAtDoc, so a coordinate-space slip in one and
# not the other is invisible while the view sits at the top (both read page 1)
# and silently misplaces the pin once it does not. The dump prints both for one
# click after scrolling away from the top, and prints how far that click's view
# really moved, so the agreement below cannot be a vacuous one: a dump that ran
# before the first size allocation has a zero scroll delta and every target
# reading page 1, which must fail here rather than pass.
SCROLLD=$(sed -n 's/^== scrolled click (scroll delta \([0-9-]*\)) ==$/\1/p' "$E/menu.txt" | head -1)
PINP=$(sed -n 's/^pin-page //p' "$E/menu.txt" | head -1)
ROTP=$(sed -n 's/^rotate-page //p' "$E/menu.txt" | head -1)
HITP=$(sed -n 's/^hit-page //p' "$E/menu.txt" | head -1)
VIEWP=$(sed -n 's/^view-page //p' "$E/menu.txt" | head -1)
[ -n "$SCROLLD" ] && [ "$SCROLLD" -gt 0 ] && [ -n "$PINP" ] && [ "$PINP" = "$ROTP" ] \
  && [ "$ROTP" = "$HITP" ] && [ "$((PINP + 1))" = "$VIEWP" ] && [ "$VIEWP" -ge 2 ]
chk $? "a right-click on a scrolled page names one page for all three targets (scrolled $SCROLLD, pin $PINP, rotate $ROTP, hit $HITP, view $VIEWP)"

# The wording is the Windows menu's, accelerator hint included (src/main.cpp:5530,
# :5542-5557). The dump prints the label the reader sees, so this asserts the
# words the reader is shown rather than what the source says.
missing=""
while IFS= read -r want; do
    [ -z "$want" ] && continue
    grep -qxF "$want" "$E/menu.txt" || grep -qxF "$want [greyed]" "$E/menu.txt" \
        || missing="$missing [$want]"
done <<'WANT'
Open...	Ctrl+O: 107
New Tab	Ctrl+T: 300
Close Tab	Ctrl+W: 301
Save	Ctrl+S: 118
Save As...	Ctrl+Shift+S: 119
Print...	Ctrl+P: 137
Undo	Ctrl+Z: 114
Redo	Ctrl+Y: 115
Find...	Ctrl+F: 103
Fit Width	Ctrl+0: 104
Zoom In	+: 105
Zoom Out	-: 106
Copy	Ctrl+C: 101
WANT
[ -z "$missing" ]
chk $? "the menu words its entries the way the Windows menu does${missing:+ (missing:$missing)}"

# The greying, asserted rather than assumed: Windows greys a command out when
# it cannot run (src/main.cpp:5543-5560), and the port greyed NOTHING - every
# entry was live whether or not it could act. The dump marks a greyed entry
# " [greyed]", so the two states are visible to a check.
#
# These read the ID, not the label: whether a command can run is a fact about
# the command and the state, so a wording change must not be able to turn one
# of these into a check that can no longer fail.
greyed() { grep -qE ": $1 \[greyed\]$" "$2"; }
# The run above seeds a highlight and a pin, so it is DIRTY by design: restored
# marks are unsaved work (openPath marks the document dirty when the sidecar
# carried marks the file does not have). That is the state Windows answers with
# Save live and the undo stack empty, so those are the assertions here.
greyed 114 "$E/menu.txt"
chk $? "Undo is greyed out with an empty undo stack"
greyed 115 "$E/menu.txt"
chk $? "Redo is greyed out with an empty redo stack"
greyed 301 "$E/menu.txt"
chk $? "Close Tab is greyed out with one tab (src/main.cpp:5544)"
stuck=""
for id in 118 119 137 133; do
    greyed "$id" "$E/menu.txt" && stuck="$stuck $id"
done
[ -z "$stuck" ]
chk $? "Save, Print and the rotations stay live on a document with unsaved marks${stuck:+ (greyed:$stuck)}"
# The other half needs the state the dump run cannot be in: a CLEAN document.
# Save and Save As are dead then (nothing to save), which is the Windows
# predicate gDoc && gDirty. A menu that greyed everything would fail the block
# above; one that greyed nothing fails here.
C=$STATE/clean; mkdir -p "$C/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$C/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$C/cfg" MNPDF_MENU_DUMP=1 \
  timeout 30 "$BIN" "$PDF" >"$C/menu.txt" 2>&1
greyed 118 "$C/menu.txt"
chk $? "Save is greyed out on a clean document (src/main.cpp:5545)"
greyed 119 "$C/menu.txt"
chk $? "Save As is greyed out on a clean document"
grep -qE ": 137$" "$C/menu.txt"
chk $? "Print stays live on a clean document with a document open"


echo
echo "== the input fixes: menu opens on release, smooth-scroll mask =="
# Reproductions for two user reports, driven with REAL X events (xdotool),
# because both defects live in the layer synthetic events bypass: the event
# mask and the grab that decides what a button release means.
#
# The old menu popped on button-PRESS, and GTK exempts the release that
# follows only within a short time window - a right-click held a fraction of a
# second (the normal gesture) put a menu item under the released button, the
# item fired, and the menu closed itself. The menu now pops on the RELEASE
# (Windows opens its context menu on WM_CONTEXTMENU, which is the release).
# The touchpad fix is the GDK_SMOOTH_SCROLL_MASK the area never selected:
# without it the backends fold smooth deltas into coarse discrete jumps and
# the proportional path in onScroll never ran.
R=$STATE/input; mkdir -p "$R/cfg"
xvfb-run -a --server-args="-screen 0 1100x800x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$R/cfg" MNPDF_TRACE_SCROLL=1 MNPDF_NO_UPDATE=1 \
  sh -c '
    "$0" "$1" > "$2" 2>&1 &
    APP=$!
    # the main window is the big Mnpdf-classed toplevel
    MAIN=
    i=0
    while [ -z "$MAIN" ] && [ $i -lt 40 ]; do
      MAIN=$(xwininfo -root -tree | awk "/arc.pdf/ {print \$1; exit}" | tr -d ":")
      [ -n "$MAIN" ] || sleep 0.2
      i=$((i+1))
    done
    [ -n "$MAIN" ] || { echo GESTURE-TOPS 0; echo WHEEL-SCROLLY -1; exit 0; }
    xdotool mousemove 550 400; sleep 0.3
    # the gesture that closed the old menu: press, hold, move, release
    xdotool mousedown 3; sleep 0.35
    xdotool mousemove 565 290; sleep 0.25
    xdotool mouseup 3; sleep 0.5
    echo GESTURE-TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")
    # an outside click must dismiss it
    xdotool mousemove 120 720; sleep 0.2
    xdotool click 1; sleep 0.5
    echo AFTER-CLICK-TOPS $(xwininfo -root -children 2>/dev/null | grep -c "\"Mnpdf\"")
    # a real wheel: discrete DOWN events the trace logs with the offset
    xdotool mousemove 550 400; sleep 0.2
    for i in 1 2 3 4; do xdotool click 5; sleep 0.12; done
    sleep 0.5
    echo WHEEL-SCROLLY $(grep -o "scrollY=[0-9-]*" "$2" | tail -1 | cut -d= -f2)
    echo EV-MASK $(grep -o "evmask=0x[0-9a-f]*" "$2" | head -1)
    # and a BURST the way a touchpad delivers: many notches with no pause, so
    # the reader has to either render each one (the lag) or coalesce them. The
    # counts below are what the fix is judged on.
    echo PRE-EVENTS $(grep -c "scroll: dir=" "$2")
    echo PRE-RENDERS $(grep -c "render took" "$2")
    echo PRE-TILES $(grep -c "tile-render" "$2")
    # one xdotool process with no delay: a per-notch xdotool spawn costs ~4 ms,
    # which lets the idle run between notches and coalesces nothing. This form
    # queues the whole burst, the way a touchpad driver does.
    xdotool click --repeat 16 --delay 0 5
    sleep 1.2
    echo POST-EVENTS $(grep -c "scroll: dir=" "$2")
    echo POST-RENDERS $(grep -c "render took" "$2")
    echo POST-TILES $(grep -c "tile-render" "$2")
    echo BURST-FINAL $(grep -o "render took.*scrollY=[0-9-]*" "$2" | tail -1 | grep -o "scrollY=[0-9-]*" | cut -d= -f2)
    echo BURST-LASTY $(grep -o "scrollY=[0-9-]*" "$2" | tail -1 | cut -d= -f2)
    kill $APP 2>/dev/null
  ' "$BIN" "$PDF" "$R/app.log" > "$R/out.txt" 2>&1
GT=$(sed -n 's/^GESTURE-TOPS //p' "$R/out.txt")
[ "$GT" = "3" ]
chk $? "the right-click menu survives press+hold+move+release (tops: $GT)"
AT=$(sed -n 's/^AFTER-CLICK-TOPS //p' "$R/out.txt")
[ "$AT" = "2" ]
chk $? "and an outside click dismisses it (tops: $AT)"
WS=$(sed -n 's/^WHEEL-SCROLLY //p' "$R/out.txt")
[ -n "$WS" ] && [ "$WS" -gt 100 ] 2>/dev/null
chk $? "a real wheel scrolls the view (scrollY after 4 notches: ${WS:-none})"
EM=$(sed -n 's/^EV-MASK evmask=//p' "$R/out.txt" | head -1)
# GDK_SMOOTH_SCROLL_MASK = 1 << 23 = 0x800000 (GDK_SCROLL_MASK is 1 << 21);
# the old mask printed 0x200706 - scroll without smooth - and a touchpad's
# deltas never reached the handler raw.
[ -n "$EM" ] && [ $(( EM & 0x800000 )) -ne 0 ]
chk $? "the drawing area selects GDK_SMOOTH_SCROLL_MASK at runtime (evmask=${EM:-none})"

# 3. a burst of notches does NOT render once per notch. Before the fix every
#    scroll event rendered the visible pages from PDFium's vector data (~4 ms
#    each at this window's device scale), so a touchpad burst saturated the
#    main loop - the "very laggy when i scroll" report. Measured: 16 notches
#    render 4 times, and each page's bitmap is rasterized once, not 16 times.
PE=$(sed -n 's/^PRE-EVENTS //p' "$R/out.txt"); PO=$(sed -n 's/^POST-EVENTS //p' "$R/out.txt")
PR=$(sed -n 's/^PRE-RENDERS //p' "$R/out.txt"); PP=$(sed -n 's/^POST-RENDERS //p' "$R/out.txt")
PT=$(sed -n 's/^PRE-TILES //p' "$R/out.txt"); PS=$(sed -n 's/^POST-TILES //p' "$R/out.txt")
BE=$((PO - PE)); BR=$((PP - PR)); BT=$((PS - PT))
[ -n "$BE" ] && [ "$BE" -ge 12 ] 2>/dev/null
chk $? "the burst probe really delivered its notches (events: ${BE:-none})"
[ -n "$BR" ] && [ "$BR" -le 6 ] 2>/dev/null
chk $? "a ${BE}-notch burst renders at most 6 frames (renders: ${BR:-none}, was 16 before the fix)"
[ -n "$BT" ] && [ "$BT" -ge 1 ] && [ "$BT" -le 4 ] 2>/dev/null
chk $? "and each page's bitmap is rasterized once, not per notch (tile renders: ${BT:-none})"
BF=$(sed -n 's/^BURST-FINAL //p' "$R/out.txt")
BL=$(sed -n 's/^BURST-LASTY //p' "$R/out.txt")
[ -n "$BF" ] && [ "$BF" = "$BL" ]
chk $? "the coalesced frame paints the burst's final position (final ${BF:-none}, last event ${BL:-none})"

echo
echo "== the icon: rounded at the brand radius =="
REPO=$(cd "$(dirname "$0")/.." && pwd)
# The icon is clipped to the mn-brand card (crop, square-fit, rounded alpha
# mask at r = 43/277 of the edge - mnbrand docs/icon-corner-radius.md). The
# square full-board icon this replaces showed the board's flat #ebe7e3
# background to the corners of every tile, which is the "make the corner
# rounded" report. These checks pin the SHIPPED artifacts, not the generator's
# good intentions: mnpdf.png is what the window, the launcher and the tarball
# carry, mnpdf.ico is what Windows embeds.
python3 - "$REPO/mnpdf.png" <<'PY' > "$STATE/icon.txt" 2>&1 || true
import math, sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
w, h = im.size
px = im.load()
print("size %dx%d" % (w, h))
corners = [px[0,0][3], px[w-1,0][3], px[0,h-1][3], px[w-1,h-1][3]]
print("corners", corners)
r = w * 43 / 277.0
# the card's first row: y0 = (h - card_h)//2; find it as the first row with
# any opaque pixel (the transparent padding above the card is 0)
y0 = next((y for y in range(h) if any(px[x,y][3] > 0 for x in range(0, w, 4))), -1)
fx = next((x for x in range(w) if px[x,y0][3] > 0), -1)
print("y0", y0, "first-opaque-x", fx, "r", round(r,1))
# expected: the arc starts at ~r - sqrt(2*r*0.875) at the first row
PY
grep -q "^size 512x512$" "$STATE/icon.txt"
chk $? "mnpdf.png is the 512x512 artwork"
grep -q "^corners \[0, 0, 0, 0\]$" "$STATE/icon.txt"
chk $? "the icon's four corners are transparent (rounded, not square)"
FX=$(sed -n 's/^y0 .*first-opaque-x //p' "$STATE/icon.txt" | cut -d' ' -f1)
R=$(sed -n 's/.* r //p' "$STATE/icon.txt")
if [ -n "$FX" ] && [ "$FX" -ge 64 ] && [ "$FX" -le 72 ] 2>/dev/null; then
    ok "the corner arc starts at the brand radius (first opaque x=$FX, r=$R)"
else
    bad "the corner arc is wrong (first opaque x=${FX:-none}, expected ~$R)"
fi
# and the Windows icon carries the same shape: its 256px entry must be rounded
python3 - "$REPO/mnpdf.ico" <<'PY' > "$STATE/ico.txt" 2>&1 || true
import struct, sys
from PIL import Image
d = open(sys.argv[1], "rb").read()
n = struct.unpack("<H", d[4:6])[0]
print("entries", [(d[6+i*16] or 256, d[7+i*16] or 256) for i in range(n)])
im = Image.open(sys.argv[1])
print("largest", im.size, "corner-alpha", im.convert("RGBA").load()[0,0][3])
PY
grep -q "^entries \[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)\]$" "$STATE/ico.txt"
chk $? "mnpdf.ico carries its six sizes (16..256)"
grep -q "^largest (256, 256) corner-alpha 0$" "$STATE/ico.txt"
chk $? "the ico's 256px entry is rounded too (transparent corner)"

echo
echo "== the release plumbing this branch is trusted for =="
# These checks exist because six release/packaging commits once reached main
# without the pipeline's reviewer, and a fresh review found a partial-match
# version check, an installer that installed no icons, and a gate that reported
# ok when a tool was missing. Each is locked here so the regression cannot come
# back silently.

# 1. The contract script release.yml runs rejects a partial version. It lives
#    in one place, so this executes the same rule the workflow does against
#    synthetic fixtures - no dependency on the tree's current version.
CONTRACT=$STATE/contract; mkdir -p "$CONTRACT/src"
printf 'static const wchar_t* const kAppVersion = L"9.9.9-rc1";\n' > "$CONTRACT/src/main.cpp"
printf 'mnpdf v9.9.9-rc1\n' > "$CONTRACT/README.txt"
"$REPO/scripts/check-release-contract.sh" "$CONTRACT" v9.9.9 >/dev/null 2>&1
[ $? -ne 0 ]
chk $? "the contract rejects kAppVersion and README v9.9.9-rc1 against tag v9.9.9"
printf 'static const wchar_t* const kAppVersion = L"9.9.9";\n' > "$CONTRACT/src/main.cpp"
"$REPO/scripts/check-release-contract.sh" "$CONTRACT" v9.9.9 >/dev/null 2>&1
[ $? -ne 0 ]
chk $? "the contract rejects README v9.9.9-rc1 against kAppVersion 9.9.9"
printf 'mnpdf v9.9.9 - Windows x64 portable release\n' > "$CONTRACT/README.txt"
"$REPO/scripts/check-release-contract.sh" "$CONTRACT" v9.9.9 >/dev/null
chk $? "the contract accepts an agreeing tag, kAppVersion and README"
"$REPO/scripts/check-release-contract.sh" "$REPO" >/dev/null
chk $? "the real src/main.cpp and README.txt satisfy the release contract"

# 2. The installer installs icons even with python3 present and no Pillow.
#    The broken version tested for python3 and then wrote nothing.
INS=$STATE/installer; mkdir -p "$INS/app"
cp "$BIN" "$INS/app/mnpdf" 2>/dev/null || cp "$REPO/build-linux/mnpdf" "$INS/app/mnpdf"
cp "$REPO/build-linux/libpdfium.so" "$INS/app/libpdfium.so" 2>/dev/null || true
cp "$REPO/mnpdf.png" "$INS/app/mnpdf.png"
cp "$REPO/scripts/install-menu-entry.sh" "$INS/app/install-menu-entry.sh"
chmod +x "$INS/app/install-menu-entry.sh"
# XDG_DATA_HOME alone redirects the installer (it reads
# ${XDG_DATA_HOME:-$HOME/.local/share} for both the entry and the icons), so the
# script runs UNMODIFIED from a directory that looks like an unpacked tarball.
#
# The no-Pillow case is REAL, not hoped for: a python3 shim first on PATH makes
# `from PIL import Image` fail, exactly as on a stock desktop. Without the shim
# this machine's own Pillow takes the good path and the check proves nothing -
# that is how the broken installer's check once "passed".
NOPIL=$STATE/noPIL; mkdir -p "$NOPIL/bin"
printf 'raise ImportError("gate: simulating a python3 without Pillow")\n' > "$NOPIL/PIL.py"
printf '#!/bin/sh\nexec env PYTHONPATH=%s /usr/bin/python3 "$@"\n' "$NOPIL" > "$NOPIL/bin/python3"
chmod +x "$NOPIL/bin/python3"
PATH="$NOPIL/bin:$PATH" python3 -c 'from PIL import Image' >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "gate: could not build a python3-without-Pillow shim" >&2; exit 1
fi
( cd "$INS/app" && XDG_DATA_HOME="$INS/share" PATH="$NOPIL/bin:$PATH" timeout 60 ./install-menu-entry.sh ) >"$INS/log.txt" 2>&1
# The bug this locks: the resize step used to exit 0 when Pillow was missing
# having written nothing, and the elif fallback (keyed on python3 being absent)
# then never ran - no icon, no warning, exit 0.
INSICON=$(ls "$INS/share/icons/hicolor/256x256/apps/"mnpdf-*.png 2>/dev/null | head -1)
if [ -n "$INSICON" ]; then
    ok "install-menu-entry.sh installs the themed icon with python3 present but no Pillow"
else
    bad "install-menu-entry.sh installed no icon (python3 without Pillow)"
    sed 's/^/       installer: /' "$INS/log.txt" >&2 || true
fi
# the entry's Icon= must name an icon that exists, and that name must be the
# content-hashed one: the hash is what makes a running desktop re-read the
# pixels after a change (a same-name change stays cached for the life of the
# session - measured on this machine's own panel, which never re-read)
ENTRY="$INS/share/applications/mnpdf.desktop"
ENTICON=$(sed -n 's/^Icon=//p' "$ENTRY" 2>/dev/null | head -1)
if [ -n "$ENTICON" ] && [ -e "$INS/share/icons/hicolor/256x256/apps/$ENTICON.png" ]; then
    ok "the entry's Icon= names an installed theme file ($ENTICON)"
else
    bad "the entry's Icon= does not name an installed theme file (${ENTICON:-none})"
fi
case "$ENTICON" in
    mnpdf-????????) ok "the icon name carries the content hash (name: $ENTICON)" ;;
    *) bad "the icon name is not content-hashed (${ENTICON:-none})" ;;
esac
# and an install must retire icon files from earlier installs
cp "$INS/share/icons/hicolor/256x256/apps/$ENTICON.png" \
   "$INS/share/icons/hicolor/256x256/apps/mnpdf-aaaabbbb.png" 2>/dev/null
( cd "$INS/app" && XDG_DATA_HOME="$INS/share" timeout 60 ./install-menu-entry.sh ) >>"$INS/log2.txt" 2>&1
if [ -e "$INS/share/icons/hicolor/256x256/apps/mnpdf-aaaabbbb.png" ]; then
    bad "installing under a hash leaves the previous icon file behind"
else
    ok "installing retires icon files from earlier installs"
fi
# ...and the ordinary install takes the good path (Pillow here, or the fallback
# on a machine without it), so --remove is exercised on a full icon set.
( cd "$INS/app" && XDG_DATA_HOME="$INS/share" timeout 60 ./install-menu-entry.sh ) >"$INS/normal.txt" 2>&1
chk $? "install-menu-entry.sh installs a normal tree"

# ...and an entry written by v2.8.1/v2.9.0 (same file, no marker) is upgraded,
# not refused: README-linux.txt tells those users to re-run the installer.
LEG=$STATE/legacy; mkdir -p "$LEG/share/applications"
cat > "$LEG/share/applications/mnpdf.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=mnpdf
GenericName=PDF Reader
Comment=A small, fast, native PDF reader with a highlighter
Exec=/old/unpack/mnpdf %f
Icon=mnpdf
Terminal=false
Categories=Office;Viewer;
MimeType=application/pdf;
StartupWMClass=Mnpdf
EOF
( cd "$INS/app" && XDG_DATA_HOME="$LEG/share" timeout 60 ./install-menu-entry.sh ) >"$LEG/log.txt" 2>&1
if [ $? -eq 0 ] && grep -qx 'X-mnpdf-installer=yes' "$LEG/share/applications/mnpdf.desktop" \
   && grep -q '^Exec="' "$LEG/share/applications/mnpdf.desktop"; then
    ok "install-menu-entry.sh upgrades the markerless v2.9.0 entry instead of refusing it"
else
    bad "install-menu-entry.sh refused (or did not rewrite) the v2.9.0 entry"
    sed 's/^/       installer: /' "$LEG/log.txt" >&2 || true
fi

# ...and an entry this installer never wrote is refused untouched: no icon is
# written first, the entry's bytes stay the same, and --remove leaves it too.
FOR=$STATE/foreign; mkdir -p "$FOR/share/applications"
cat > "$FOR/share/applications/mnpdf.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Some Other PDF Tool
Exec=/usr/bin/other-pdf %f
Icon=other-pdf
EOF
cp "$FOR/share/applications/mnpdf.desktop" "$FOR/before.desktop"
( cd "$INS/app" && XDG_DATA_HOME="$FOR/share" timeout 60 ./install-menu-entry.sh ) >"$FOR/log.txt" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && cmp -s "$FOR/before.desktop" "$FOR/share/applications/mnpdf.desktop" \
   && [ ! -e "$FOR/share/icons/hicolor/256x256/apps/mnpdf.png" ]; then
    ok "install-menu-entry.sh refuses a foreign entry, leaves it and writes no icons"
else
    bad "install-menu-entry.sh clobbered (or half-installed over) a foreign entry"
    sed 's/^/       installer: /' "$FOR/log.txt" >&2 || true
fi
( cd "$INS/app" && XDG_DATA_HOME="$FOR/share" timeout 60 ./install-menu-entry.sh --remove ) >"$FOR/rm.txt" 2>&1
if [ $? -ne 0 ] && [ -e "$FOR/share/applications/mnpdf.desktop" ]; then
    ok "install-menu-entry.sh --remove refuses an entry it did not write"
else
    bad "install-menu-entry.sh --remove deleted a foreign entry"
fi

# 3. The menu entry quotes the app path, so a directory with a space works
ENTRY="$INS/share/applications/mnpdf.desktop"
grep -q '^Exec="' "$ENTRY" 2>/dev/null
chk $? "the installed menu entry quotes its Exec path"
desktop-file-validate "$ENTRY" >/dev/null 2>&1
chk $? "the installed menu entry passes desktop-file-validate"

# 4. The window icon is the artwork beside the BINARY, not a file in the CWD:
#    a menu launch starts in $HOME, where there is no mnpdf.png. Launch the
#    unpacked layout from an empty CWD and assert the window's WM_HINTS icon
#    pixmap is the 512x512 brand PNG. (GTK on X11 never sets _NET_WM_ICON, so
#    WM_HINTS is the observable; the broken CWD-relative path fell back to an
#    icon NAME, which sets no pixmap at all - and a theme pixmap would not be
#    512x512.)
ICON=$STATE/icon; mkdir -p "$ICON/cfg/mnpdf" "$ICON/cwd"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\n' > "$ICON/cfg/mnpdf/app.txt"
PDFABS=$(cd "$(dirname "$PDF")" && pwd)/$(basename "$PDF")
python3 - "$ICON" <<'PY'
import sys
open(sys.argv[1] + "/dims.py", "w").write('''
import ctypes, sys
x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
d = x11.XOpenDisplay(None)
if not d:
    sys.exit(2)
pid = int(sys.argv[1], 16)
rw = ctypes.c_ulong(); x = ctypes.c_int(); y = ctypes.c_int()
w = ctypes.c_ulong(); h = ctypes.c_ulong(); bw = ctypes.c_ulong(); dep = ctypes.c_uint()
x11.XGetGeometry.restype = ctypes.c_int
x11.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_uint)]
Ignore = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
handler = Ignore(lambda dp, ev: 0)
x11.XSetErrorHandler(handler)
rc = x11.XGetGeometry(d, pid, ctypes.byref(rw), ctypes.byref(x), ctypes.byref(y),
                      ctypes.byref(w), ctypes.byref(h), ctypes.byref(bw), ctypes.byref(dep))
if rc != 0:
    print("dims=%dx%d" % (w.value, h.value))
''')
PY
( cd "$ICON/cwd" && xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 MNPDF_NO_UPDATE=1 XDG_CONFIG_HOME="$ICON/cfg" \
  sh -c 'timeout 12 "$0" "$1" >"$2/log.txt" 2>&1 & sleep 6;
         id=$(xwininfo -root -children 2>/dev/null | grep "\"Mnpdf\"" | head -1 | sed "s/^ *//;s/ .*//");
         hint=$(xprop -id "$id" WM_HINTS 2>/dev/null | sed -n "s/.*bitmap id # to use for icon: *//p");
         [ -n "$hint" ] && python3 "$2/dims.py" "$hint"' \
  "$INS/app/mnpdf" "$PDFABS" "$ICON" ) >"$ICON/xprop.txt" 2>&1
grep -q '^dims=512x512$' "$ICON/xprop.txt"
chk $? "the window icon is the 512x512 mnpdf.png beside the binary, not the CWD"

# 5. --remove takes back everything it wrote, including the empty app dirs,
#    and says so when there is nothing left.
( cd "$INS/app" && XDG_DATA_HOME="$INS/share" timeout 60 ./install-menu-entry.sh --remove ) >"$INS/rm.txt" 2>&1
if [ ! -e "$ENTRY" ] \
   && ! ls "$INS/share/icons/hicolor/256x256/apps/"mnpdf*.png >/dev/null 2>&1 \
   && [ ! -d "$INS/share/icons/hicolor/256x256/apps" ] \
   && [ ! -d "$INS/share/icons/hicolor/48x48/apps" ] \
   && [ ! -d "$INS/share/icons/hicolor/48x48" ]; then
    ok "install-menu-entry.sh --remove takes back the entry, the icons and their empty dirs"
else
    bad "install-menu-entry.sh --remove left files or empty dirs behind"
fi
OUT=$( cd "$INS/app" && XDG_DATA_HOME="$INS/share" timeout 60 ./install-menu-entry.sh --remove 2>&1 )
[ "$OUT" = "nothing to remove" ]
chk $? "install-menu-entry.sh --remove reports an empty tree as empty"

# 6. The packager's behaviour, not its source: a full tree produces an archive
#    with the PNG and without the Windows .ico, and removing any required
#    staged file makes it refuse instead of silently skipping it.
PKG=$STATE/packager
mkdir -p "$PKG/scripts" "$PKG/build-linux" "$PKG/third_party/pdfium-linux"
cp "$REPO/scripts/make-release-tarball.sh" "$PKG/scripts/"
cp "$REPO/scripts/install-menu-entry.sh" "$PKG/scripts/"
cp "$REPO/mnpdf.png" "$PKG/mnpdf.png"
printf 'const char* gAppVersion = "9.9.9";\n' > "$PKG/build-linux/mnpdf-version.cpp"
printf '#!/bin/sh\nexit 0\n' > "$PKG/build-linux/mnpdf"; chmod +x "$PKG/build-linux/mnpdf"
printf 'pdfium\n' > "$PKG/build-linux/libpdfium.so"
printf 'license\n' > "$PKG/third_party/pdfium-linux/LICENSE"
printf 'readme\n' > "$PKG/README-linux.txt"
( cd "$PKG" && ./scripts/make-release-tarball.sh ) >"$PKG/build.txt" 2>&1
tar tzf "$PKG/build-linux/mnpdf-linux-x64-v9.9.9.tar.gz" >"$PKG/list.txt" 2>/dev/null
if [ $? -eq 0 ] && grep -q '/mnpdf.png$' "$PKG/list.txt" \
   && grep -q '/install-menu-entry.sh$' "$PKG/list.txt" \
   && grep -q '/README-linux.txt$' "$PKG/list.txt" \
   && grep -q '/PDFIUM-LICENSE.txt$' "$PKG/list.txt" \
   && ! grep -q 'mnpdf.ico' "$PKG/list.txt"; then
    ok "make-release-tarball.sh ships the PNG, installer and documents, not the Windows mnpdf.ico"
else
    bad "make-release-tarball.sh produced the wrong archive contents"
    sed 's/^/       packager: /' "$PKG/build.txt" >&2 || true
fi
for f in mnpdf.png scripts/install-menu-entry.sh README-linux.txt third_party/pdfium-linux/LICENSE; do
    cp "$PKG/$f" "$PKG/$f.keep"
    rm "$PKG/$f"
    if ( cd "$PKG" && ./scripts/make-release-tarball.sh ) >"$PKG/missing.txt" 2>&1; then
        bad "make-release-tarball.sh shipped without $f"
    else
        ok "make-release-tarball.sh refuses a missing $f"
    fi
    mv "$PKG/$f.keep" "$PKG/$f"
done

echo
printf 'pass=%d fail=%d\n' "$PASS" "$FAIL"
rm -rf "$STATE"
[ "$FAIL" = "0" ]
