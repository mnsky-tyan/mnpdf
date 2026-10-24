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
    if command -v curl >/dev/null; then
        CODE=$(curl -s -o /dev/null -w '%{http_code}' -I \
            "https://github.com/mnsky-tyan/mnpdf/releases/download/$FOUND/mnpdf-linux-x64-$FOUND.tar.gz")
    else
        CODE="no curl"
    fi
    if [ "$CODE" = "302" ] || [ "$CODE" = "200" ]; then
        ok "the release it found really carries the Linux archive"
    elif [ "$CODE" = "000" ] || [ "$CODE" = "no curl" ]; then
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
printf 'pass=%d fail=%d\n' "$PASS" "$FAIL"
rm -rf "$STATE"
[ "$FAIL" = "0" ]
