param([string]$Pdf)
if (-not $Pdf) { $Pdf = Join-Path $PSScriptRoot "build\arc.pdf" }
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class C {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int h2, bool r);
}
"@
[void][C]::SetProcessDpiAwarenessContext([IntPtr](-4))

# requires a fresh launch: fit-mode/zoom assumptions only hold on a clean state
$procs = @(Get-Process mnpdf -ErrorAction SilentlyContinue)
if ($procs) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }
if (-not $procs) {
  $env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
  Start-Process -FilePath (Join-Path $PSScriptRoot "build\mnpdf.exe") -ArgumentList $Pdf
  Start-Sleep -Milliseconds 1500
  $procs = @(Get-Process mnpdf -ErrorAction Stop)
}
$p = $procs | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { throw "no mnpdf instance with a window" }
$h = $p.MainWindowHandle

function Title {
  $sb = New-Object System.Text.StringBuilder 256
  [void][C]::GetWindowTextW($h, $sb, 256)
  $sb.ToString()
}

$t0 = Title
Write-Output "start: '$t0'"

# wait until a condition holds (polled) instead of sleeping a guessed length:
# a loaded machine makes every fixed sleep a coin flip
function Await([scriptblock]$Cond, [int]$TimeoutMs = 10000, [int]$StepMs = 100) {
  $elapsed = 0
  while ($elapsed -lt $TimeoutMs) {
    if (& $Cond) { return $true }
    Start-Sleep -Milliseconds $StepMs
    $elapsed += $StepMs
  }
  return $false
}

# 1. resize wider -> fit-width must refit (zoom % changes)
$zoom0 = if ($t0 -match '(\d+)%') { [int]$Matches[1] } else { 0 }
[void][C]::MoveWindow($h, 60, 60, 1500, 900, $true)
Await { (Title) -match '(\d+)%' -and [int]$Matches[1] -ne $zoom0 } 8000 | Out-Null
$t1 = Title
$zoom1 = if ($t1 -match '(\d+)%') { [int]$Matches[1] } else { 0 }
if ($zoom1 -ne $zoom0) { Write-Output "PASS resize refit: zoom $zoom0% -> $zoom1%  ('$t1')" }
else { Write-Output "FAIL resize refit: zoom stayed $zoom1% ('$t1')" }

# 2. wheel down continuously -> page number must advance without a flip reset
$pg = 1
$w = [IntPtr]((-120) -shl 16)
for ($i = 0; $i -lt 30; $i++) {
  [void][C]::PostMessageW($h, 0x020A, $w, [IntPtr]0)   # wheel down
  Start-Sleep -Milliseconds 60
  $t = Title
  if ($t -match 'mnpdf (\d+)/13') { if ([int]$Matches[1] -gt $pg) { $pg = [int]$Matches[1] } }
}
$t2 = Title
Write-Output "after 30 wheel ticks: '$t2'"
if ($pg -ge 2) { Write-Output "PASS continuous scroll: reached page $pg by wheel" }
else { Write-Output "FAIL continuous scroll: still page $pg" }

# 3. wheel back up -> must return to page 1 (keep ticking until it does)
$w = [IntPtr](120 -shl 16)
$back = $false
for ($i = 0; $i -lt 80 -and -not $back; $i++) {
  [void][C]::PostMessageW($h, 0x020A, $w, [IntPtr]0)
  $back = Await { (Title) -match 'mnpdf 1/13' } 400
}
$t3 = Title
if ($t3 -match 'mnpdf 1/13') { Write-Output "PASS scroll back to top: '$t3'" }
else { Write-Output "FAIL scroll back: '$t3'" }

$p.Refresh()
if ($p.HasExited) { throw "mnpdf exited unexpectedly" }
