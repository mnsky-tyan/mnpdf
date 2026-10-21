#!/bin/sh
# Package the Linux release asset exactly the way release v2.7.0 was published,
# so the repo can produce the name the update check looks for.
#
#   ./scripts/make-release-tarball.sh [tag]
#
# With no tag the build's version is used (the Windows kAppVersion constant that
# CMake read into mnpdf-version.cpp). The result lands in build-linux/ as
# mnpdf-linux-x64-<tag>.tar.gz and carries the binary, libpdfium.so and the
# documents, in the same layout the release asset was unpacked from.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

tag=${1:-}
if [ -z "$tag" ]; then
    # the generated header is the single source of the version: the Windows
    # kAppVersion constant. Reading it here keeps the two in step.
    ver=$(sed -n 's/^const char\* gAppVersion = "\([^"]*\)";$/\1/p' \
              build-linux/mnpdf-version.cpp 2>/dev/null | head -1)
    [ -n "$ver" ] || { echo "no build-linux/mnpdf-version.cpp; run ./build-linux.sh first" >&2; exit 1; }
    tag="v$ver"
fi

[ -x build-linux/mnpdf ] || { echo "no build-linux/mnpdf; run ./build-linux.sh first" >&2; exit 1; }

stage=build-linux/release-$tag
rm -rf "$stage"
mkdir -p "$stage"

# the library beside the binary: the $ORIGIN rpath is what makes the tarball
# runnable from wherever it is unpacked
if [ -e build-linux/libpdfium.so ]; then
    cp build-linux/libpdfium.so "$stage/libpdfium.so"
else
    so=$(ls third_party/pdfium-linux/lib/libpdfium.so 2>/dev/null | head -1)
    [ -n "${so:-}" ] || { echo "no libpdfium.so found" >&2; exit 1; }
    cp "$so" "$stage/libpdfium.so"
fi
cp build-linux/mnpdf "$stage/mnpdf"
[ -e README.txt ] && cp README.txt "$stage/README.txt"
[ -e third_party/pdfium-linux/LICENSE ] && cp third_party/pdfium-linux/LICENSE "$stage/PDFIUM-LICENSE.txt"
[ -e README-linux.txt ] && cp README-linux.txt "$stage/README-linux.txt"

out=build-linux/mnpdf-linux-x64-$tag.tar.gz
tar -C "$stage" -czf "$out" .
echo "wrote $out ($(stat -c%s "$out") bytes)"
echo "sha256 $(sha256sum "$out" | cut -d' ' -f1)"
