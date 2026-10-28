#!/bin/sh
# The release version contract, in one place so the workflow and the gate
# cannot disagree about it: the tag, kAppVersion in src/main.cpp and the
# version line in README.txt must all name the same X.Y.Z.
#
# Both greps are anchored to the end of the version, so a partial match cannot
# pass: kAppVersion = L"2.9.0-rc1" must not satisfy a v2.9.0 tag, and README.txt
# saying "mnpdf v2.9.0-rc1" must not either. (The real README header continues
# with " - Windows x64 portable release" after the version, so its leg allows
# whitespace or end-of-line after the version.)
#
#   ./scripts/check-release-contract.sh [repo-root] [tag]
#
# The tag is optional: without it only src/main.cpp and README.txt are compared.
set -eu

root=${1:-.}
tag=${2:-}

app=$(grep -oP 'kAppVersion\s*=\s*L"\K[0-9]+\.[0-9]+\.[0-9]+(?=")' \
          "$root/src/main.cpp" 2>/dev/null | head -1 || true)
readme=$(grep -oP 'mnpdf v\K[0-9]+\.[0-9]+\.[0-9]+(?=$|[ \t])' \
             "$root/README.txt" 2>/dev/null | head -1 || true)

if [ -z "$app" ] || [ -z "$readme" ]; then
    echo "could not read the version from $root/src/main.cpp or $root/README.txt" >&2
    exit 1
fi
if [ "$readme" != "$app" ]; then
    echo "README.txt says $readme but the app says $app" >&2
    exit 1
fi
if [ -n "$tag" ] && [ "v$app" != "$tag" ]; then
    echo "the tag ($tag) does not match kAppVersion ($app) in src/main.cpp" >&2
    exit 1
fi

echo "tag=${tag:-(none)} app=$app readme=$readme"
