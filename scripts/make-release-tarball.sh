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

# The tag must match the binary's own version. The archive is named after the
# tag, and the update check looks the archive up by exactly that name, so a
# tarball named for a version the binary does not claim is one the check will
# never offer - and one that says the wrong thing to anyone who unpacks it.
builtver=$(sed -n 's/^const char\* gAppVersion = "\([^"]*\)";$/\1/p' \
               build-linux/mnpdf-version.cpp | head -1)
if [ -n "$builtver" ] && [ "$tag" != "v$builtver" ]; then
    echo "the tag ($tag) does not match the build's version ($builtver)" >&2
    echo "build first, or pass the tag the build carries: v$builtver" >&2
    exit 1
fi

stage=build-linux/mnpdf-linux-x64-$tag
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
# The archive carries the tag directory, the way the published asset unpacks
# (verified against v2.7.0: `tar tzf` lists mnpdf-linux-x64-v2.7.0/... first).
# README-linux.txt says to run ./mnpdf "from this directory", so unpacking
# straight into the reader's cwd would be wrong.
parent=$(dirname "$stage")
base=$(basename "$stage")
tar -C "$parent" -czf "$out" "$base"
echo "wrote $out ($(stat -c%s "$out") bytes)"
echo "sha256 $(sha256sum "$out" | cut -d' ' -f1)"
