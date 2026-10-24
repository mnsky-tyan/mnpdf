#!/bin/sh
# Put mnpdf in this desktop's application menu, so it can be found by name
# and pinned to the taskbar like any other app.
#
#   ./install-menu-entry.sh          add the menu entry
#   ./install-menu-entry.sh --remove take it out again
#
# It writes a .desktop file pointing at THIS copy (wherever the tarball was
# unpacked), plus the icon, into the user's own data directories. No root is
# needed and nothing outside ~/.local is touched.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app="$here/mnpdf"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
entry="$apps/mnpdf.desktop"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$entry"
    for s in 16 32 48 64 128 256; do
        rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/${s}x${s}/apps/mnpdf.png"
    done
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    echo "removed $entry"
    exit 0
fi

[ -x "$app" ] || { echo "no mnpdf binary next to this script ($app)" >&2; exit 1; }

mkdir -p "$apps"

# the icon: a PNG per size, converted from the Windows icon the source ships,
# so the menu and the taskbar show the real logo rather than a generic sheet
if [ -e "$here/mnpdf.ico" ] && command -v python3 >/dev/null; then
    python3 - "$here/mnpdf.ico" <<'PY'
import os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
ico = Image.open(sys.argv[1]).convert("RGBA")
base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
for s in (16, 32, 48, 64, 128, 256):
    d = f"{base}/icons/hicolor/{s}x{s}/apps"
    os.makedirs(d, exist_ok=True)
    ico.resize((s, s), Image.LANCZOS).save(f"{d}/mnpdf.png")
PY
elif [ -e "$here/mnpdf.png" ]; then
    for s in 48 256; do
        d="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/${s}x${s}/apps"
        mkdir -p "$d" && cp "$here/mnpdf.png" "$d/mnpdf.png"
    done
fi

cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=mnpdf
GenericName=PDF Reader
Comment=A small, fast, native PDF reader with a highlighter
Exec=$app %f
Icon=mnpdf
Terminal=false
Categories=Office;Viewer;
MimeType=application/pdf;
StartupWMClass=Mnpdf
EOF

# refresh the desktop's caches so the entry shows up in search immediately
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true

echo "installed $entry"
echo "search for \"mnpdf\" in the application menu, or right-click a PDF and Open With"
