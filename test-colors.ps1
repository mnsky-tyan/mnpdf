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
# so a stray keystroke cannot become a colour - so the bad entry below is hex
# digits in the wrong number, and the probe waits out a reader's pause before
# clicking away.
#
# Unlike every other suite this one launches the app in the foreground. The
# colour box is a popup that needs the keyboard, and a minimised owner can
# never give it focus - keystrokes posted at a hidden window are swallowed,
# which is exactly what a first attempt at this suite measured. Each window is
# disposed of as soon as its case finishes.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\tests\lib.ps1"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public delegate bool EnumWindowsProcCol(IntPtr h, IntPtr l);
public static class CL {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProcCol cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr cap, [Out] char[] buf);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

$failures = New-Object System.Collections.Generic.List[string]
$doc = Join-Path $PSScriptRoot 'tests\arc.pdf'
$exe = $env:MNPDF_GATE_EXE
if (-not $exe) { $exe = Join-Path $PSScriptRoot 'build\mnpdf.exe' }
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'

function Pass([string]$Name) { Write-Output ("PASS {0}" -f $Name) }
function Fail([string]$Name) { $failures.Add($Name); Write-Output ("FAIL {0}" -f $Name) }

# The colour box is a top-level EDIT owned by the app window. The search field
# is a child, and so is excluded by the WS_CHILD bit; the note editor is owned
# by the app too but is not an EDIT class.
function FindColorBox([int]$ProcId, [IntPtr]$Owner) {
  $script:hit = [IntPtr]::Zero
  $script:found = $false
  $cb = [EnumWindowsProcCol]{
    param($h, $l)
    $wpId = 0
    [void][CL]::GetWindowThreadProcessId($h, [ref]$wpId)
    if ($wpId -eq $ProcId) {
      $cn = New-Object System.Text.StringBuilder 64
      [void][CL]::GetClassNameW($h, $cn, 64)
      $isChild = [bool]([CL]::GetWindowLongW($h, -16) -band 0x40000000)
      if (-not $isChild -and $cn.ToString() -eq 'Edit' -and [CL]::GetParent($h) -eq $Owner) {
        if (-not $found) { $script:hit = $h; $script:found = $true }
      }
    }
    return $true }
  [void][CL]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:hit
}
function FindDlgCol([int]$ProcId) {
  $script:hit = [IntPtr]::Zero
  $found = $false
  $cb = [EnumWindowsProcCol]{
    param($h, $l)
    $wpId = 0
    [void][CL]::GetWindowThreadProcessId($h, [ref]$wpId)
    if ($wpId -eq $ProcId) {
      $cn = New-Object System.Text.StringBuilder 64
      [void][CL]::GetClassNameW($h, $cn, 64)
      if ($cn.ToString() -eq '#32770') {
        if (-not $found) { $script:hit = $h; $script:found = $true }
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
  [void][CL]::SendText($Box, 0x000D, [IntPtr]64, $b)
  return (-join $b).TrimEnd([char]0)
}
# every visible string inside a dialog body, for the same reason
function DlgBody([IntPtr]$Dlg) {
  if ($Dlg -eq [IntPtr]::Zero) { return '' }
  try {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($Dlg)
    if (-not $el) { return '' }
    $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
              [System.Windows.Automation.Condition]::TrueCondition)
    $parts = @()
    foreach ($n in $all) { if ($n.Current.Name) { $parts += $n.Current.Name } }
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
# type the six hex digits the way a reader does, one keystroke at a time
function TypeHex([IntPtr]$Box, [string]$Hex) {
  foreach ($ch in $Hex.ToCharArray()) {
    [void][CL]::PostMessageW($Box, 0x0102, [IntPtr][int][char]$ch, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 25
  }
}
function AwaitBox([int]$ProcId, [IntPtr]$Owner, [int]$TimeoutMs = 4000) {
  for ($i = 0; $i -lt ($TimeoutMs / 20); $i++) {
    $b = FindColorBox $ProcId $Owner
    if ($b -ne [IntPtr]::Zero -and [CL]::IsWindowVisible($b)) { return $b }
    Start-Sleep -Milliseconds 20
  }
  return [IntPtr]::Zero
}
# the app's own menu opens the entry box: 149 = Custom... on the default
# highlight colour, 169 = the same on the default pin colour
function OpenColorBox([int]$ProcId, [IntPtr]$Wnd, [int]$Cmd) {
  [void][CL]::PostMessageW($Wnd, 0x0111, [IntPtr]$Cmd, [IntPtr]::Zero)
  $b = AwaitBox $ProcId $Wnd
  if ($b -ne [IntPtr]::Zero) {
    # The box refuses to commit a focus loss in its first 400 ms, so that a box
    # which never took focus cannot give up a stray keystroke. A reader spends
    # longer than that typing a colour, so the probe waits the same way out.
    Start-Sleep -Milliseconds 700
  }
  return $b
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
      [void][CL]::SendMessageW($d, 0x0111, [IntPtr]$Button, [IntPtr]::Zero)   # WM_COMMAND, the button id
      $gone = (Get-Date).AddMilliseconds(2000)
      while ((Get-Date) -lt $gone -and (FindDlgCol $ProcId) -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 75
      }
      if ((FindDlgCol $ProcId) -eq [IntPtr]::Zero) { return $d }
      # fall back to clicking the real control, for a dialog that wants it
      $btn = [CL]::GetDlgItem($d, $Button)
      if ($btn -ne [IntPtr]::Zero -and [CL]::IsWindowEnabled($btn)) {
        Start-Sleep -Milliseconds 150
        [void][CL]::SendMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
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
# How green-and-blue versus red a band of the page is, as one number: the page
# is white so an untouched band reads 0, a yellow highlight reads clearly
# negative, and #20c0a0 reads clearly positive. A band of the client rather
# than a row, because which row of text a drag lands on depends on the window
# height. The capture is always the whole client: handing PrintWindow a bitmap
# shorter than the client clips the page and the band comes out empty.
function BandSignature([IntPtr]$Wnd, [double]$Y0, [double]$Y1) {
  $cr = New-Object CL+RECT
  [void][CL]::GetClientRect($Wnd, [ref]$cr)
  $w = $cr.R - $cr.L; $h = $cr.B - $cr.T
  if ($w -lt 8 -or $h -lt 8) { return -1000 }          # a minimised client rect
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [void][CL]::PrintWindow($Wnd, $hdc, 1)               # PW_CLIENTONLY
  $g.ReleaseHdc($hdc); $g.Dispose()
  $ymin = [int]($h * $Y0); $ymax = [Math]::Min([int]($h * $Y1), $h - 2)
  $best = -1000
  for ($y = $ymin; $y -lt $ymax; $y += 2) {
    for ($x = 40; $x -lt [Math]::Min(560, $w - 4); $x += 2) {
      $c = $bmp.GetPixel($x, $y)
      $v = ($c.G - $c.R) + ($c.B - $c.R)
      if ($v -gt $best) { $best = $v }
    }
  }
  $bmp.Dispose()
  return $best
}
# A block selection rather than a row: which rows of this document carry text
# is not fixed, and only a block is a selection that always lands on some.
function DragBlock([IntPtr]$Wnd, [double]$X0, [double]$Y0, [double]$X1, [double]$Y1) {
  $cr = New-Object CL+RECT
  [void][CL]::GetClientRect($Wnd, [ref]$cr)
  $w = $cr.R - $cr.L; $h = $cr.B - $cr.T
  $x0 = [int]($w * $X0); $y0 = [int]($h * $Y0)
  $x1 = [int]($w * $X1); $y1 = [int]($h * $Y1)
  [void][CL]::PostMessageW($Wnd, 0x0201, [IntPtr]1, (New-Object IntPtr (($y0 -shl 16) -bor ($x0 -band 0xFFFF))))
  Start-Sleep -Milliseconds 60
  foreach ($f in @(0.33, 0.66)) {
    $xm = [int]($w * ($X0 + ($X1 - $X0) * $f))
    $ym = [int]($h * ($Y0 + ($Y1 - $Y0) * $f))
    [void][CL]::PostMessageW($Wnd, 0x0200, [IntPtr]1, (New-Object IntPtr (($ym -shl 16) -bor ($xm -band 0xFFFF))))
    Start-Sleep -Milliseconds 40
  }
  [void][CL]::PostMessageW($Wnd, 0x0202, [IntPtr]0, (New-Object IntPtr (($y1 -shl 16) -bor ($x1 -band 0xFFFF))))
  Start-Sleep -Milliseconds 200
}
function HighlightBlock([IntPtr]$Wnd, [double]$X0, [double]$Y0, [double]$X1, [double]$Y1) {
  DragBlock $Wnd $X0 $Y0 $X1 $Y1
  [void][CL]::PostMessageW($Wnd, 0x0111, [IntPtr]135, [IntPtr]::Zero)   # Highlight
  Start-Sleep -Milliseconds 900
}

$proc = $null; $wnd = [IntPtr]::Zero
try {
  # A clean slate: the yellow default, no custom colours.
  New-Item -ItemType Directory -Force -Path (Split-Path $appPref) | Out-Null
  Set-Content $appPref "titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`n"

  $proc = Start-Process -FilePath $exe -ArgumentList """$doc""" -PassThru
  $wnd = FindAppWindow $proc.Id
  for ($i = 0; $i -lt 80; $i++) { $wnd = FindAppWindow $proc.Id; if ($wnd -ne [IntPtr]::Zero) { break }; Start-Sleep -Milliseconds 250 }
  Start-Sleep -Milliseconds 900

  # ---- case 1: the box says what shape the answer has -----------------------
  $box = OpenColorBox $proc.Id $wnd 149
  if ($box -eq [IntPtr]::Zero) { Fail 'the colour box opens' } else {
    $t = BoxText $box
    if ($t -eq '#') { Pass 'the colour box opens with the # already in it' }
    else { Fail ('the colour box opens with the # already in it (got "{0}")' -f $t) }
  }

  # ---- case 2: a valid colour becomes the default highlight colour --------
  $box = OpenColorBox $proc.Id $wnd 149
  TypeHex $box '20c0a0'
  [void][CL]::PostMessageW($box, 0x0100, [IntPtr]13, [IntPtr]::Zero)      # Enter
  if (AwaitPref 'pal0=20c0a0' -and (AwaitPref 'hlcolor=6')) {
    Pass 'a typed colour lands in the palette and becomes the default'
  } else {
    Fail ('a typed colour lands in the palette and becomes the default ({0})' -f ((PrefLines) -join ' '))
  }

  # ---- case 3: the mark on the page really is that colour ----------------
  $before = BandSignature $wnd 0.05 0.48
  HighlightBlock $wnd 0.15 0.05 0.80 0.48
  $after = BandSignature $wnd 0.05 0.48
  $sc = Get-ChildItem (Join-Path $env:APPDATA 'mnpdf\doc-*.txt') -ErrorAction SilentlyContinue |
        Select-Object -First 1
  $sideColor = if ($sc) { ([regex]::Match((Get-Content $sc.FullName -Raw), 'hl=\d+,\d+,\d+,(\d+)')).Groups[1].Value } else { '' }
  if ($after -gt 25 -and $before -lt 10) {
    Pass 'a highlight painted in that colour reads as that colour on the page'
  } else {
    Fail ('a highlight painted in that colour reads as that colour on the page (signature {0} -> {1})' -f $before, $after)
  }
  if ($sideColor -eq '6') { Pass 'the sidecar records the custom slot' }
  else { Fail ('the sidecar records the custom slot (got "{0}")' -f $sideColor) }
  # The preset default has to move the same measure the other way, so the
  # result above cannot be an artefact of the measurement.
  [void][CL]::PostMessageW($wnd, 0x0111, [IntPtr]140, [IntPtr]::Zero)     # default highlight = yellow preset
  Start-Sleep -Milliseconds 500
  $yellowBefore = BandSignature $wnd 0.52 0.95
  HighlightBlock $wnd 0.15 0.52 0.80 0.95
  $yellow = BandSignature $wnd 0.52 0.95
  if ($yellow -lt 5 -and $yellow -le $yellowBefore) { Pass 'a preset default still paints the preset colour' }
  else { Fail ('a preset default still paints the preset colour (signature {0} -> {1})' -f $yellowBefore, $yellow) }

  # ---- case 4: an invalid entry explains itself, and keeps the box -------
  $box = OpenColorBox $proc.Id $wnd 149
  TypeHex $box '12345'                       # hex, but five digits: the box takes it, the parse refuses it
  [void][CL]::PostMessageW($box, 0x0100, [IntPtr]13, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 500
  $dlg = FindDlgCol $proc.Id
  $body = DlgBody $dlg
  if ($dlg -ne [IntPtr]::Zero -and $body -match 'is not a colour' -and $body -match '12345' -and
      $body -match 'ff4d00' -and [CL]::IsWindowVisible($box)) {
    Pass 'an invalid colour is explained at once and leaves the box open'
  } else {
    Fail ('an invalid colour is explained at once and leaves the box open (dialog {0}, box open {1}, body {2})' -f
          ($dlg -ne [IntPtr]::Zero), [CL]::IsWindowVisible($box), ($body -replace "`r?`n", ' | '))
  }
  if ($dlg -ne [IntPtr]::Zero) { DismissDlg $proc.Id 2 | Out-Null }        # OK

  # ---- case 5: typed then clicked away keeps the colour ------------------
  $box = OpenColorBox $proc.Id $wnd 149
  TypeHex $box 'abcdef'
  [void][CL]::PostMessageW($box, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_KILLFOCUS: the reader clicked away
  if (AwaitPref 'pal[0-9]+=abcdef') { Pass 'a colour typed then clicked away is kept' }
  else { Fail ('a colour typed then clicked away is kept ({0})' -f ((PrefLines) -join ' ')) }

  # ---- case 6: the slots can be cleared, with a confirmation -------------
  # With only three slots, a reader who wants a different set has to be able to
  # start from empty, and it asks first and says what happens to the marks.
  [void][CL]::PostMessageW($wnd, 0x0111, [IntPtr]180, [IntPtr]::Zero)
  DismissDlg $proc.Id 7 | Out-Null                                          # No, first
  $pf = (PrefLines) -join ' '
  if ($pf -match 'pal[0-9]+=') {
    Pass 'clearing asks first, and No keeps the colours'
  } else { Fail ('clearing asks first, and No keeps the colours ({0})' -f $pf) }
  [void][CL]::PostMessageW($wnd, 0x0111, [IntPtr]180, [IntPtr]::Zero)
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
  $box = OpenColorBox $proc.Id $wnd 169
  TypeHex $box 'ff8800'
  [void][CL]::PostMessageW($box, 0x0100, [IntPtr]13, [IntPtr]::Zero)
  if (AwaitPref 'pal0=ff8800' -and (AwaitPref 'pincolor=6')) {
    Pass 'the pin colour menu takes a custom colour too'
  } else {
    Fail ('the pin colour menu takes a custom colour too ({0})' -f ((PrefLines) -join ' '))
  }

  # ---- case 8: it survives a quit and relaunch ---------------------------
  [void][CL]::PostMessageW($wnd, 0x0111, [IntPtr]112, [IntPtr]::Zero)
  for ($i = 0; $i -lt 60; $i++) { if ($proc.HasExited) { break }; Start-Sleep -Milliseconds 250 }
  if (-not $proc.HasExited) { $proc | Stop-Process -Force }
  $script:proc2 = Start-Process -FilePath $exe -ArgumentList """$doc""" -PassThru
  $script:wnd2 = FindAppWindow $script:proc2.Id
  for ($i = 0; $i -lt 80; $i++) {
    $script:wnd2 = FindAppWindow $script:proc2.Id
    if ($script:wnd2 -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
  }
  Start-Sleep -Milliseconds 900
  if (AwaitPref 'pal0=ff8800' -and (AwaitPref 'pincolor=6')) {
    Pass 'the custom pin colour survives a quit and relaunch'
  } else {
    Fail ('the custom pin colour survives a quit and relaunch ({0})' -f ((PrefLines) -join ' '))
  }
  [void][CL]::PostMessageW($script:wnd2, 0x0111, [IntPtr]112, [IntPtr]::Zero)
  for ($i = 0; $i -lt 60; $i++) { if ($script:proc2.HasExited) { break }; Start-Sleep -Milliseconds 250 }
  if (-not $script:proc2.HasExited) { $script:proc2 | Stop-Process -Force }
} finally {
  foreach ($p in @($proc, $script:proc2)) {
    if ($p -and -not $p.HasExited) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
  }
  Remove-Item -LiteralPath $appPref -Force -ErrorAction SilentlyContinue
}
Write-Output ''
if ($failures.Count) { Write-Output ("RESULT: {0} FAILURE(S)" -f $failures.Count); exit 1 }
Write-Output 'RESULT: ALL PASS'
