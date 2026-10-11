# Build the release ZIP, and refuse to do it when the version contract is broken.
#
# The contract:
#   1. src/main.cpp      - kAppVersion, what the app reports over the wire
#   2. README.txt        - its first line, the file the ZIP ships to the reader
#   3. the archive name  - mnpdf-win-x64-<tag>.zip, which is literally what the
#                          running app asks GitHub for when it checks for an
#                          update (src/main.cpp: kDownloadBaseUrl + tag +
#                          "/mnpdf-win-x64-" + tag + ".zip"). A ZIP named for a
#                          version the app does not report is a release no
#                          reader can ever update to, or one that offers a
#                          download that 404s.
#
# Legs 1 and 2 are compared against each other, so a mismatch refuses the build.
# Leg 3 cannot be compared against itself - this script derives the name it
# writes from kAppVersion - so it is checked differently: the app's own URL
# template must be the naming scheme this script builds. That check can fail
# (rename the app's pattern and the build refuses), which is what makes the
# documented third leg real rather than decorative.
#
# Only leg 2 was enforced before this script existed (test-release.ps1 checks
# README.txt against kAppVersion), and this file did not exist at all - the
# published ZIPs (v2.2.0 - v2.4.0 in build/) were assembled by hand, so nothing
# caught a mismatch between the archive name and the app's own version string.
#
# Usage:  powershell -File scripts\make-release-zip.ps1 [-OutDir build]
# Prints the created ZIP path on success; exits 1 with a loud reason otherwise.

[CmdletBinding()]
param([string]$OutDir)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'build' }
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repo $OutDir }

function Fail([string]$why) {
  Write-Output ("RELEASE: REFUSING - {0}" -f $why)
  exit 1
}

# leg 1: the app's own version
$mainCpp = Join-Path $repo 'src\main.cpp'
if (-not (Test-Path -LiteralPath $mainCpp)) { Fail "src\main.cpp not found at $mainCpp" }
$mainText = Get-Content -LiteralPath $mainCpp -Raw
$m1 = [regex]::Match($mainText, 'kAppVersion\s*=\s*L"(\d+\.\d+\.\d+)"')
if (-not $m1.Success) { Fail 'kAppVersion not found (or not X.Y.Z) in src\main.cpp' }
$appVersion = $m1.Groups[1].Value

# leg 2: the shipped README's first line
$readme = Join-Path $repo 'README.txt'
if (-not (Test-Path -LiteralPath $readme)) { Fail "README.txt not found at $readme" }
$readmeText = Get-Content -LiteralPath $readme -Raw
$m2 = [regex]::Match($readmeText, 'mnpdf v(\d+\.\d+\.\d+)')
if (-not $m2.Success) { Fail 'README.txt carries no "mnpdf vX.Y.Z" line' }
$readmeVersion = $m2.Groups[1].Value

if ($appVersion -ne $readmeVersion) {
  Fail ("version contract broken: src\main.cpp says {0}, README.txt says {1}" -f $appVersion, $readmeVersion)
}

# leg 3: the archive name the updater will request. This script builds that
# name from kAppVersion, so it cannot disagree with itself; the honest check is
# that the app's own URL template uses this same naming scheme. A rename of the
# app's pattern (a different prefix or extension) fails here instead of shipping
# a release the updater can never fetch.
$m3 = [regex]::Match($mainText, 'mnpdf-win-x64-"\s*\+\s*tag\s*\+\s*L"\.zip"')
if (-not $m3.Success) {
  Fail 'src\main.cpp does not build the update URL as "/mnpdf-win-x64-" + tag + ".zip" - the archive this script builds would never be requested'
}
$tag = "v$appVersion"
$zipName = "mnpdf-win-x64-$tag.zip"
$zipPath = Join-Path $OutDir $zipName

# the payload: exactly what every published ZIP has carried since v2.2.0,
# flat (no folder inside the archive)
$payload = @(
  @{ src = Join-Path $repo 'build\mnpdf.exe';          name = 'mnpdf.exe' },
  @{ src = Join-Path $repo 'build\pdfium.dll';         name = 'pdfium.dll' },
  @{ src = Join-Path $repo 'README.txt';               name = 'README.txt' },
  @{ src = Join-Path $repo 'PDFIUM-LICENSE.txt';       name = 'PDFIUM-LICENSE.txt' }
)
$missing = @($payload | Where-Object { -not (Test-Path -LiteralPath $_.src) })
if ($missing.Count -gt 0) {
  Fail ("missing payload file(s): " + (($missing | ForEach-Object { $_.src }) -join ', ') + ' - run build.bat first')
}

# state the agreement it is about to build
Write-Output ("RELEASE: version contract agreed - kAppVersion={0}, README.txt={1}, archive={2} (naming scheme verified against src\main.cpp)" -f $appVersion, $readmeVersion, $zipName)

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("mnpdf-rel-" + [Guid]::NewGuid().ToString('n'))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
  foreach ($f in $payload) { Copy-Item -LiteralPath $f.src -Destination (Join-Path $staging $f.name) -Force }
  [System.IO.Compression.ZipFile]::CreateFromDirectory(
    $staging, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
} finally {
  Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path -LiteralPath $zipPath)) { Fail "the archive was not created at $zipPath" }
$size = (Get-Item -LiteralPath $zipPath).Length
Write-Output ("RELEASE: wrote {0} ({1:N0} bytes)" -f $zipPath, $size)
Write-Output $zipPath
