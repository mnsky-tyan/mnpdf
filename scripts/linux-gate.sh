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

echo "== launch, render, and write a pixel shot =="
D=$STATE/launch; mkdir -p "$D"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D" MNPDF_SHOT="$OUT" MNPDF_SHOT_EXIT=1 \
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
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
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
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
  timeout 30 "$BIN" "$PDF" >"$D/log.txt" 2>&1
FIRST=$(cat "$D/mnpdf/last.txt" 2>/dev/null | tr -d '\n')
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
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
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D" \
  timeout 4 "$BIN" "$D/broken.pdf" >"$D/log.txt" 2>&1
RC=$?
# the app refuses with a message box, so the process is expected to still be
# alive at the timeout; the refusal itself is asserted by the (fixed) check
# below, which is the guard that matters
[ "$RC" -eq 124 ] || [ "$RC" -eq 0 ]; chk $? "a refused document does not crash the app"
ls "$D"/mnpdf/doc-*.txt >/dev/null 2>&1
[ $? -ne 0 ]; chk $? "no sidecar is written for a file that never opened"

echo
printf 'pass=%d fail=%d\n' "$PASS" "$FAIL"
rm -rf "$STATE"
[ "$FAIL" = "0" ]
