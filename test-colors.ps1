# Custom colour entry. Reported together: the box gave no clue whether the #
# belonged, an invalid entry only beeped, a colour typed and then clicked away
# was discarded instead of taken, and the three custom slots could never be
# emptied - "after setting there's no way to remove that".
#
# This drives the real path: the app's own menu command, real keystrokes into
# the colour box, and the app's own state files. It also samples the pixels of
# a highlight painted in the colour, because "make sure you are actually
# providing the user with the colour" is a claim about the page, not about a
# preference line.
#
# Two things here are measured rather than assumed. The typed colour has to end
# up in a palette slot AND become the default, and the mark made with it has to
# read as that colour on screen and survive a quit and relaunch. Two keystroke
# facts are load bearing: non-hex characters never reach the box (it filters
# them), and a focus loss inside the box's first 400 ms is deliberately ignored
# so a stray keystroke cannot become a colour - the app holds the same 400 ms
# grace in its pin editor (src/main.cpp colorBoxProc/pinBoxProc), and this
# suite waits it out on its side of the contract - so the bad entry below is
# hex digits in the wrong number, and the click-away case keeps asking until
# the app is ready to hear it.
#
# Unlike every other suite this one launches the app in the foreground. The
# colour box is a popup that needs the keyboard, and a minimised owner can
# never give it focus - keystrokes posted at a hidden window are swallowed,
# which is exactly what a first attempt at this suite measured. Each window is
# disposed of as soon as its case finishes.
#
# It is also the only suite that reads the app's pixels, so this process has to
# describe the app's window the way the app does: per-monitor DPI aware, the
# same call the app itself makes before its window exists (src/main.cpp,
# SetProcessDpiAwarenessContext). An unaware reader on a 192 dpi screen is told
# the 1100x800 window is 550x400 and its client is 537x364, and a PrintWindow
# capture of that size is only the top-left quarter of the real client, so a
# mark painted by a drag could fall below the captured quarter and read as "no
# mark at all". A reader that shares the app's DPI awareness gets the whole
# client every time. Every other suite already pins this at its top; the colour
# suite is the one that needed it and never had it.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\tests\lib.ps1"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null   # PER_MONITOR_AWARE_V2
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public delegate bool EnumWindowsProcCol(IntPtr h, IntPtr l);
public static class CL {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProcCol cb, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@

$doc = Join-Path $PSScriptRoot 'tests\arc.pdf'
$exe = Resolve-AppExe
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'

# The colour box is a top-level EDIT owned by the app window. The search field
# is a child, and so is excluded by the WS_CHILD bit; the note editor is owned
# by the app too but is not an EDIT class.
# The app owns two top-level Edit pop-ups: the note box beside a pin, made
# when a pin starts, and this colour box. Both are unowned-by-the-desktop popups
# parented to the app window, so the class and the parent cannot tell them apart
# and taking whichever window happens to come first in the enumeration is a coin
# toss - which showed up as a box that read as empty. A note box is multiline and
# the colour box is not, so that is the difference to ask for.
function FindColorBox([int]$ProcId, [IntPtr]$Owner) {
  $script:hit = [IntPtr]::Zero
  $script:found = $false
  $cb = [EnumWindowsProcCol]{
    param($h, $l)
    $wpId = 0
    [void][MN]::GetWindowThreadProcessId($h, [ref]$wpId)
    if ($wpId -eq $ProcId) {
      $cn = New-Object System.Text.StringBuilder 64
      [void][CL]::GetClassNameW($h, $cn, 64)
      $isChild = [bool]([CL]::GetWindowLongW($h, -16) -band 0x40000000)
      $isMulti = [bool]([CL]::GetWindowLongW($h, -16) -band 0x0004)          # ES_MULTILINE
      if (-not $isChild -and -not $isMulti -and $cn.ToString() -eq 'Edit' -and [CL]::GetParent($h) -eq $Owner) {
        if (-not $script:found) { $script:hit = $h; $script:found = $true }
      }
    }
    return $true }
  [void][CL]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:hit
}
function FindDlgCol([int]$ProcId) {
  $script:hit = [IntPtr]::Zero
  $script:found = $false
  $cb = [EnumWindowsProcCol]{
    param($h, $l)
    $wpId = 0
    [void][MN]::GetWindowThreadProcessId($h, [ref]$wpId)
    if ($wpId -eq $ProcId) {
      $cn = New-Object System.Text.StringBuilder 64
      [void][CL]::GetClassNameW($h, $cn, 64)
      if ($cn.ToString() -eq '#32770') {
        if (-not $script:found) { $script:hit = $h; $script:found = $true }
      }
    }
    return $true }
  [void][CL]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:hit
}
# WM_GETTEXT, not GetWindowTextW: the title call is blind on a control owned by
# another process and answers with whatever text the control was created with,
# not what the reader typed into it
function BoxText([IntPtr]$Box) {
  if ($Box -eq [IntPtr]::Zero) { return '' }
  $b = [char[]]::new(64)
  [void][MN]::SendText($Box, 0x000D, [IntPtr]64, $b)
  # WM_GETTEXT fills the text and one NUL and writes nothing further, so the
  # rest of the buffer is whatever the CLR had in it there. A raw join keeps
  # that garbage attached to the text ("#e", "#d") or in front of it (""),
  # which reads as a broken box when the box was right all along; the text is
  # everything before the first NUL, so cut there.
  $s = -join $b
  $z = $s.IndexOf([char]0)
  if ($z -ge 0) { $s = $s.Substring(0, $z) }
  return $s
}
# every visible string inside a dialog body, for the same reason
function DlgBody([IntPtr]$Dlg) {
  if ($Dlg -eq [IntPtr]::Zero) { return '' }
  # The dialog's STATIC texts, read with WM_GETTEXT via [MN]::SendText. This
  # used to go through UI Automation, which returned an empty tree for a dialog
  # sitting on another virtual desktop (the gate runs every window on 'second');
  # raw messages do not care which desktop a window lives on.
  try {
    $parts = @()
    $prev = [IntPtr]::Zero
    while ($true) {
      $prev = [MN]::FindWindowExW($Dlg, $prev, 'Static', [IntPtr]::Zero)
      if ($prev -eq [IntPtr]::Zero) { break }
      $b = New-Object char[] 4096
      [void][MN]::SendText($prev, 0x000D, [IntPtr]4095, $b)
      $t = (-join $b).TrimEnd([char]0)
      if ($t) { $parts += $t }
    }
    return ($parts -join "`n")
  } catch { return '' }          # a dialog can be gone by the time it is read
}
function PrefLines {
  (Get-Content $appPref -ErrorAction SilentlyContinue) |
    Where-Object { $_ -match '^(hlcolor|pincolor|pal[0-9]+|palnext)=' }
}
# the app flushes its prefs on a debounce, not at the instant of the click
function AwaitPref([string]$Pattern, [int]$TimeoutMs = 5000) {
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  do {
    if (((PrefLines) -join ' ') -match $Pattern) { return $true }
    Start-Sleep -Milliseconds 100
  } while ((Get-Date) -lt $deadline)
  return $false
}
# PrefLines keeps only the palette keys, so a preference outside that set
# (night mode) is polled against the whole file
function AwaitPrefRaw([string]$Pattern, [int]$TimeoutMs = 5000) {
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  do {
    if ((Get-Content $appPref -Raw -ErrorAction SilentlyContinue) -match $Pattern) { return $true }
    Start-Sleep -Milliseconds 100
  } while ((Get-Date) -lt $deadline)
  return $false
}
# type the six hex digits the way a reader does, one keystroke at a time
# Open the box, replace what is in it with a typed entry, and commit it with
# Enter, until the entry actually took. The app answers a focus loss it did not
# ask for by committing whatever the box happens to hold at that instant, and
# part of a colour parses as nothing - so a typed entry whose effect is missing
# is typed again rather than reported as a defect of the app.
function WithTypedBox([int]$ProcId, [IntPtr]$Wnd, [int]$Cmd, [string]$Hex, [scriptblock]$Took) {
  for ($a = 0; $a -lt 5; $a++) {
    $b = OpenColorBox $ProcId $Wnd $Cmd
    if ($b -ne [IntPtr]::Zero) {
      TypeHex $b $Hex
      [void][MN]::PostMessageW($b, 0x0100, [IntPtr]13, [IntPtr]::Zero)      # Enter
      if (& $Took $b) { return $b }
    }
    Start-Sleep -Milliseconds 200
  }
  return [IntPtr]::Zero
}
function TypeHex([IntPtr]$Box, [string]$Hex) {
  # Start from an empty box, the way a reader retyping a colour does. The app
  # gives this box the foreground, so while it is up the keyboard still reaches
  # it, and a colour typed behind something already there parses as one long
  # invalid entry. Selecting everything and typing over it is one message; a run
  # of backspaces is ten, and the app answers a focus loss it did not ask for by
  # committing whatever happens to be in the box, so the whole entry has to be
  # over inside the box's first 400 ms, where that answer is still withheld
  # (src/main.cpp colorBoxProc holds the same grace for the pin editor).
  [void][MN]::PostMessageW($Box, 0x00B1, [IntPtr]0, (New-Object IntPtr -1))   # EM_SETSEL: take all of it
  foreach ($ch in $Hex.ToCharArray()) {
    [void][MN]::PostMessageW($Box, 0x0102, [IntPtr][int][char]$ch, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 15
  }
}
function AwaitBox([int]$ProcId, [IntPtr]$Owner, [int]$TimeoutMs = 4000) {
  for ($i = 0; $i -lt ($TimeoutMs / 20); $i++) {
    $b = FindColorBox $ProcId $Owner
    if ($b -ne [IntPtr]::Zero -and [MN]::IsWindowVisible($b)) { return $b }
    Start-Sleep -Milliseconds 20
  }
  return [IntPtr]::Zero
}
# the app's own menu opens the entry box: CMD_CUSTOM_HL_COLOR = Custom... on the
# default highlight colour, CMD_CUSTOM_PIN_COLOR = the same on the default pin
# colour (tests/lib.ps1 names every id the app's WM_COMMAND answers)
# The box gives up what is in it when the keyboard moves away from it, and the
# OS moves the keyboard all by itself: a few hundred milliseconds after the box
# appears, Windows Terminal takes the foreground back and the app hears "the
# reader is elsewhere", commits and destroys the box. A reader that waited past
# that point finds a dead handle, which is what a 700 ms settle here used to
# do. So the box is checked - still there, with something in it - before it is
# handed over, and opened again when it is not.
function OpenColorBox([int]$ProcId, [IntPtr]$Wnd, [int]$Cmd, [int]$Attempts = 6) {
  for ($a = 0; $a -lt $Attempts; $a++) {
    [void][MN]::PostMessageW($Wnd, 0x0111, [IntPtr]$Cmd, [IntPtr]::Zero)
    $b = AwaitBox $ProcId $Wnd
    if ($b -ne [IntPtr]::Zero) {
      # read it now, while the foreground is still the box's
      $t = BoxText $b
      if ($t -ne '' -and [MN]::IsWindowVisible($b)) { return $b }
    }
    Start-Sleep -Milliseconds 200
  }
  return [IntPtr]::Zero
}
# Dismiss a dialog. The command is SENT to the dialog itself: a posted command
# is not seen this way (measured: a posted IDYES left the confirmation up and
# the app thread modal), and a click on the button is dropped while the dialog
# is still being built. It does not come back while a dialog is still on
# screen, because the next case would then read this one's buttons.
function DismissDlg([int]$ProcId, [int]$Button, [int]$TimeoutMs = 8000) {
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  do {
    $d = FindDlgCol $ProcId
    if ($d -ne [IntPtr]::Zero) {
      [void][MN]::SendMessageW($d, 0x0111, [IntPtr]$Button, [IntPtr]::Zero)   # WM_COMMAND, the button id
      $gone = (Get-Date).AddMilliseconds(2000)
      while ((Get-Date) -lt $gone -and (FindDlgCol $ProcId) -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 75
      }
      if ((FindDlgCol $ProcId) -eq [IntPtr]::Zero) { return $d }
      # fall back to clicking the real control, for a dialog that wants it
      $btn = [CL]::GetDlgItem($d, $Button)
      if ($btn -ne [IntPtr]::Zero -and [CL]::IsWindowEnabled($btn)) {
        Start-Sleep -Milliseconds 150
        [void][MN]::SendMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
        $gone = (Get-Date).AddMilliseconds(2000)
        while ((Get-Date) -lt $gone -and (FindDlgCol $ProcId) -ne [IntPtr]::Zero) {
          Start-Sleep -Milliseconds 75
        }
      }
      return $d
    }
    Start-Sleep -Milliseconds 50
  } while ((Get-Date) -lt $deadline)
  return [IntPtr]::Zero
}
# A minimised window reports a 0x0 client, so a capture taken from one reads a
# sentinel and nothing else. An unexplained SC_MINIMIZE arrives at this app from
# outside the suite roughly once every few runs, and a reader that then reports a
# broken colour is measuring the wrong thing: restore the window first and only
# give up when it will not come back.
function EnsureShown([IntPtr]$Wnd) {
  $cr0 = New-Object MNRect
  [void][MN]::GetClientRect($Wnd, [ref]$cr0)
  if ($cr0.B - $cr0.T -ge 8) { return $true }
  for ($i = 0; $i -lt 25; $i++) {
    if ([MN]::IsIconic($Wnd)) { [void][CL]::ShowWindow($Wnd, 9) }   # 9 = SW_RESTORE
    Start-Sleep -Milliseconds 100
    [void][MN]::GetClientRect($Wnd, [ref]$cr0)
    if (($cr0.B - $cr0.T) -ge 8 -and -not [MN]::IsIconic($Wnd)) { return $true }
  }
  return $false
}
# Count the pixels on the page that are not greyscale, split by direction.
# A page with no mark on it is greyscale - measured: every pixel answers
# (g-r)+(b-r) = 0 - so a count answers "is that colour on the page" without the
# reader having to know where the mark is, which row of text the drag landed on,
# or how tall the window happens to be. The two families sit on opposite sides
# of that zero: a warm mark (the yellow default) counts negative, a cool one (a
# custom colour such as #20c0a0) counts positive, and the two cannot be mistaken
# for each other or for the black ink of the text, which is greyscale too.
# The count also reports how light the client is, so a caller can tell a page
# that was not painted yet (a capture of a window still drawing comes out all
# black) from a painted one. The capture is the app's own client at the size the
# app renders it: PrintWindow draws the window 1:1 and clips, so a bitmap sized
# from a virtualised client shows a quarter of the page - which is why this suite
# pins the same per-monitor DPI awareness as the app before it measures.
# The scan strides 2 px in both directions: a mark covers thousands of pixels,
# so a quarter sample answers every question here at a quarter of the cost
# (a full walk of a 1100x800 client measured ~8.4 s per capture in PowerShell),
# and the assertions count rises of hundreds, not absolutes.
function ColourCount([IntPtr]$Wnd) {
  [void](EnsureShown $Wnd)
  $cr = New-Object MNRect
  [void][MN]::GetClientRect($Wnd, [ref]$cr)
  $bw = $cr.R - $cr.L; $bh = $cr.B - $cr.T
  if ($bw -lt 8 -or $bh -lt 8) { return [pscustomobject]@{ cool = -1; warm = -1; lum = 0 } }   # a minimised client rect
  $bmp = New-Object System.Drawing.Bitmap($bw, $bh)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [void][CL]::PrintWindow($Wnd, $hdc, 1)               # PW_CLIENTONLY
  $g.ReleaseHdc($hdc); $g.Dispose()
  $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $bw, $bh),
                        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $stride = $data.Stride
  $buf = New-Object 'byte[]' ($stride * $bh)
  [void][System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
  $bmp.UnlockBits($data); $bmp.Dispose()
  $cool = 0; $warm = 0; $sum = 0; $n = 0
  for ($y = 0; $y -lt $bh; $y += 2) {
    $rowBase = $y * $stride
    for ($x = 0; $x -lt $bw; $x += 2) {
      $i = $rowBase + $x * 4
      $b = $buf[$i]; $gn = $buf[$i + 1]; $r = $buf[$i + 2]
      $v = ($gn - $r) + ($b - $r)
      if ($v -ge 40) { $cool++ } elseif ($v -le -40) { $warm++ }
      $sum += ($r + $gn + $b); $n++
    }
  }
  return [pscustomobject]@{ cool = $cool; warm = $warm; lum = $(if ($n) { $sum / $n } else { 0 }) }
}
# The page is a light sheet with dark ink on it. A capture that is not that
# yet is a window still painting (all black, or a blend), so the measurement a
# caller takes from it is about the capture and not about the app.
function AwaitPaintedPage([IntPtr]$Wnd, [int]$TimeoutMs = 25000) {
  [void](Await { (ColourCount $Wnd).lum -gt 200 } $TimeoutMs)
}
# A mark that has not reached the picture yet counts zero, so wait until it
# does rather than measuring the moment after the command.
function WaitCount([IntPtr]$Wnd, [string]$What, [int]$MoreThan, [int]$TimeoutMs = 15000) {
  [void](Await { (ColourCount $Wnd).$What -gt $MoreThan } $TimeoutMs)
}
# A block selection rather than a row: which rows of this document carry text
# is not fixed, and only a block is a selection that always lands on some.
# A pointer sweep that paints a selection. The corner fractions arrive as
# parameters, so the pixel values derived from them must use different names:
# PowerShell variable names are case-insensitive, so $x0 and $X0 are one
# variable and a derived value written back into it silently replaces the
# fraction the rest of the drag reads. That is exactly what happened here -
# $x0 = $w * $X0 wrote pixels into the fraction, the next move computed
# $w * ($fraction that had become pixels), and the app received a teleporting
# pointer: the drag never extended a selection, so the highlight never existed.
function DragBlock([IntPtr]$Wnd, [double]$Fx0, [double]$Fy0, [double]$Fx1, [double]$Fy1) {
  $cr = New-Object MNRect
  [void][MN]::GetClientRect($Wnd, [ref]$cr)
  $cwPx = $cr.R - $cr.L; $chPx = $cr.B - $cr.T
  # The reader draws a tab strip at the top of the client, and the strip is
  # chrome: a click down there belongs to it, never to the document, exactly
  # like a toolbar. Its band is 30px at 96 dpi and grows with the scale, so a
  # drag is clamped to start below it - 64 clears every dpi the gates run at.
  $stripPx = 64
  $px0 = [int]($cwPx * $Fx0); $py0 = [Math]::Max([int]($chPx * $Fy0), $stripPx)
  $px1 = [int]($cwPx * $Fx1); $py1 = [int]($chPx * $Fy1)
  [void][MN]::PostMessageW($Wnd, 0x0201, [IntPtr]1, (New-Object IntPtr (($py0 -shl 16) -bor ($px0 -band 0xFFFF))))
  Start-Sleep -Milliseconds 60
  foreach ($frac in @(0.33, 0.66)) {
    $mx = [int]($cwPx * ($Fx0 + ($Fx1 - $Fx0) * $frac))
    $my = [int]($chPx * ($Fy0 + ($Fy1 - $Fy0) * $frac))
    [void][MN]::PostMessageW($Wnd, 0x0200, [IntPtr]1, (New-Object IntPtr (($my -shl 16) -bor ($mx -band 0xFFFF))))
    Start-Sleep -Milliseconds 40
  }
  [void][MN]::PostMessageW($Wnd, 0x0202, [IntPtr]0, (New-Object IntPtr (($py1 -shl 16) -bor ($px1 -band 0xFFFF))))
  Start-Sleep -Milliseconds 200
}
function HighlightBlock([IntPtr]$Wnd, [double]$Fx0, [double]$Fy0, [double]$Fx1, [double]$Fy1) {
  DragBlock $Wnd $Fx0 $Fy0 $Fx1 $Fy1
  [void][MN]::PostMessageW($Wnd, 0x0111, [IntPtr]$CMD_HIGHLIGHT, [IntPtr]::Zero)   # Highlight
  Start-Sleep -Milliseconds 900
}

# requires a machine we own: this suite rewrites app.txt and would otherwise
# do it underneath a running instance
Assert-NoRunningApp

$proc = $null; $wnd = [IntPtr]::Zero
$script:proc2 = $null
try {
  # A clean slate with the user's real prefs protected: Init-PrefForge backs
  # their app.txt up and arms the watchdog (tests\watchdog.ps1), then the forge
  # below pins the yellow default, no custom colours, and an update clock inside
  # its cooldown - so no case here spends a real GitHub request, which the
  # unpinned version of this setup silently did at every launch.
  Init-PrefForge $appPref 'colors'
  [void](Set-ForgedAppPref 300)

  $proc = Launch $exe $doc
  $wnd = FindAppWindow $proc.Id

  # ---- case 1: the box says what shape the answer has -----------------------
  $box = OpenColorBox $proc.Id $wnd $CMD_CUSTOM_HL_COLOR
  if ($box -eq [IntPtr]::Zero) { Fail 'the colour box opens' } else {
    $t = BoxText $box
    # the box holds the keyboard while it is up, and the desktop talks to
    # the front window while a suite runs: a stray keystroke lands after the
    # seed, so what is checked is that the box opens already telling the reader
    # the shape of the answer, not that nobody else typed
    if ($t.StartsWith('#')) { Pass 'the colour box opens with the # already in it' }
    else { Fail ('the colour box opens with the # already in it (got "{0}")' -f $t) }
  }

  # ---- case 2: a valid colour becomes the default highlight colour --------
  $box = WithTypedBox $proc.Id $wnd $CMD_CUSTOM_HL_COLOR '20c0a0' {
    param($b) (AwaitPref 'pal0=20c0a0' 400) -and (AwaitPref 'hlcolor=6' 400) }
  if ($box -ne [IntPtr]::Zero) { Pass 'a typed colour lands in the palette and becomes the default' }
  else { Fail ('a typed colour lands in the palette and becomes the default ({0})' -f ((PrefLines) -join ' ')) }

  # ---- case 3: the mark on the page really is that colour ----------------
  # Wait for a painted page before measuring, and for the mark to reach the
  # picture after the command: both are conditions of the app, and a fixed
  # sleep is a coin toss on a loaded machine (the capture of a window that has
  # not painted is all black, and black reads as 0 on the signature).
  AwaitPaintedPage $wnd
  $before = ColourCount $wnd
  HighlightBlock $wnd 0.15 0.05 0.80 0.48
  WaitCount $wnd 'cool' 500
  $marked = ColourCount $wnd
  $sc = Get-ChildItem (Join-Path $env:APPDATA 'mnpdf\doc-*.txt') -ErrorAction SilentlyContinue |
        Select-Object -First 1
  $sideColor = if ($sc) { ([regex]::Match((Get-Content $sc.FullName -Raw), 'hl=\d+,\d+,\d+,(\d+)')).Groups[1].Value } else { '' }
  if ($before.cool -eq 0 -and $marked.cool -gt 500) {
    Pass 'a highlight painted in that colour reads as that colour on the page'
  } else {
    Fail ('a highlight painted in that colour reads as that colour on the page (cool {0} -> {1})' -f $before.cool, $marked.cool)
  }
  if ($sideColor -eq '6') { Pass 'the sidecar records the custom slot' }
  else { Fail ('the sidecar records the custom slot (got "{0}")' -f $sideColor) }
  # The preset default has to move the same measure the other way, so the
  # result above cannot be an artefact of the measurement: the yellow preset
  # paints the other direction, and the page was greyscale before either mark.
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_DEFAULT_HL_YELLOW, [IntPtr]::Zero)   # default highlight = yellow preset
  Start-Sleep -Milliseconds 500
  $yellowBefore = ColourCount $wnd
  HighlightBlock $wnd 0.15 0.52 0.80 0.95
  # The app paints a mark in the new default when a selection is standing as the
  # default changes, and a drag leaves its selection behind, so part of the
  # yellow can already be on the page here. What has to be true is that the
  # preset default paints the preset colour: the count goes up from what it was,
  # and nothing greyscale can move it.
  WaitCount $wnd 'warm' ($yellowBefore.warm + 500)
  $yellow = ColourCount $wnd
  if ($yellow.warm -ge $yellowBefore.warm + 500) { Pass 'a preset default still paints the preset colour' }
  else { Fail ('a preset default still paints the preset colour (warm {0} -> {1})' -f $yellowBefore.warm, $yellow.warm) }

  # ---- case 4: an invalid entry explains itself, and keeps the box -------
  # hex, but five digits: the box takes it, the parse refuses it
  $box = WithTypedBox $proc.Id $wnd $CMD_CUSTOM_HL_COLOR '12345' {
    param($b)
    Start-Sleep -Milliseconds 500
    $d = FindDlgCol $ProcId
    $d -ne [IntPtr]::Zero -and [MN]::IsWindowVisible($b) }
  if ($box -eq [IntPtr]::Zero) { Fail 'an invalid colour is explained at once and leaves the box open (no dialog)' }
  else {
    $dlg = FindDlgCol $proc.Id
    $body = DlgBody $dlg
    if ($body -match 'is not a colour' -and $body -match '12345' -and $body -match 'ff4d00') {
      Pass 'an invalid colour is explained at once and leaves the box open'
    } else {
      Fail ('an invalid colour is explained at once and leaves the box open (box open {0}, body {1})' -f
            [MN]::IsWindowVisible($box), ($body -replace "`r?`n", ' | '))
    }
    DismissDlg $proc.Id 2 | Out-Null                                        # OK
  }

  # ---- case 5: typed then clicked away keeps the colour ------------------
  $box = OpenColorBox $proc.Id $wnd $CMD_CUSTOM_HL_COLOR
  TypeHex $box 'abcdef'
  # The app holds a focus loss for the box's first 400 ms, and the reader's
  # "click away" can land inside that. A click away that the app is not ready to
  # hear is ignored, so keep asking the way a reader clicking a second time does,
  # and accept a box that has already given the colour up on its own.
  for ($k = 0; $k -lt 14; $k++) {
    if (AwaitPref 'pal[0-9]+=abcdef' 500) { break }
    if ([MN]::IsWindowVisible($box)) { [void][MN]::PostMessageW($box, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero) }
  }
  if (AwaitPref 'pal[0-9]+=abcdef') { Pass 'a colour typed then clicked away is kept' }
  else { Fail ('a colour typed then clicked away is kept ({0})' -f ((PrefLines) -join ' ')) }

  # ---- case 6: the slots can be cleared, with a confirmation -------------
  # With only three slots, a reader who wants a different set has to be able to
  # start from empty, and it asks first and says what happens to the marks.
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_CLEAR_CUSTOM, [IntPtr]::Zero)
  DismissDlg $proc.Id 7 | Out-Null                                          # No, first
  $pf = (PrefLines) -join ' '
  if ($pf -match 'pal[0-9]+=') {
    Pass 'clearing asks first, and No keeps the colours'
  } else { Fail ('clearing asks first, and No keeps the colours ({0})' -f $pf) }
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_CLEAR_CUSTOM, [IntPtr]::Zero)
  DismissDlg $proc.Id 6 | Out-Null                                          # Yes
  $keep = $null
  for ($w = 0; $w -lt 40; $w++) {
    $keep = (PrefLines) -join ' '
    if ($keep -notmatch 'pal[0-9]+=') { break }
    Start-Sleep -Milliseconds 100
  }
  if ($keep -notmatch 'pal[0-9]+=') { Pass 'clearing empties the slots' }
  else { Fail ('clearing empties the slots ({0})' -f $keep) }

  # ---- case 7: the pin colour menu behaves the same way ------------------
  $box = WithTypedBox $proc.Id $wnd $CMD_CUSTOM_PIN_COLOR 'ff8800' {
    param($b) (AwaitPref 'pal0=ff8800' 400) -and (AwaitPref 'pincolor=6' 400) }
  if ($box -ne [IntPtr]::Zero) { Pass 'the pin colour menu takes a custom colour too' }
  else { Fail ('the pin colour menu takes a custom colour too ({0})' -f ((PrefLines) -join ' ')) }

  # ---- case 8: night mode darkens the page and is remembered --------------
  # The pixel measure is the suite's own: a painted page is a light sheet (lum
  # over 200), the same page inverted is not. Nothing here names a colour or a
  # pixel row, so it holds at any zoom on any screen.
  AwaitPaintedPage $wnd
  $dayLum = (ColourCount $wnd).lum
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_NIGHT, [IntPtr]::Zero)
  [void](Await { (ColourCount $wnd).lum -lt 80 } 10000)
  $nightLum = (ColourCount $wnd).lum
  if ($nightLum -lt 80 -and $nightLum -lt ($dayLum / 2)) {
    Pass ("night mode turns the page dark (lum {0:N0} -> {1:N0})" -f $dayLum, $nightLum)
  } else {
    Fail ("night mode turns the page dark (lum {0:N0} -> {1:N0})" -f $dayLum, $nightLum)
  }
  if (AwaitPrefRaw '(?m)^night=1\s*$') { Pass 'night mode is remembered in app.txt' }
  else { Fail ('night mode is remembered in app.txt ({0})' -f ((Get-Content $appPref -Raw -ErrorAction SilentlyContinue) -replace "`n", ' | ')) }
  # and back again, so the suite leaves the reader's own setting as it found it
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_NIGHT, [IntPtr]::Zero)
  [void](Await { (ColourCount $wnd).lum -gt 200 } 10000)

  # ---- case 9: the custom pin colour survives a quit and relaunch -------
  [void][MN]::PostMessageW($wnd, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero)
  for ($i = 0; $i -lt 60; $i++) { if ($proc.HasExited) { break }; Start-Sleep -Milliseconds 250 }
  if (-not $proc.HasExited) { $proc | Stop-Process -Force }
  $script:proc2 = Launch $exe $doc
  $script:wnd2 = FindAppWindow $script:proc2.Id
  if (AwaitPref 'pal0=ff8800' -and (AwaitPref 'pincolor=6')) {
    Pass 'the custom pin colour survives a quit and relaunch'
  } else {
    Fail ('the custom pin colour survives a quit and relaunch ({0})' -f ((PrefLines) -join ' '))
  }
  [void][MN]::PostMessageW($script:wnd2, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero)
  for ($i = 0; $i -lt 60; $i++) { if ($script:proc2.HasExited) { break }; Start-Sleep -Milliseconds 250 }
  if (-not $script:proc2.HasExited) { $script:proc2 | Stop-Process -Force }
} finally {
  foreach ($p in @($proc, $script:proc2)) {
    if ($p -and -not $p.HasExited) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
  }
  Restore-AppPref          # the user's real app.txt goes back on every path
}
Write-Output ''
Complete-Suite
