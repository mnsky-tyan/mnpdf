# the fixture lives in tests\ because build\ is all output and can be deleted
# wholesale; every run works on a fresh copy so a mutating run cannot poison
# the next one. Copied after the SKIP guard below, so a skipped run leaves
# the checkout untouched.
$Pdf = Join-Path $PSScriptRoot "build\arc.pdf"
. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class C {
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
}
"@

# requires a fresh launch we own: fit-mode/zoom assumptions only hold on a clean state
Assert-NoRunningApp
Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") $Pdf -Force
$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
$proc = Launch (Resolve-AppExe) $Pdf
$p = $proc
$h = FindAppWindow $p.Id
Start-Sleep -Milliseconds 500


$t0 = Title $h
Write-Output "start: '$t0'"

# 1. resize -> fit-width must refit (zoom % changes)
$zoom0 = if ($t0 -match '(\d+)%') { [int]$Matches[1] } else { 0 }
# The target must be a size this screen can actually show, not one taken from
# another machine: Windows caps a window at the work area (plus the invisible
# frame sliver), so a hard-coded 1500x900 on a runner whose work area is smaller
# comes out exactly the size the created window was already capped at, the
# client width never moves, and fit-width correctly keeps its zoom - CI reported
# that as "resize refit: zoom stayed 165%" on the 1024x768 runner. Half the work
# area always fits, and always differs from a window capped at the whole of it.
$rw = [Math]::Max(320, [int]([C]::GetSystemMetrics(78) / 2))    # SM_CXMAXIMIZED: the primary work area
$rh = [Math]::Max(240, [int]([C]::GetSystemMetrics(79) / 2))    # SM_CYMAXIMIZED
[MN]::MoveWindow($h, 60, 60, $rw, $rh, $true) | Out-Null
Await { (Title $h) -match '(\d+)%' -and [int]$Matches[1] -ne $zoom0 } 8000 | Out-Null
$t1 = Title $h
$zoom1 = if ($t1 -match '(\d+)%') { [int]$Matches[1] } else { 0 }
if ($zoom1 -ne $zoom0) { Pass "resize refit: zoom $zoom0% -> $zoom1%  ('$t1')" }
else { Fail "resize refit" "zoom stayed $zoom1% ('$t1')" }

# 2. wheel down continuously -> page number must advance without a flip reset
$pg = 1
$w = [IntPtr]((-120) -shl 16)
for ($i = 0; $i -lt 30; $i++) {
  [void][MN]::PostMessageW($h, 0x020A, $w, [IntPtr]0)   # wheel down
  Start-Sleep -Milliseconds 60
  $t = Title $h
  if ($t -match "mnpdf (\d+)/$FixturePages") { if ([int]$Matches[1] -gt $pg) { $pg = [int]$Matches[1] } }
}
$t2 = Title $h
Write-Output "after 30 wheel ticks: '$t2'"
if ($pg -ge 2) { Pass "continuous scroll: reached page $pg by wheel" }
else { Fail "continuous scroll" "still page $pg" }

# 3. wheel back up -> must return to page 1 (keep ticking until it does)
$w = [IntPtr](120 -shl 16)
$back = $false
for ($i = 0; $i -lt 80 -and -not $back; $i++) {
  [void][MN]::PostMessageW($h, 0x020A, $w, [IntPtr]0)
  $back = Await { (Title $h) -match "mnpdf 1/$FixturePages" } 400
}
$t3 = Title $h
if ($t3 -match "mnpdf 1/$FixturePages") { Pass "scroll back to top: '$t3'" }
else { Fail "scroll back" "'$t3'" }

$p.Refresh()
if ($p.HasExited) { throw "mnpdf exited unexpectedly" }

# this suite's instance goes home: a suite that leaves an app running fails the
# next suite's machine-is-free pre-flight, here and in CI
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero)
[void](Await { $p.HasExited } 15000)
if (-not $p.HasExited) { $p | Stop-Process -Force }

Complete-Suite
