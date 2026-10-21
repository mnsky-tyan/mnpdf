#!/usr/bin/env bash
# Build mnpdf for Linux. The Windows counterpart is build.bat; this is the same
# recipe in shell: configure with cmake, compile, and make sure the binary and
# libpdfium.so land together in build-linux/.
set -euo pipefail
cd "$(dirname "$0")"

# 1. PDFium probe: prove the vendored Linux library works with this toolchain
#    before building the app on top of it. A missing .so is the one failure a
#    fresh checkout hits (git-lfs or an interrupted clone), and cmake would
#    report it as a linker error three hundred lines away.
if [ ! -f third_party/pdfium-linux/lib/libpdfium.so ]; then
  echo "BUILD FAILED: third_party/pdfium-linux/lib/libpdfium.so is missing" >&2
  exit 1
fi
g++ -O2 -o /tmp/mnpdf-probe.$$ src/linux/probe.cpp -I third_party/pdfium-linux/include \
  -L third_party/pdfium-linux/lib -lpdfium
if ! LD_LIBRARY_PATH=third_party/pdfium-linux/lib /tmp/mnpdf-probe.$$ tests/arc.pdf /tmp/mnpdf-probe.$$.ppm >/dev/null; then
  echo "BUILD FAILED: PDFium could not render tests/arc.pdf" >&2
  rm -f /tmp/mnpdf-probe.$$ /tmp/mnpdf-probe.$$.ppm
  exit 1
fi
rm -f /tmp/mnpdf-probe.$$ /tmp/mnpdf-probe.$$.ppm
echo "PDFIUM OK"

# 2. the app
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
cmake --build build-linux -j"$(nproc)" >/dev/null

# 3. the headless engine suite is part of the build: a core regression fails the
#    build here, not three steps later in a gate that may not be run. It gets a
#    fresh scratch root per run, so no assertion can read a previous run's files.
ST=$(mktemp -d /tmp/mnpdf-core-test.XXXXXX)
if ! LD_LIBRARY_PATH=build-linux timeout 300 build-linux/mnpdf-core-test \
      tests/arc.pdf "$ST" > "$ST/log.txt" 2>&1; then
  echo "BUILD FAILED: the headless engine suite did not pass:" >&2
  tail -20 "$ST/log.txt" >&2
  rm -rf "$ST"
  exit 1
fi
grep -E "^pass=" "$ST/log.txt"
rm -rf "$ST"

echo "BUILD OK: build-linux/mnpdf"
