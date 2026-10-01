# Shared support for every mnpdf suite. Dot-source this from a suite's root:
#     . "$PSScriptRoot\tests\lib.ps1"
#
# These rules live here exactly once, because they drifted apart badly when
# each suite carried its own copy:
#
#   1. How to find the app's real window, and how to look at it without stealing
#      focus from whatever the user is doing.
#   2. How to start the app on a document and wait for it to be ready (Await +
#      Launch), quoting the path so a spaced worktree still reaches argv[1].
#   3. How to forge and restore %APPDATA%\mnpdf\app.txt safely (several suites
#      forge it to make the update check deterministic).
#
# Dot-sourcing puts these in the caller's scope, so $script:pref* set here are
# the same variables the suites read.

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct WPL {
  public uint length; public uint flags; public uint showCmd;
  public int a1; public int a2; public int a3; public int a4;
  public int a5; public int a6; public int a7; public int a8;
}
public static class MN {
  // the title parameter must be IntPtr, not string: PowerShell marshals
  // IntPtr.Zero / $null into a string parameter as an actual empty string,
  // which makes the title filter match nothing and FindWindowExW always return 0
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, [MarshalAs(UnmanagedType.LPWStr)] string cl, IntPtr ti);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr h, ref WPL p);
  [DllImport("user32.dll")] public static extern bool SetWindowPlacement(IntPtr h, ref WPL p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@

# wait until a condition holds (polled) instead of sleeping a guessed length:
# a loaded machine makes every fixed sleep a coin flip
function Await([scriptblock]$Cond, [int]$TimeoutMs = 15000, [int]$StepMs = 100) {
  $elapsed = 0
  while ($elapsed -lt $TimeoutMs) {
    if (& $Cond) { return $true }
    Start-Sleep -Milliseconds $StepMs
    $elapsed += $StepMs
  }
  return $false
}

# The app's real window. Two traps ruled out here:
#   - a backgrounded window (MNPDF_BACKGROUND=1, which the gate sets) keeps its
#     MainWindowHandle 0 until its first ShowWindow, and WS_VISIBLE is masked
#     at creation so that steer is the app's own, not ours;
#   - that first ShowWindow runs only after openPath/openDialog, so a handle
#     bound at launch is either 0 or the wrong (transient) window.
# Class + owning pid is exact and immune to both.
function FindAppWindow([int]$ProcId) {
  $w = [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "mnpdf", [IntPtr]::Zero)
  while ($w -ne [IntPtr]::Zero) {
    $owner = 0
    [void][MN]::GetWindowThreadProcessId($w, [ref]$owner)
    if ($owner -eq $ProcId) { return $w }
    $w = [MN]::FindWindowExW([IntPtr]::Zero, $w, "mnpdf", [IntPtr]::Zero)
  }
  return [IntPtr]::Zero
}

# Restore a backgrounded window without activating it: a window the user already
# has open must never come to the front while a suite runs. SW_SHOWNOACTIVATE
# through SetWindowPlacement gives a real client rect (posted clicks at client
# coords, resize-refit and the titlebar client-rect assertions all need one)
# with the foreground untouched. Verified: showCmd=1, client 537x364,
# isFg=False.
function ShowNoActivate([IntPtr]$Wnd) {
  $wp = New-Object WPL
  $wp.length = 44
  [void][MN]::GetWindowPlacement($Wnd, [ref]$wp)
  $wp.showCmd = 4   # SW_SHOWNOACTIVATE
  [void][MN]::SetWindowPlacement($Wnd, [ref]$wp)
}

# Start the app and hand back its running, non-activated process. The wait for
# the window to be shown has to come first: the app shows it only after the
# document loads, and restoring earlier races the app's own first ShowWindow
# (which would re-minimize it).
#
# The document path is passed embedded in quotes ("""$Doc""") because
# Start-Process -ArgumentList builds a raw command line: unquoted, a path with a
# space is split into several argv entries and the app only ever reads argv[1].
# openPath then fails, openDialog() parks the app in a modal GetOpenFileNameW
# that runs before the first ShowWindow, and under MNPDF_BACKGROUND=1 (WS_VISIBLE
# masked out of the create style) the main window is never visible - so the wait
# below dies with the misleading "app window never shown". Embedded quotes are
# harmless on a path without spaces, and the app's own CommandLineToArgvW strips
# them, so argv parsing stays untouched.
function Launch([string]$Exe, [string]$Doc) {
  if ([string]::IsNullOrEmpty($Doc)) { $proc = Start-Process -FilePath $Exe -PassThru }
  else { $proc = Start-Process -FilePath $Exe -ArgumentList """$Doc""" -PassThru }
  if (-not (Await { (FindAppWindow $proc.Id) -ne [IntPtr]::Zero } 20000)) { throw "no mnpdf app window ($Exe $Doc)" }
  $w = FindAppWindow $proc.Id
  if (-not (Await { [MN]::IsWindowVisible($w) } 20000)) { throw "app window never shown" }
  # A minimized window still answers IsWindowVisible, so the wait above can pass
  # while the app is still about to run its own first ShowWindow - which for a
  # backgrounded launch minimizes it again. Measured in that window: window
  # 314x50 at -32000,-32000, client 0x0, iconic=True (Windows keeps the minimized
  # rect as the window size, so a suite that trusts a rect there measures a frame
  # no reader ever sees). Restore, then let the settle poll above confirm the
  # window is really back before the suite measures it.
  ShowNoActivate $w
  for ($settle = 0; $settle -lt 10 -and [MN]::IsIconic($w); $settle++) {
    Start-Sleep -Milliseconds 200
    ShowNoActivate $w
  }
  return $proc
}

# ---- the app.txt forge / restore rule ------------------------------------------------
#
# The suites that exercise the update check forge %APPDATA%\mnpdf\app.txt so the
# answer is deterministic and offline (a completed check remembered from minutes
# ago, or a stale clock that must be re-stamped). That file also holds the
# user's real titlebar / autosave / hlcolor / pincolor / palnext / updcheck /
# updtag preferences and the frame geometry winx/winy/winw/winh/winmax, so a run
# must put it back exactly as it found it - on every path, including a hard kill.

$script:prefSentinel = 'test-forged=1'

function AppPref-Forged {
  if (-not (Test-Path -LiteralPath $script:appPref)) { return $false }
  (Get-Content -LiteralPath $script:appPref -Raw -ErrorAction SilentlyContinue) -match ('(?m)^' + [regex]::Escape($script:prefSentinel) + '\b')
}

# leftover forge = the in-file sentinel, OR the durable marker. The in-file
# sentinel is not durable: the app rewrites app.txt from its own keys only
# (src/main.cpp writeAppPref), so a launch after an expired cooldown erases
# the sentinel while the forged defaults live on. A run killed in that window
# leaves a file no content check can tell from real prefs, so the forge also
# drops a marker file next to it (the app never touches that name):
# its presence means the real app.txt is currently a forge and must be discarded,
# never adopted as user prefs.
function AppPref-LeftoverForge {
  (AppPref-Forged) -or (Test-Path -LiteralPath $script:prefMarker)
}

function Restore-AppPref {
  # this run's forge is over: drop the durable marker FIRST so the file left
  # behind is never mistaken for a leftover forge by the next run
  Remove-Item -LiteralPath $script:prefMarker -ErrorAction SilentlyContinue
  # a leftover forgery is untrusted, not the user's state: never copy it back
  if (Test-Path -LiteralPath $script:prefMissing) { Remove-Item -LiteralPath $script:appPref -ErrorAction SilentlyContinue }
  elseif (Test-Path -LiteralPath $script:prefBackup) { Copy-Item -LiteralPath $script:prefBackup -Destination $script:appPref -Force -ErrorAction SilentlyContinue }
  # belt and braces: nothing may survive this call looking forged. The app's own
  # writeAppPref emits only known keys, so a sentinel here means the restore above
  # did not happen and the file is still pure forge.
  if (AppPref-Forged) { Remove-Item -LiteralPath $script:appPref -ErrorAction SilentlyContinue }
}

# Call this BEFORE writing anything: it remembers where app.txt is, backs the
# user's real one up, recognizes a leftover forge from an interrupted run, and
# arms the watchdog that puts it all back whatever happens to this shell.
# $Label keeps the per-suite temp files apart when two suites run side by side.
function Init-PrefForge([string]$AppPref, [string]$Label) {
  $script:appPref     = $AppPref
  $script:prefMarker  = Join-Path (Split-Path -Parent $AppPref) 'app.txt.testforge'
  $script:prefBackup  = Join-Path $env:TEMP ("mnpdf-{0}-pref-{1}.bak" -f $Label, $PID)
  $script:prefMissing = Join-Path $env:TEMP ("mnpdf-{0}-pref-{1}.missing" -f $Label, $PID)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $AppPref) | Out-Null
  if (AppPref-LeftoverForge) {
    Write-Output "note: app.txt holds leftover forged test values (an interrupted forge); the real prefs are already gone, so it is discarded rather than treated as user state"
    Set-Content -LiteralPath $script:prefMissing -Value ''
  } elseif (Test-Path -LiteralPath $AppPref) {
    Copy-Item -LiteralPath $AppPref -Destination $script:prefBackup -Force
  } else {
    Set-Content -LiteralPath $script:prefMissing -Value ''
  }
  # a watchdog child that outlives this shell by any means (timeout, tree kill,
  # hard crash) is the only thing that restores the prefs on those paths
  $wd = @"
while (Get-Process -Id $PID -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
Start-Sleep -Milliseconds 400
if (Test-Path -LiteralPath '$script:prefMissing') { Remove-Item -LiteralPath '$script:appPref' -Force -ErrorAction SilentlyContinue }
elseif (Test-Path -LiteralPath '$script:prefBackup') { Copy-Item -LiteralPath '$script:prefBackup' -Destination '$script:appPref' -Force -ErrorAction SilentlyContinue }
# a file still carrying the sentinel is leftover forge, not user state: never let it
# survive this watchdog as if it were real
if ((Get-Content -LiteralPath '$script:appPref' -Raw -ErrorAction SilentlyContinue) -match '(?m)^$script:prefSentinel') { Remove-Item -LiteralPath '$script:appPref' -Force -ErrorAction SilentlyContinue }
Remove-Item -LiteralPath '$script:prefBackup' -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath '$script:prefMissing' -Force -ErrorAction SilentlyContinue
# the durable marker's job ends once the prefs are back; drop it so it cannot
# misidentify the restored file as a forge on the next run (a tree-killed watchdog
# that never ran leaves it in place, which is exactly the leftover-forge signal)
if (Test-Path -LiteralPath '$script:prefMarker') { Remove-Item -LiteralPath '$script:prefMarker' -Force -ErrorAction SilentlyContinue }
"@
  [void](Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile','-NoLogo','-WindowStyle','Hidden','-EncodedCommand',([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($wd))) -WindowStyle Hidden)
}
