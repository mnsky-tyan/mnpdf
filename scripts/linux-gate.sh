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

echo "== the Advanced cascade: thumbnails, outline, night mode =="
D=$STATE/advanced; mkdir -p "$D"
# MNPDF_CMD drives one command by its menu id, the way the Windows suites post
# WM_COMMAND, so a suite can assert on chrome the page back buffer never holds.
shot_for() {   # shot_for <id> <file> -> 0 on success
    local id=$1 out=$2
    rm -rf "$D/cfg$id"; mkdir -p "$D/cfg$id"
    xvfb-run -a --server-args="-screen 0 1280x900x24" \
      env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D/cfg$id" MNPDF_CMD="$id" \
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
    python3 - "$D/outline.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
band = im.crop((max(0, w - 240), 0, w, h))
dark = sum(1 for p in band.getdata() if p[0] < 200 and p[1] < 200 and p[2] < 200)
print("       outline band dark pixels: %d" % dark)
sys.exit(0 if dark > 200 else 1)
PY
    chk $? "the outline drawer lists the document's bookmarks"
fi

echo "== the titlebar toggle, and it survives a relaunch =="
D=$STATE/titlebar; mkdir -p "$D"
rm -rf "$D/cfg"; mkdir -p "$D/cfg"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D/cfg" MNPDF_CMD=113 MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$D/log1.txt" 2>&1
grep -q "^titlebar=0$" "$D/cfg/mnpdf/app.txt"; chk $? "hiding the titlebar is recorded in app.txt"
# a relaunch must come back captionless, not with the default caption
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D/cfg" MNPDF_SHOT="$D/again.png" MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$D/log2.txt" 2>&1
grep -q "^titlebar=0$" "$D/cfg/mnpdf/app.txt"; chk $? "the captionless state survives a relaunch"

echo "== the Windows app.txt field order is kept =="
D=$STATE/order; mkdir -p "$D"
# a hand-written app.txt with the Windows keys, including the ones Linux does
# not own, so the merge is exercised the way a shared directory exercises it
mkdir -p "$D/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\nupdcheck=1700000000\nupdtag=v2.6.0\n' > "$D/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$D/cfg" MNPDF_SHOT_EXIT=1 MNPDF_SHOT=/dev/null \
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
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$I/cfg" timeout 12 "$BIN" "$PDF" >"$I/first.log" 2>&1 &
holder=$!
sleep 6
# the second one toggles the titlebar, which the primary would persist
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$I/cfg" MNPDF_CMD=113 MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >"$I/second.log" 2>&1
# the holder ends on its own timeout, a few seconds after the second one is
# done; waiting for it is what lets xvfb-run clean up after itself
wait $holder 2>/dev/null
grep -q "^titlebar=1$" "$I/cfg/mnpdf/app.txt"
chk $? "a second window does not overwrite the shared app.txt"

echo "== the update check honours the Windows cooldown =="
# The Windows build asks GitHub at most once an hour and stamps the attempt
# into app.txt (kUpdateCheckIntervalMin, src/main.cpp:67). A check inside that
# window must not make a request.
U=$STATE/updcheck; mkdir -p "$U/cfg/mnpdf"
printf 'tabstrip=1\ntitlebar=1\nautosave=1\nhlcolor=0\npincolor=5\nnight=0\nupdcheck=%s\nupdtag=v2.6.0\n' \
    "$(date +%s)" > "$U/cfg/mnpdf/app.txt"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$U/cfg" MNPDF_CMD=170 MNPDF_SHOT_EXIT=1 \
  timeout 40 "$BIN" "$PDF" >"$U/log.txt" 2>&1
# a cooled-down check must not have replaced the tag it read
grep -q "^updtag=v2.6.0$" "$U/cfg/mnpdf/app.txt"
chk $? "an update check inside the hour does not ask again"
# the answer opens a modal, and the app quits while it is up: the modal is
# destroyed with the window, so the reader must not destroy it a second time.
# GTK answers that with a critical, which the log carries.
grep -q "Gtk-CRITICAL" "$U/log.txt"; [ $? -ne 0 ]
chk $? "the update answer's modal leaves no GTK critical behind"

echo "== the update check asks the web host, not the API =="
# The Windows build reads the tag off the web host, never api.github.com,
# because that host's unauthenticated quota is shared with every other
# client behind the same IP and answers "API rate limit exceeded" once it is
# spent (src/main.cpp:2430-2432). The port must not inherit that problem.
# the check looks at the URL constants, not the whole file: the file's comment
# mentions api.github.com to explain why it does not use it
if grep -E '^static const (char\*|std::string) k[A-Za-z]*Url' src/linux/update.cpp \
     | grep -q 'api.github.com'; then
    bad "the update check still uses api.github.com"
else ok "the update check does not use the API's shared quota"; fi
grep -q "releases/latest" src/linux/update.cpp
chk $? "the update check reads the tag the way the Windows build does"

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
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$E/cfg" MNPDF_SHOT_EXIT=1 \
  timeout 30 "$BIN" "$PDF" >/dev/null 2>&1
hash=$(ls "$E/cfg/mnpdf/" 2>/dev/null | sed -n 's/^doc-\(.*\)\.txt$/\1/p' | head -1)
[ -n "$hash" ] || { echo "gate: could not learn the sidecar hash" >&2; exit 1; }
seed_marks "$hash"
xvfb-run -a --server-args="-screen 0 1280x900x24" \
  env GDK_BACKEND=x11 XDG_CONFIG_HOME="$E/cfg" MNPDF_MENU_DUMP=1 \
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
# what the Linux menu actually carries: "label: id"
seen = set()
for line in menu.splitlines():
    m = re.match(r'\s*.*?: (\d+)\s*$', line)
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
print("       windows ids carried by the menu: %d of %d"
      % (sum(1 for n in required if n in table and table[n] in seen), len(required)))
for m in missing:
    print("       " + m)
sys.exit(1 if missing else 0)
PY
chk $? "the menu carries the Windows command ids (tests/lib.ps1)"

echo
printf 'pass=%d fail=%d\n' "$PASS" "$FAIL"
rm -rf "$STATE"
[ "$FAIL" = "0" ]
