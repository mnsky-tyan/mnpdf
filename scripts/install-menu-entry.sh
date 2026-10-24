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
    base="${XDG_DATA_HOME:-$HOME/.local/share}"
    if [ ! -e "$entry" ] && [ ! -e "$base/icons/hicolor/256x256/apps/mnpdf.png" ]; then
        echo "nothing to remove"
        exit 0
    fi
    rm -f "$entry"
    for s in 16 32 48 64 128 256; do
        rm -f "$base/icons/hicolor/${s}x${s}/apps/mnpdf.png"
        rmdir "$base/icons/hicolor/${s}x${s}/apps" 2>/dev/null || true
        rmdir "$base/icons/hicolor/${s}x${s}" 2>/dev/null || true
    done
    # KDE rebuilds its own menu cache; every other desktop uses this one
    command -v update-desktop-database >/dev/null && \
        update-desktop-database "$apps" >/dev/null 2>&1 || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    echo "removed $entry and the icons"
    exit 0
fi

[ -x "$app" ] || { echo "no mnpdf binary next to this script ($app)" >&2; exit 1; }

mkdir -p "$apps"

# The icon: a PNG per size, resized from the shipped mnpdf.png (the same
# source as the Windows mnpdf.ico), so the menu and the taskbar show the real
# artwork rather than a generic sheet.
#
# The Pillow path is the good one. The fallback matters: python3 being present
# is NOT the same as Pillow being present, and a first version of this block
# tested for python3 only - its resize step then exited 0 having written
# nothing, the elif below never ran, and the menu got no icon at all while the
# script still reported success. So the resize now signals "no Pillow" by
# exiting 1, and the fallback is chosen on that, not on python3's existence.
base="${XDG_DATA_HOME:-$HOME/.local/share}"
if [ -e "$here/mnpdf.png" ] && command -v python3 >/dev/null; then
    if python3 - "$here/mnpdf.png" "$base" <<'PY'
import os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit(1)          # no Pillow: say so, so the caller falls back
im = Image.open(sys.argv[1]).convert("RGBA")
for s in (16, 32, 48, 64, 128, 256):
    d = f"{sys.argv[2]}/icons/hicolor/{s}x{s}/apps"
    os.makedirs(d, exist_ok=True)
    im.resize((s, s), Image.LANCZOS).save(f"{d}/mnpdf.png")
PY
    then :; else
        echo "  (no Pillow - installing the shipped PNG as the icon; themes will scale it)"
        for s in 48 256; do
            d="$base/icons/hicolor/${s}x${s}/apps"
            mkdir -p "$d" && cp "$here/mnpdf.png" "$d/mnpdf.png"
        done
    fi
elif [ -e "$here/mnpdf.png" ]; then
    for s in 48 256; do
        d="$base/icons/hicolor/${s}x${s}/apps"
        mkdir -p "$d" && cp "$here/mnpdf.png" "$d/mnpdf.png"
    done
fi

if [ -e "$entry" ] && ! grep -q '^X-mnpdf-installer=yes' "$entry"; then
    echo "warning: $entry already exists and was not written by this installer" >&2
    echo "         leaving it alone (remove it by hand to install mnpdf's entry)" >&2
    exit 1
fi

cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=mnpdf
GenericName=PDF Reader
Comment=A small, fast, native PDF reader with a highlighter
Exec="$app" %f
Icon=mnpdf
Terminal=false
Categories=Office;Viewer;
MimeType=application/pdf;
StartupWMClass=Mnpdf
X-mnpdf-installer=yes
EOF

# refresh the desktop's caches so the entry shows up in search immediately
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true

echo "installed $entry"
echo "search for \"mnpdf\" in the application menu, or right-click a PDF and Open With"
