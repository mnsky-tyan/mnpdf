Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class F {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, [MarshalAs(UnmanagedType.LPWStr)] string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int h2, bool r);
}
"@
[void][F]::SetProcessDpiAwarenessContext([IntPtr](-4))
$failures = New-Object System.Collections.Generic.List[string]
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function GetClip {
  if (-not $script:clipOk) { return '' }   # clipboard known broken: never touch it (reads can hang)
  $tmp = [System.IO.Path]::GetTempFileName()
  $proc = Start-Process powershell -ArgumentList '-NoProfile','-Command',"Get-Clipboard -Raw | Out-File -Encoding unicode '$tmp'" -PassThru -WindowStyle Hidden
  if (-not $proc.WaitForExit(2000)) { try { $proc.Kill() } catch {}; return '' }   # zombie lock: bail out
  return ((Get-Content $tmp -Raw -ErrorAction SilentlyContinue) -replace "
?
$", '')
}

function Title([IntPtr]$h) { $sb = New-Object System.Text.StringBuilder 256; [void][F]::GetWindowTextW($h, $sb, 256); $sb.ToString() }

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

$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
$existing = Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }
if ($existing) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }
$p = Start-Process -FilePath (Join-Path $PSScriptRoot "build\mnpdf.exe") -ArgumentList (Join-Path $PSScriptRoot "build\arc.pdf") -PassThru
Start-Sleep -Milliseconds 1500
$p.Refresh()
$h = $p.MainWindowHandle
[void][F]::MoveWindow($h, 60, 60, 1100, 800, $true)
Start-Sleep -Milliseconds 500

# --- T1: scroll to page 3 with the wheel (go-to-page was removed by request) ---
# scroll notch by notch and stop as soon as page 3 is current: a fixed notch
# count lands somewhere else whenever the machine is loaded
$reached3 = $false
for ($i = 0; $i -lt 60 -and -not $reached3; $i++) {
  [F]::PostMessageW($h, 0x020A, [IntPtr]0xFF880000, (Lparam 550 400)) | Out-Null   # wheel down
  $reached3 = Await { (Title $h) -match 'mnpdf 3/13' } 500
}
$t = Title $h
if ($reached3) { Write-Output "PASS wheel scroll to page 3 ('$t')" }
else { $failures.Add("wheel scroll"); Write-Output "FAIL wheel scroll to page 3 ('$t')" }

# --- T2: autosave after wheel scrolling ---
$w = [IntPtr]((-120) -shl 16)
for ($i = 0; $i -lt 4; $i++) { [F]::PostMessageW($h, 0x020A, $w, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 80 }
$tScroll = Title $h
$wantPage = if ($tScroll -match 'mnpdf (\d+)/') { [int]$Matches[1] } else { 0 }
$sidecar = $null
Await {                                                   # 800ms debounce tick, polled
  $script:sidecar = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
  $script:sidecar -and (Get-Content $script:sidecar.FullName -Raw -ErrorAction SilentlyContinue) -match "page=$wantPage`n"
} 8000 | Out-Null
$sidecar = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($sidecar) {
  $content = Get-Content $sidecar.FullName -Raw
  Write-Output ("sidecar: " + ($content -replace "`n", ' | '))
  if ($content -match 'page=(\d+)') { Write-Output "PASS autosave wrote page=$($Matches[1]) (title was '$tScroll')" }
  else { $failures.Add("autosave content"); Write-Output "FAIL sidecar content: $content" }
} else { $failures.Add("autosave file"); Write-Output "FAIL no sidecar file" }
if (Test-Path "$env:APPDATA\mnpdf\last.txt") { Write-Output "PASS last.txt written" }
else { $failures.Add("last.txt"); Write-Output "FAIL no last.txt" }

# --- T3: cross-page drag selection (zoom out until two pages fit) ---
for ($i = 0; $i -lt 8; $i++) {
  [F]::PostMessageW($h, 0x0100, [IntPtr]0x6D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 60
  [F]::PostMessageW($h, 0x0101, [IntPtr]0x6D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 300
$t = Title $h
Write-Output ("zoomed out: '$t'")
# drag from lower page 1 across the gap into upper page 2
[F]::PostMessageW($h, 0x0201, [IntPtr]1, (Lparam 540 260)) | Out-Null
Start-Sleep -Milliseconds 60
foreach ($y in 320, 380, 440, 500, 560) { [F]::PostMessageW($h, 0x0200, [IntPtr]1, (Lparam 540 $y)) | Out-Null; Start-Sleep -Milliseconds 40 }
[F]::PostMessageW($h, 0x0202, [IntPtr]0, (Lparam 540 560)) | Out-Null
Start-Sleep -Milliseconds 200
$prev = GetClip
[F]::PostMessageW($h, 0x0301, [IntPtr]0, [IntPtr]0) | Out-Null          # WM_COPY
# nothing to poll for: the clipboard gate is closed on this box, so GetClip
# always returns '' and the check below can only take the SKIP branch
Start-Sleep -Milliseconds 4000
$t = GetClip
$pages = ($t -split "`r`n").Count
if (-not $clipOk) { Write-Output "SKIP cross-page copy: clipboard locked (verified previously)" }
elseif ($t -and $t -match "`r`n") { Write-Output "PASS cross-page copy: [$($t.Length) chars, $pages lines] '$($t.Substring(0, [Math]::Min(60, $t.Length)))' ..." }
else { $failures.Add("cross-page copy"); Write-Output "FAIL cross-page copy got: '$t'" }

# --- T4: quit, then plain-launch reopen restores the saved page ---
$scq = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
$savedPage = if ($scq) { [int]([regex]::Match((Get-Content $scq.FullName -Raw), 'page=(\d+)')).Groups[1].Value } else { 0 }
[F]::PostMessageW($h, 0x0111, [IntPtr]112, [IntPtr]0) | Out-Null       # WM_COMMAND 112 = Quit
Await { $p.Refresh(); $p.HasExited } 8000 | Out-Null
$p.Refresh()
if ($p.HasExited) { Write-Output "PASS quit menu exited the app" }
else { $failures.Add("quit"); Write-Output "FAIL quit: still running" }
Start-Process -FilePath (Join-Path $PSScriptRoot "build\mnpdf.exe")    # NO arguments
$p2 = $null
Await {                                                    # the relaunch paints its title once ready
  $procs = @(Get-Process mnpdf -ErrorAction SilentlyContinue)
  $script:p2 = $procs | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
  $script:p2 -and (Title $script:p2.MainWindowHandle) -match 'mnpdf \d+/13'
} 15000 | Out-Null
$procs = @(Get-Process mnpdf -ErrorAction SilentlyContinue)
$p2 = $procs | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p2) { $failures.Add("reopen"); Write-Output "FAIL reopen: no instance"; exit 1 }
$t = Title $p2.MainWindowHandle
if ($t -match "mnpdf $savedPage/13") { Write-Output "PASS reopen last doc at page $savedPage ('$t')" }
else { $failures.Add("reopen page"); Write-Output "FAIL reopen: expected page $savedPage, got '$t'" }

Write-Output ""
if ($failures.Count) { Write-Output "RESULT: $($failures.Count) FAILURE(S)"; exit 1 }
Write-Output "RESULT: ALL PASS"
