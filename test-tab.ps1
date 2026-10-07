. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
# the app is per-monitor aware, so this process must be too (shared P/Invoke
# surface in lib): the strip coordinates below are computed from the window's
# own dpi, so a wrong awareness here would aim at pixels no reader sees
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function Title([IntPtr]$h) { $sb = New-Object System.Text.StringBuilder 256; [void][MN]::GetWindowTextW($h, $sb, 256); $sb.ToString() }

$env:MNPDF_VERBOSE = "1"   # verbose titles: which document is active is the title

# The common file dialogs are real OS windows, not app clients. They are not
# used here at all: a posted command counts as input to the app, which hands it
# back the right to take foreground, and a modal dialog opening on the 'second'
# desktop then pulled the captain's view over to it - measured. The suite opens
# the second document the sanctioned way instead: WM_COPYDATA, the documented
# cross-process data message, which needs no dialog and touches no foreground
# rights.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class CD {
  [StructLayout(LayoutKind.Sequential)]
  public struct CDS { public IntPtr dwData; public int cbData; public IntPtr lpData; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, ref CDS d);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
'@
function AskOpen([IntPtr]$h, [string]$Path) {
  $bytes = [Text.Encoding]::Unicode.GetBytes($Path + "`0")
  $p = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
  try {
    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $p, $bytes.Length)
    $cd = New-Object CD+CDS
    $cd.dwData = [IntPtr]1
    $cd.cbData = $bytes.Length
    $cd.lpData = $p
    [void][CD]::SendMessageW($h, 0x004A, [IntPtr]::Zero, [ref]$cd)   # WM_COPYDATA
  } finally {
    [Runtime.InteropServices.Marshal]::FreeHGlobal($p)
  }
}
function AwaitTitle([IntPtr]$h, [string]$Pattern, [int]$Ms = 8000) {
  Await { (Title $h) -match $Pattern } $Ms
}
function ClickAt([IntPtr]$h, [int]$x, [int]$y) {
  [void][MN]::PostMessageW($h, 0x0201, [IntPtr]1, (Lparam $x $y))   # WM_LBUTTONDOWN
  Start-Sleep -Milliseconds 60
  [void][MN]::PostMessageW($h, 0x0202, [IntPtr]0, (Lparam $x $y))   # WM_LBUTTONUP
  Start-Sleep -Milliseconds 120
}

# the strip's own geometry, in device px: the app scales it from the window dpi
function StripMetrics([IntPtr]$h) {
  $dpi = [CD]::GetDpiForWindow($h)
  $strip = [Math]::Max(26, [int]($dpi * 30 / 96))
  $tw = [int]($dpi * 160 / 96)
  return @{ strip = $strip; tw = $tw }
}

# requires a fresh launch we own: the zoom a tab is expected to come back with
# only holds on a clean state, and a run that must not disturb a running
# instance finds out FIRST
Assert-NoRunningApp
$A = Join-Path $PSScriptRoot "tests\arc.pdf"           # document A
$B = Join-Path $PSScriptRoot "tests\arc-annot.pdf"      # document B - a different
                                                       # page count is what makes
                                                       # the two documents
                                                       # distinguishable by title
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
$proc = Launch (Resolve-AppExe) $A
$h = FindAppWindow $proc.Id

# the page counts come from the app, not from a fixture assumption: which
# document is active is only ever asserted through the verbose title
function PagesInTitle([IntPtr]$h) {
  $t = Title $h
  if ($t -match 'mnpdf 1/(\d+)') { return [int]$Matches[1] } else { return 0 }
}
if (-not (AwaitTitle $h 'mnpdf 1/\d+')) { Fail 'the window opens on a document' "'$(Title $h)'" }
$pagesA = PagesInTitle $h

# --- T1: the window opens on one tab holding the document --------------------
if (AwaitTitle $h "mnpdf 1/$pagesA") { Pass 'opens on a single tab with the document' }
else { Fail 'opens on a single tab with the document' "'$(Title $h)'" }

# --- T2: zoom twice, so each tab has a distinguishable view to come back to ---
# the starting zoom is fit-width, so the number is whatever the fit is; what
# matters is that the tab keeps ITS number when it comes back
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]105, [IntPtr]::Zero)   # Zoom In
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]105, [IntPtr]::Zero)   # Zoom In
Start-Sleep -Seconds 1     # both commands land before the number is read: a
                           # match on the first of two queued zooms reads 207%
                           # while the tab settles at 249%
if (-not (AwaitTitle $h "mnpdf 1/$pagesA\s+\d+%")) { Fail 'zoomed in, so the tab has its own view' "'$(Title $h)'" }
$zoomA = ([regex]::Match((Title $h), '(\d+)%')).Groups[1].Value + '%'
if ($zoomA) { Pass 'zoomed in, so the tab has its own view' "'$(Title $h)'" }
else { Fail 'zoomed in, so the tab has its own view' "'$(Title $h)'" }

# --- T3: New Tab opens an empty tab; the verbose title loses the page info ----
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]300, [IntPtr]::Zero)   # New Tab
if (AwaitTitle $h '^mnpdf$') { Pass 'New Tab opens an empty tab' }
else { Fail 'New Tab opens an empty tab' "'$(Title $h)'" }

# --- T4: opening the second document fills the new tab ----------------------
AskOpen $h $B
$ok4 = AwaitTitle $h 'mnpdf 1/\d+'
$pagesB = PagesInTitle $h
if ($pagesA -gt 0 -and $pagesB -gt 0 -and $pagesA -ne $pagesB) {
  Pass 'opening a second document fills the new tab' "'$(Title $h)'"
} else {
  Fail 'opening a second document fills the new tab' "'$(Title $h)'"
}

# --- T5: clicking tab 1's strip position switches back, WITH its zoom --------
$m = StripMetrics $h
$x1 = 4 + [int]($m.tw / 2)
$ys = [int]($m.strip / 2)
ClickAt $h $x1 $ys
if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'clicking the first tab brings its document and zoom back' }
else { Fail 'clicking the first tab brings its document and zoom back' "'$(Title $h)'" }

# --- T6: clicking tab 2's strip position switches to the second document -----
$x2 = 4 + [int]($m.tw * 1.5)
ClickAt $h $x2 $ys
if (AwaitTitle $h "mnpdf 1/$pagesB") { Pass 'clicking the second tab switches to its document' }
else { Fail 'clicking the second tab switches to its document' "'$(Title $h)'" }

# --- T7: Close Tab closes the second document; the first survives ------------
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]301, [IntPtr]::Zero)   # Close Tab
if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'Close Tab drops the second document and keeps the first' }
else { Fail 'Close Tab drops the second document and keeps the first' "'$(Title $h)'" }

# --- T8: the + button opens another empty tab, and Close Tab returns ----------
$cr = New-Object MNRect
[void][MN]::GetClientRect($h, [ref]$cr)
$clientW = $cr.R - $cr.L
ClickAt $h ($clientW - [int]($m.strip / 2)) $ys
if (AwaitTitle $h '^mnpdf$') {
  [void][MN]::PostMessageW($h, 0x0111, [IntPtr]301, [IntPtr]::Zero)   # Close Tab
  if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'the + button opens a tab and closing it returns' }
  else { Fail 'the + button opens a tab and closing it returns' "'$(Title $h)'" }
} else {
  Fail 'the + button opens a tab and closing it returns' "'$(Title $h)'"
}

# --- T9: closing the last tab quits the window --------------------------------
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]301, [IntPtr]::Zero)   # Close Tab
$exited = $false
for ($i = 0; $i -lt 60 -and -not $exited; $i++) {
  $exited = $proc.HasExited
  if (-not $exited) { Start-Sleep -Milliseconds 200 }
}
if ($exited) { Pass 'closing the last tab quits' }
else { Fail 'closing the last tab quits' 'still running'; Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
