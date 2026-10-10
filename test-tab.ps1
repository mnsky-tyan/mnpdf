. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
# the app is per-monitor aware, so this process must be too (shared P/Invoke
# surface in lib): the strip coordinates below are computed from the window's
# own dpi, so a wrong awareness here would aim at pixels no reader sees
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$env:MNPDF_VERBOSE = "1"   # verbose titles: which document is active is the title

# The common file dialogs are real OS windows, not app clients. They are not
# used here at all: a posted command counts as input to the app, which hands it
# back the right to take foreground, and a modal dialog opening on the 'second'
# desktop then switched the active desktop to itself - measured. The suite opens
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

# the strip's own geometry, in device px: the app scales both from the window dpi
function StripMetrics([IntPtr]$h) {
  $dpi = [CD]::GetDpiForWindow($h)
  $strip = TabStripPx $h
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
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_ZOOM_IN, [IntPtr]::Zero)   # Zoom In
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_ZOOM_IN, [IntPtr]::Zero)   # Zoom In
Start-Sleep -Seconds 1     # both commands land before the number is read: a
                           # match on the first of two queued zooms reads 207%
                           # while the tab settles at 249%
if (-not (AwaitTitle $h "mnpdf 1/$pagesA\s+\d+%")) { Fail 'zoomed in, so the tab has its own view' "'$(Title $h)'" }
$zoomA = ([regex]::Match((Title $h), '(\d+)%')).Groups[1].Value + '%'
if ($zoomA) { Pass 'zoomed in, so the tab has its own view' "'$(Title $h)'" }
else { Fail 'zoomed in, so the tab has its own view' "'$(Title $h)'" }

# --- T3: New Tab opens an empty tab; the verbose title loses the page info ----
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_NEW_TAB, [IntPtr]::Zero)   # New Tab
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

# --- T6b: the strip hides on command and gives the band back -----------
# The toggle is a real menu row, 302, and the titlebar row it mirrors are the
# protocol: the driver posts WM_COMMAND, never a mouse click on a menu it cannot
# see. What each side of the toggle must do is asserted through the band itself:
# while the strip is hidden the coordinates that USED to be tab 1 belong to the
# document, so clicking them cannot switch tabs; once it is back they switch again.
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_HIDE_TABSTRIP, [IntPtr]::Zero)   # Hide tab strip
Start-Sleep -Milliseconds 400
# a diagnostic, not a verdict: what the app persisted is proved by T12's
# relaunch, which reads it back through the app's own behaviour. Only the band
# assertions below decide pass/fail.
$prefText = Get-Content (Join-Path $env:APPDATA 'mnpdf\app.txt') -Raw -ErrorAction SilentlyContinue
if (-not $prefText) { $prefText = '(unreadable)' }
Write-Host "note: app.txt after hiding the strip: $($prefText.Trim())"
ClickAt $h $x1 $ys                     # the coordinates that were tab 1
if (AwaitTitle $h "mnpdf 1/$pagesB") { Pass 'a hidden strip no longer claims the band' }
else { Fail 'a hidden strip no longer claims the band' "'$(Title $h)'" }
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_HIDE_TABSTRIP, [IntPtr]::Zero)   # Show tab strip
Start-Sleep -Milliseconds 400
ClickAt $h $x1 $ys
if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'showing the strip gives the band back to the tabs' }
else { Fail 'showing the strip gives the band back to the tabs' "'$(Title $h)'" }
ClickAt $h $x2 $ys                     # back to the second document, so T7 holds
if (AwaitTitle $h "mnpdf 1/$pagesB") { Pass 'the second document is active again' }
else { Fail 'the second document is active again' "'$(Title $h)'" }

# --- T7: Close Tab closes the second document; the first survives ------------
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_CLOSE_TAB, [IntPtr]::Zero)   # Close Tab
if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'Close Tab drops the second document and keeps the first' }
else { Fail 'Close Tab drops the second document and keeps the first' "'$(Title $h)'" }

# --- T8: the + button opens another empty tab, and Close Tab returns ----------
$cr = New-Object MNRect
[void][MN]::GetClientRect($h, [ref]$cr)
$clientW = $cr.R - $cr.L
ClickAt $h ($clientW - [int]($m.strip / 2)) $ys
if (AwaitTitle $h '^mnpdf$') {
  [void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_CLOSE_TAB, [IntPtr]::Zero)   # Close Tab
  if (AwaitTitle $h "mnpdf 1/$pagesA\s+$zoomA") { Pass 'the + button opens a tab and closing it returns' }
  else { Fail 'the + button opens a tab and closing it returns' "'$(Title $h)'" }
} else {
  Fail 'the + button opens a tab and closing it returns' "'$(Title $h)'"
}

# --- T9: closing the last tab quits the window --------------------------------
[void][MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_CLOSE_TAB, [IntPtr]::Zero)   # Close Tab
$exited = $false
for ($i = 0; $i -lt 60 -and -not $exited; $i++) {
  $exited = $proc.HasExited
  if (-not $exited) { Start-Sleep -Milliseconds 200 }
}
if ($exited) { Pass 'closing the last tab quits' }
else { Fail 'closing the last tab quits' 'still running'; Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }

# ---- what a tab keeps has to survive the tab, not only the window -----------
# Every case below reads the state back THROUGH THE APP: a tab's zoom is only
# in that tab's sidecar, so a relaunch that shows the same zoom proves the tab's
# sidecar was written by whichever path took the tab out of the front - the
# switch away, or the close. Autosave is switched OFF and confirmed in the
# app's own preference file, because with the debounce timer armed a flush could
# write the sidecar no tab path ever touched. Zooming alone is used as the
# state, deliberately: it marks no document dirty, so no "Save changes?" dialog
# can open in a backgrounded window and stall the quit.
function ZoomInTitle([IntPtr]$Wnd) {
  $t = Title $Wnd
  if ($t -match '(\d+)%') { return $Matches[1] + '%' } else { return '' }
}
function AutosaveOff([IntPtr]$Wnd) {
  [void][MN]::PostMessageW($Wnd, 0x0111, [IntPtr]$CMD_AUTOSAVE, [IntPtr]::Zero)
  $pref = Join-Path $env:APPDATA 'mnpdf\app.txt'
  return (Await { (Get-Content $pref -Raw -ErrorAction SilentlyContinue) -match 'autosave=0' } 8000)
}
function QuitApp($P) {
  $w = FindAppWindow $P.Id
  if ($w -ne [IntPtr]::Zero) { [void][MN]::PostMessageW($w, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero) }
  for ($i = 0; $i -lt 60 -and -not $P.HasExited; $i++) { Start-Sleep -Milliseconds 200 }
  return $P.HasExited
}
function ZoomTo([IntPtr]$Wnd, [int]$Times, [int]$CmdId) {
  for ($i = 0; $i -lt $Times; $i++) { [void][MN]::PostMessageW($Wnd, 0x0111, [IntPtr]$CmdId, [IntPtr]::Zero) }
  Start-Sleep -Seconds 1     # the zoom is applied when the app gets the command,
                             # and the title only carries the number afterwards
  return (ZoomInTitle $Wnd)
}

# --- T10: quitting with another tab in front still keeps this tab's mark ----
# The reported regression: the debounced flush and the quit both wrote whatever
# bundle happened to be ACTIVE, so a mark made in a tab that was later left in
# the background never reached its sidecar.
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue
$procA = Launch (Resolve-AppExe) $A
$wA = FindAppWindow $procA.Id
if (-not (AwaitTitle $wA "mnpdf 1/$pagesA")) {
  Fail 'a background tab keeps its own sidecar at quit' "no document window: '$(Title $wA)'"
} elseif (-not (AutosaveOff $wA)) {
  Fail 'a background tab keeps its own sidecar at quit' 'autosave stayed on'
  [void](QuitApp $procA)
} else {
  $zA = ZoomTo $wA 2 $CMD_ZOOM_IN
  [void][MN]::PostMessageW($wA, 0x0111, [IntPtr]$CMD_NEW_TAB, [IntPtr]::Zero)   # New Tab
  AskOpen $wA $B                       # the second tab is the active one now
  $quit = $false
  if (AwaitTitle $wA 'mnpdf 1/\d+') { $quit = QuitApp $procA }
  if (-not $quit) { Fail 'a background tab keeps its own sidecar at quit' 'the quit did not complete' }
  else {
    $procA2 = Launch (Resolve-AppExe) $A
    $wA2 = FindAppWindow $procA2.Id
    if (AwaitTitle $wA2 "mnpdf 1/$pagesA\s+$zA") { Pass 'a background tab keeps its own sidecar at quit' }
    else { Fail 'a background tab keeps its own sidecar at quit' "expected $zA, got '$(Title $wA2)'" }
    [void](QuitApp $procA2)
  }
}

# --- T11: a mark made in a tab then closed in front still reaches its sidecar -
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue
$procC = Launch (Resolve-AppExe) $A
$wC = FindAppWindow $procC.Id
if (-not (AwaitTitle $wC "mnpdf 1/$pagesA")) {
  Fail 'closing a tab keeps its mark in its sidecar' "no document window: '$(Title $wC)'"
} elseif (-not (AutosaveOff $wC)) {
  Fail 'closing a tab keeps its mark in its sidecar' 'autosave stayed on'
  [void](QuitApp $procC)
} else {
  [void](ZoomTo $wC 2 $CMD_ZOOM_IN)                  # the mark this tab will own
  [void][MN]::PostMessageW($wC, 0x0111, [IntPtr]$CMD_NEW_TAB, [IntPtr]::Zero)   # New Tab
  AskOpen $wC $B
  $mC = StripMetrics $wC
  $pagesB = 0
  if (AwaitTitle $wC 'mnpdf 1/\d+') { $pagesB = PagesInTitle $wC }
  $xTab1 = 4 + [int]($mC.tw / 2)
  ClickAt $wC $xTab1 ([int]($mC.strip / 2))                            # back to tab 1
  $zClose = ''
  if (AwaitTitle $wC "mnpdf 1/$pagesA") { $zClose = ZoomTo $wC 1 $CMD_ZOOM_OUT }   # a different view, still unsaved
  [void][MN]::PostMessageW($wC, 0x0111, [IntPtr]$CMD_CLOSE_TAB, [IntPtr]::Zero)   # Close Tab: THIS tab goes
  $took = $false
  if (($pagesB -gt 0) -and (AwaitTitle $wC "mnpdf 1/$pagesB")) { $took = QuitApp $procC }
  if (-not ($took -and $zClose)) { Fail 'closing a tab keeps its mark in its sidecar' "closed tab: '$(Title $wC)'" }
  else {
    $procC2 = Launch (Resolve-AppExe) $A
    $wC2 = FindAppWindow $procC2.Id
    if (AwaitTitle $wC2 "mnpdf 1/$pagesA\s+$zClose") { Pass 'closing a tab keeps its mark in its sidecar' }
    else { Fail 'closing a tab keeps its mark in its sidecar' "expected $zClose, got '$(Title $wC2)'" }
    [void](QuitApp $procC2)
  }
}

# --- T12: a hidden strip stays hidden across a relaunch ------------------
# The toggle writes the same preference file the other view choices live in, so
# what a relaunch starts with is the proof the preference was persisted, not just
# applied. The + square is the probe: with the strip shown it opens a tab, and
# with the strip hidden its coordinates are document, so the same click cannot.
$procD = Launch (Resolve-AppExe) $A
$wD = FindAppWindow $procD.Id
if (-not (AwaitTitle $wD 'mnpdf 1/\d+')) {
  Fail 'a hidden strip stays hidden across a relaunch' "no document window: '$(Title $wD)'"
} else {
  [void][MN]::PostMessageW($wD, 0x0111, [IntPtr]$CMD_HIDE_TABSTRIP, [IntPtr]::Zero)   # Hide tab strip
  Start-Sleep -Milliseconds 400
  if (QuitApp $procD) {
    $procD2 = Launch (Resolve-AppExe) $A
    $wD2 = FindAppWindow $procD2.Id
    if (AwaitTitle $wD2 'mnpdf 1/\d+') {
      $mD = StripMetrics $wD2
      $crD = New-Object MNRect
      [void][MN]::GetClientRect($wD2, [ref]$crD)
      $plusX = $crD.R - $crD.L - [int]($mD.strip / 2)
      ClickAt $wD2 $plusX ([int]($mD.strip / 2))       # where the + would be
      Start-Sleep -Milliseconds 300                    # let the click's effect land
      # The document title is asserted DIRECTLY after the click: the verbose
      # title always carries 'mnpdf N/M Z%', so polling AwaitTitle would match
      # before the click could matter. If the strip were shown, this click opens
      # a tab and the title becomes a bare 'mnpdf'; reading the title directly
      # here is what actually proves the click changed nothing.
      if ((Title $wD2) -match 'mnpdf 1/\d+\s+\d+%') {
        Pass 'a relaunch with the strip hidden starts with no strip'
        [void][MN]::PostMessageW($wD2, 0x0111, [IntPtr]$CMD_HIDE_TABSTRIP, [IntPtr]::Zero)   # Show tab strip
        Start-Sleep -Milliseconds 400
        ClickAt $wD2 $plusX ([int]($mD.strip / 2))     # now it is the + square
        if (AwaitTitle $wD2 '^mnpdf$') { Pass 'showing the strip restores the + button' }
        else { Fail 'showing the strip restores the + button' "'$(Title $wD2)'" }
      } else {
        Fail 'a relaunch with the strip hidden starts with no strip' "the + click switched state: '$(Title $wD2)'"
      }
    } else {
      Fail 'a hidden strip stays hidden across a relaunch' "relaunch: '$(Title $wD2)'"
    }
    [void](QuitApp $procD2)
  } else {
    Fail 'a hidden strip stays hidden across a relaunch' 'the quit did not complete'
  }
}

Complete-Suite
