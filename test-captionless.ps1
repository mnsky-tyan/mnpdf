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
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetSystemMetricsForDpi(int i, uint dpi);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

# the app is per-monitor aware (src/main.cpp wWinMain), so this process must be
# too: hit-test probes then land in the same pixels the app's own zones are
# measured in instead of relying on Windows virtualising them
[void][CAP]::SetProcessDpiAwarenessContext([IntPtr](-4))

# the title is this suite's only proof that a document is open: the app puts its
# page count there only in this mode (src/main.cpp updateTitle), and it does so
# from renderPage, i.e. once gDoc holds the fixture
$env:MNPDF_VERBOSE = "1"

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
# the resize sliver above the caption, in pixels: the same two window metrics
# the app uses for its frame band (src/main.cpp WM_NCHITTEST), taken at this
# window's own dpi, so the caption probe below it holds on any monitor scale
function FrameTop([IntPtr]$Wnd) {
  $d = [CAP]::GetDpiForWindow($Wnd)
  return [int][CAP]::GetSystemMetricsForDpi(33, $d) + [int][CAP]::GetSystemMetricsForDpi(92, $d)
}
# The frame this suite asserts on only exists while the window is really shown:
# a window still minimizing offers a client rect of 0x0 and a window rect that is
# Windows' parked position, neither of which the app ever shows a reader.
function SettledRect([IntPtr]$Wnd) {
  [void](Await { -not [CAP]::IsIconic($Wnd) -and (ClientSize $Wnd)[1] -gt 200 } 15000)
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
  # the update clock is stamped inside its cooldown, so no launch here spends a
  # GitHub request and an update result never rewrites app.txt under the reader
  Set-Content -LiteralPath $appPref ("titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`nupdcheck={0}`nupdtag=v2.1.3`n{1}`n" -f ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - 300), $script:prefSentinel)

  $p = Launch $exe $doc
  $h = FindAppWindow $p.Id
  SettledRect $h
  [void](Await { (Title $h) -match 'mnpdf \d+/\d+' } 20000)

  # ---- 1. a captioned launch carves the caption out ------------------------
  $gapCap = CaptionGap $h
  if ($gapCap -gt 0) { Pass ("captioned launch leaves {0} px of non-client at the top" -f $gapCap) }
  else { Fail 'captioned launch' "no caption to hide (gap=0)" }
  $cwCap = (ClientSize $h)[0]
  $htCapTop  = HitAt $h 30 4                        # the frame sliver above the caption
  $htCapDrag = HitAt $h ([int]($cwCap / 2)) ((FrameTop $h) + 4)   # inside the caption, but
  #                     horizontally centred: the system-menu icon on the left and the
  #                     caption buttons on the right answer with their own hit codes
  #                     (HTSYSMENU, HTMINBUTTON...), which is a fact about the window,
  #                     not about the caption being draggable
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
  # This process is per-monitor aware, so these probes are in the same pixels
  # the app measures them with. The strip is its 8 design pixels at this dpi,
  # and the two probe points sit well inside and well outside it on every
  # scale, which is what keeps the assertion a claim about the strip rather
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
  SettledRect $h2
  [void](Await { (Title $h2) -match 'mnpdf \d+/\d+' } 20000)
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
  # only ever the two instances this suite started: the gate guarantees no other
  # instance is running, and an instance the captain opened himself must never
  # be force-killed, because a force kill skips the app's graceful exit
  foreach ($proc in @($p, $p2)) {
    if ($proc -and -not $proc.HasExited) { $proc | Stop-Process -Force -ErrorAction SilentlyContinue }
  }
}

Write-Output ""
if ($failures.Count) { Write-Output "RESULT: $($failures.Count) FAILURE(S)"; exit 1 }
Write-Output "RESULT: ALL PASS"
