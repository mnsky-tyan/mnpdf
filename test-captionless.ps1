# The caption-less window. Reported together: a launch with the titlebar
# preference hidden still carved a caption out of the client until a later frame
# recalc happened to fix it, and the frame of a caption-less window could not be
# dragged at all - with the client covering the whole window DefWindowProc
# reports HTCLIENT for every pixel, so the only way to move or resize the window
# was the system menu.
#
# This suite measures the real window, because every claim here is a claim
# about geometry rather than about a preference line:
#   - hiding the titlebar must make the client exactly as tall as the window
#     (the caption's 58 px at this dpi is gone, not merely painted away);
#   - the preference must persist, and a plain relaunch with it hidden must
#     come up caption-less from the very first frame - the same bug the fix in
#     4b8bbb6 addressed, on the path a toggle alone never reaches;
#   - the caption-less window must still be workable: the top strip moves it
#     (the caption is gone, so it is the only handle), and the other three
#     edges plus the corners resize it - the WM_NCCALCSIZE branch gives the
#     client the whole window, so WM_NCHITTEST computes these zones in the
#     window proc instead.
#
# Unlike the colour suite this one runs backgrounded: everything it asserts is
# a posted message plus a rect read, so the captain's screen is untouched.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\tests\lib.ps1"

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CAP {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetSystemMetricsForDpi(int i, uint dpi);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

$failures = New-Object System.Collections.Generic.List[string]
$doc = Join-Path $PSScriptRoot 'tests\arc.pdf'
$exe = $env:MNPDF_GATE_EXE
if (-not $exe) { $exe = Join-Path $PSScriptRoot 'build\mnpdf.exe' }
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'
$CMD_TOGGLE = 113    # Hide titlebar / Show titlebar
$CMD_QUIT   = 112    # Quit

function Pass([string]$Name) { Write-Output ("PASS {0}" -f $Name) }
function Fail([string]$Name, [string]$Detail) {
  $failures.Add($Name); Write-Output ("FAIL {0} - {1}" -f $Name, $Detail)
}

function Title([IntPtr]$Wnd) { $sb = New-Object System.Text.StringBuilder 256; [void][CAP]::GetWindowTextW($Wnd, $sb, 256); $sb.ToString() }

# how much of the window the non-client area still eats, in pixels
function CaptionGap([IntPtr]$Wnd) {
  $c = New-Object CAP+RECT; $w = New-Object CAP+RECT
  [void][CAP]::GetClientRect($Wnd, [ref]$c)
  [void][CAP]::GetWindowRect($Wnd, [ref]$w)
  return (($w.B - $w.T) - ($c.B - $c.T))
}
function ClientSize([IntPtr]$Wnd) {
  $c = New-Object CAP+RECT
  [void][CAP]::GetClientRect($Wnd, [ref]$c)
  return ($c.R - $c.L), ($c.B - $c.T)
}
# WM_NCHITTEST takes a point in SCREEN coordinates. Read from the window rect,
# not from the client rect: the two share a top-left corner here only when the
# caption is hidden, which is the case that matters.
function HitAt([IntPtr]$Wnd, [int]$Dx, [int]$Dy) {
  $w = New-Object CAP+RECT
  [void][CAP]::GetWindowRect($Wnd, [ref]$w)
  $l = New-Object IntPtr ((($w.T + $Dy) -shl 16) -bor (($w.L + $Dx) -band 0xFFFF))
  return [CAP]::SendMessageW($Wnd, 0x0084, [IntPtr]::Zero, $l).ToInt32()
}
function Toggle([IntPtr]$Wnd) {
  [void][CAP]::PostMessageW($Wnd, 0x0111, [IntPtr]$CMD_TOGGLE, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 500
}
function QuitAndWait($Proc) {
  [void][CAP]::PostMessageW((FindAppWindow $Proc.Id), 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero)
  if (-not (Await { $Proc.HasExited } 20000)) { $Proc | Stop-Process -Force | Out-Null; Start-Sleep -Milliseconds 500 }
}

# ---- a deterministic start, with the captain's real prefs protected ----------
Init-PrefForge $appPref 'captionless'
try {
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $appPref) | Out-Null
  Set-Content -LiteralPath $appPref ("titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`n" + $script:prefSentinel + "`n")

  $p = Launch $exe $doc
  $h = FindAppWindow $p.Id
  [void](Await { (Title $h) -match 'arc\.pdf' } 20000)

  # ---- 1. a captioned launch carves the caption out ------------------------
  $gapCap = CaptionGap $h
  if ($gapCap -gt 0) { Pass ("captioned launch leaves {0} px of non-client at the top" -f $gapCap) }
  else { Fail 'captioned launch' "no caption to hide (gap=0)" }
  $htCapTop  = HitAt $h 30 4      # the frame sliver above the caption
  $htCapDrag = HitAt $h 30 10     # inside the caption, below the frame sliver
  if ($htCapDrag -eq 2) { Pass 'a captioned window drags from its caption' }
  else { Fail 'caption drag' "expected HTCAPTION(2), got $htCapDrag" }
  if ($htCapTop -eq 12) { Pass 'a captioned window resizes from the top frame sliver' }
  else { Fail 'caption top frame' "expected HTTOP(12), got $htCapTop" }

  # ---- 2. hiding the titlebar takes the caption away -----------------------
  Toggle $h
  $gapNone = CaptionGap $h
  if ($gapNone -eq 0) { Pass 'hiding the titlebar makes the client the whole window' }
  else { Fail 'captionless client' "gap should be 0, got $gapNone" }
  $cw, $ch = ClientSize $h
  if ($ch -gt 0) { Pass ("caption-less client is a real {0}x{1} rect" -f $cw, $ch) }
  else { Fail 'captionless client size' "got {0}x{1}" -f $cw, $ch }
  $pref = (Get-Content -LiteralPath $appPref -Raw -ErrorAction SilentlyContinue)
  if ($pref -match '(?m)^titlebar=0') { Pass 'the hidden titlebar is persisted to app.txt' }
  else { Fail 'titlebar pref' 'app.txt does not say titlebar=0 after the toggle' }

  # ---- 3. the caption-less window stays workable ---------------------------
  # The strip is 8 design pixels, and this process runs DPI-virtualised on
  # a 192 dpi screen, so its physical thickness here is 8 px rather than 16 -
  # the two probe points below sit well inside and well outside either
  # reading, which is what makes the assertion a claim about the strip rather
  # than about the scaling mode.
  $ht = HitAt $h 30 2
  if ($ht -eq 2) { Pass 'the top strip still moves the window' }
  else { Fail 'top strip move' "expected HTCAPTION(2) near the top, got $ht" }

  $htLow = HitAt $h 30 60
  if ($htLow -eq 1) { Pass 'below the strip the window reports its client, not a caption' }
  else { Fail 'below strip' "expected HTCLIENT(1), got $htLow" }

  $midY = [int]($ch / 2)
  for ($side = 0; $side -lt 3; $side++) {
    $dx = @(1, ($cw - 2), [int]($cw / 2))[$side]
    $dy = @($midY, $midY, ($ch - 2))[$side]
    $want = @(10, 11, 15)[$side]           # HTLEFT / HTRIGHT / HTBOTTOM
    $name = @('left edge', 'right edge', 'bottom edge')[$side]
    $ht = HitAt $h $dx $dy
    if ($ht -eq $want) { Pass ("resizing from the {0}" -f $name) }
    else { Fail ("resize {0}" -f $name) ("expected {1}, got {2}" -f $want, $ht) }
  }
  $htCorner = HitAt $h 1 ($ch - 2)
  if ($htCorner -eq 16) { Pass 'resizing from the bottom-left corner' }
  else { Fail 'resize bottom-left corner' "expected HTBOTTOMLEFT(16), got $htCorner" }

  # ---- 4. and it stays hidden through a quit and a plain relaunch ----------
  QuitAndWait $p
  $p2 = Launch $exe $doc
  $h2 = FindAppWindow $p2.Id
  [void](Await { (Title $h2) -match 'arc\.pdf' } 20000)
  $gapRel = CaptionGap $h2
  if ($gapRel -eq 0) { Pass 'a relaunch with the titlebar hidden comes up caption-less' }
  else { Fail 'relaunch captionless' "the caption came back (gap=$gapRel)" }
  $htRel = HitAt $h2 30 2
  if ($htRel -eq 2) { Pass 'the relaunched caption-less window still moves from its top strip' }
  else { Fail 'relaunch top strip' "expected HTCAPTION(2), got $htRel" }

  # ---- 5. showing it again brings the caption back ------------------------
  Toggle $h2
  $gapBack = CaptionGap $h2
  if ($gapBack -gt 0) { Pass ("showing the titlebar returns {0} px of caption" -f $gapBack) }
  else { Fail 'titlebar restored' "the caption did not come back (gap=0)" }
  $p2 | Stop-Process -Force | Out-Null
}
finally {
  Restore-AppPref
  Get-Process -Name mnpdf -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path -like "$PSScriptRoot\build\*" } |
    Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Output ""
if ($failures.Count) { Write-Output "RESULT: $($failures.Count) FAILURE(S)"; exit 1 }
Write-Output "RESULT: ALL PASS"
