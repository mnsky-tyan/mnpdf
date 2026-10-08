# Shared support for every mnpdf suite. Dot-source this from a suite's root:
#     . "$PSScriptRoot\tests\lib.ps1"
#
# These rules live here exactly once, because they drifted apart badly when
# each suite carried its own copy:
#
#   1. How to find the app's real window, and how to look at it without stealing
#      focus from whatever the user is doing.
#   2. How to start the app on a document and wait for it to be ready
#      (Start-App + Launch), quoting the path so a spaced worktree still
#      reaches argv[1].
#   3. How to forge and restore %APPDATA%\mnpdf\app.txt safely (several suites
#      forge it to make the update check deterministic), including the
#      watchdog child that restores it even against a hard kill.
#   4. The app's interop surface (class MN), the menu-command ids, the sidecar
#      filename rule, and the PASS/FAIL bookkeeping every suite shares.
#
# Dot-sourcing puts these in the caller's scope, so $script:pref* and
# $script:failures set here are the same variables the suites read.
# $ErrorActionPreference becomes 'Stop' in the caller's scope too: a failed
# P/Invoke or a missing variable must be a hard stop, never a silent
# stale-value read.
$ErrorActionPreference = 'Stop'

# ---- the app's interop surface (one Add-Type per suite process) ----------
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
[StructLayout(LayoutKind.Sequential)]
public struct MNRect { public int L; public int T; public int R; public int B; }
public static class MN {
  // the title parameter must be IntPtr, not string: PowerShell marshals
  // IntPtr.Zero / $null into a string parameter as an actual empty string,
  // which makes the title filter match nothing and FindWindowExW always return 0
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, [MarshalAs(UnmanagedType.LPWStr)] string cl, IntPtr ti);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindowW([MarshalAs(UnmanagedType.LPWStr)] string cl, IntPtr ti);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr h, ref WPL p);
  [DllImport("user32.dll")] public static extern bool SetWindowPlacement(IntPtr h, ref WPL p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  // WM_GETTEXT into a char[]: cross-process GetWindowTextW is blind on
  // controls and answers with the creation text, so the suites that read a
  // control's real content send the message instead
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
  public static extern IntPtr SendText(IntPtr h, uint m, IntPtr cap, [Out] char[] buf);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int h2, bool r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out MNRect r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out MNRect r);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr hMenu, int nPos);
}
"@

# ---- menu-command ids the app's WM_COMMAND answers (src/main.cpp) --------
# Named once here so a suite never re-types a bare int with an ad-hoc comment;
# the ids are the menu protocol and stay exactly what main.cpp dispatches.
${CMD_MENU_FIND}        = 2     # Edit > Find (the search bar's own command id)
${CMD_COPY}             = 101
${CMD_FIND}             = 103
${CMD_FIT_WIDTH}        = 104
${CMD_ZOOM_IN}          = 105
${CMD_ZOOM_OUT}         = 106
${CMD_OPEN}             = 107
${CMD_AUTOSAVE}         = 109
${CMD_MINIMIZE}         = 110
${CMD_MAX_RESTORE}      = 111
${CMD_QUIT}             = 112
${CMD_TITLEBAR}         = 113
${CMD_UNDO}             = 114
${CMD_REDO}             = 115
${CMD_COPY_HL_TEXT}     = 116
${CMD_DELETE_HL}        = 117
${CMD_SAVE}             = 118
${CMD_SAVE_AS}          = 119
${CMD_ADD_PIN}          = 130
${CMD_EDIT_PIN}         = 131
${CMD_DELETE_PIN}       = 132
${CMD_ROTATE_CW}        = 133
${CMD_ROTATE_CCW}       = 134
${CMD_HIGHLIGHT}        = 135   # highlight the selection in the default colour
${CMD_PRINT}            = 137
${CMD_DEFAULT_HL_YELLOW}= 140   # first preset of the default-highlight submenu
${CMD_CUSTOM_HL_COLOR}  = 149   # Custom... on the default highlight colour
${CMD_CUSTOM_PIN_COLOR} = 169   # Custom... on the default pin colour
${CMD_CHECK_UPDATES}    = 170
${CMD_CLEAR_CUSTOM}     = 180   # clear the three custom palette slots
${CMD_NIGHT}            = 200   # invert the page for dark reading (toggles)
${CMD_OUTLINE}          = 201   # bookmarks side drawer (toggles)
${CMD_THUMBS}           = 202   # thumbnails side drawer (toggles)
${CMD_INSERT_SIG}       = 203   # pick a JPEG, then click the page
${CMD_CLEAR_SIGS}       = 204   # drop every stamp from the in-memory doc
${CMD_MERGE}            = 205   # add files to the end of this document
${CMD_SPLIT}            = 206   # export a page range into a new file
${CMD_REOPEN_LAST}      = 207   # open the document this one replaced
${CMD_NEW_TAB}          = 300   # New Tab: one more document in this window
${CMD_CLOSE_TAB}       = 301   # Close Tab: drop the active one

# the arc.pdf fixture's page count, read out of the title assertions it feeds
${FixturePages} = 13

# ---- PASS/FAIL bookkeeping (one convention for all nine suites) ---------
$script:failures = New-Object System.Collections.Generic.List[string]

function Pass([string]$Name) { Write-Output ("PASS {0}" -f $Name) }

function Fail([string]$Name, [string]$Detail) {
  $script:failures.Add($Name)
  if ($Detail) { Write-Output ("FAIL {0} - {1}" -f $Name, $Detail) }
  else { Write-Output ("FAIL {0}" -f $Name) }
}

# Every suite ends with exactly this. The runners grep the RESULT line (and
# ^FAIL), so its shape is a runner contract: FAIL/RESULT at column 0.
function Complete-Suite {
  if ($script:failures.Count) { Write-Output ("RESULT: {0} FAILURE(S)" -f $script:failures.Count); exit 1 }
  Write-Output "RESULT: ALL PASS"
}

# ---- waiting --------------------------------------------------------------
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

# internal: Restore a backgrounded window without activating it (SW_SHOWNOACTIVATE
# through SetWindowPlacement gives a real client rect with the foreground
# untouched). Launch composes it; suites go through Launch, not here.
# Verified: showCmd=1, client 537x364, isFg=False.
function ShowNoActivate([IntPtr]$Wnd) {
  $wp = New-Object WPL
  $wp.length = 44
  [void][MN]::GetWindowPlacement($Wnd, [ref]$wp)
  $wp.showCmd = 4   # SW_SHOWNOACTIVATE
  [void][MN]::SetWindowPlacement($Wnd, [ref]$wp)
}

# The app binary a suite drives: the runner's explicit choice, else the tree's
# own build output (lib.ps1 lives in tests\, so the repo root is one level up).
function Resolve-AppExe {
  if ($env:MNPDF_GATE_EXE) { return $env:MNPDF_GATE_EXE }
  return Join-Path $PSScriptRoot "..\build\mnpdf.exe"
}

$script:appWaitMs = 20000   # one bound for both launch waits

# ---- local-only seam: land the app window on a chosen virtual desktop ------
#
# Set MNPDF_WINDOW_DESKTOP (a 0-based desktop number) and MNPDF_VD_DLL (the
# VirtualDesktopAccessor.dll path) to have every app window moved to that
# desktop as part of the launch. This is how a local gate keeps test windows off
# the desktop someone is using: the move happens here, deterministically, before
# any restore or measurement - not by a watcher racing the suite. Both variables
# are unset on CI, where the block below never runs and behaviour is unchanged.
$script:vdReady = $false
function Move-AppWindowToDesktop([IntPtr]$Wnd) {
  if (-not $env:MNPDF_WINDOW_DESKTOP) { return }
  if (-not $script:vdReady) {
    $dll = $env:MNPDF_VD_DLL
    if (-not $dll -or -not (Test-Path $dll)) { return }
    try { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VDM {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr LoadLibraryW(string p);
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)]
  public static extern IntPtr GetProcAddress(IntPtr h, string n);
  public delegate int D1(IntPtr h);
  public delegate int D2(IntPtr h, int n);
  public static IntPtr H;
  // The delegates live behind real static METHODS: Windows PowerShell 5.1
  // cannot invoke a delegate-typed static FIELD with [VDM]::Name(...) - it
  // answers "does not contain a method named" and, under the suite's stop-on-
  // error preference, that killed suites silently. Same lesson as the watcher.
  public static D1 pOf;
  public static D2 pMove;
  public static bool Bind(string p) {
    H = LoadLibraryW(p);
    if (H == IntPtr.Zero) return false;
    pOf   = (D1)Marshal.GetDelegateForFunctionPointer(GetProcAddress(H, "GetWindowDesktopNumber"), typeof(D1));
    pMove = (D2)Marshal.GetDelegateForFunctionPointer(GetProcAddress(H, "MoveWindowToDesktopNumber"), typeof(D2));
    return pOf != null && pMove != null;
  }
  public static int DesktopOf(IntPtr h) { return pOf(h); }
  public static int MoveTo(IntPtr h, int n) { return pMove(h, n); }
}
'@ } catch { if (-not [VDM]::pOf) { return } }   # already loaded: reuse it
    if (-not [VDM]::Bind($dll)) { return }
    $script:vdReady = $true
  }
  # a window created moments ago has no desktop yet (-1): wait out the
  # assignment, then move and verify, retrying the move itself a few times
  for ($i = 0; $i -lt 40 -and [VDM]::DesktopOf($Wnd) -lt 0; $i++) { Start-Sleep -Milliseconds 50 }
  $target = 0
  if (-not [int]::TryParse($env:MNPDF_WINDOW_DESKTOP, [ref]$target)) { return }
  for ($i = 0; $i -lt 10 -and [VDM]::DesktopOf($Wnd) -ne $target; $i++) {
    [void][VDM]::MoveTo($Wnd, $target)
    Start-Sleep -Milliseconds 60
  }
}

# Start the app and hand back its running process with a shown, owned window,
# without touching activation or the restore state: the wait for the window to
# be shown has to come first, because the app shows it only after the document
# loads, and restoring earlier races the app's own first ShowWindow (which
# would re-minimize it).
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
function Start-App([string]$Exe, [string]$Doc) {
  if ($env:MNPDF_WINDOW_DESKTOP) {
    # Gate runs spawn the app through WMI on purpose. A process created by
    # WmiPrvSE inherits NO right to take foreground, so every activation it
    # attempts - MessageBoxes included - is denied by the system: the box still
    # displays and is readable, but it can never switch the active desktop.
    # Start-Process inherits the caller's foreground right, so a gate-run app
    # that lives on the 'second' desktop could then switch the active desktop to
    # itself on every MessageBox. The environment block is passed explicitly
    # because WMI children do not inherit the caller's env (APPDATA/TEMP
    # isolation).
    $cmd = '"' + $Exe + '"'
    if (-not [string]::IsNullOrEmpty($Doc)) { $cmd += ' "' + $Doc + '"' }
    $envPairs = @(foreach ($e in (Get-ChildItem Env:)) { '{0}={1}' -f $e.Name, $e.Value })
    # The WMI property is EnvironmentVariables (not Environment - that name
    # answers "not found" and killed suites before their first output line),
    # and the embedded startup object only marshals through the classic
    # ManagementClass.InvokeMethod: Invoke-CimMethod cannot bind it here at all.
    $startup = ([wmiclass]'Win32_ProcessStartup').CreateInstance()
    $startup['EnvironmentVariables'] = [string[]]$envPairs
    $pmc = [wmiclass]'Win32_Process'
    $in = $pmc.GetMethodParameters('Create')
    $in['CommandLine'] = $cmd
    $in['ProcessStartupInformation'] = $startup
    $r = $pmc.InvokeMethod('Create', $in, $null)
    if ($r['ReturnValue'] -ne 0) { throw "WMI launch failed ($Exe): $($r['ReturnValue'])" }
    $proc = $null
    for ($i = 0; $i -lt 20 -and -not $proc; $i++) {
      try { $proc = Get-Process -Id $r['ProcessId'] -ErrorAction Stop } catch { Start-Sleep -Milliseconds 50 }
    }
    if (-not $proc) { throw "app process vanished right after launch ($Exe)" }
  }
  elseif ([string]::IsNullOrEmpty($Doc)) { $proc = Start-Process -FilePath $Exe -PassThru }
  else { $proc = Start-Process -FilePath $Exe -ArgumentList """$Doc""" -PassThru }
  if (-not (Await { (FindAppWindow $proc.Id) -ne [IntPtr]::Zero } $script:appWaitMs)) { throw "no mnpdf app window ($Exe $Doc)" }
  $w = FindAppWindow $proc.Id
  Move-AppWindowToDesktop $w
  if (-not (Await { [MN]::IsWindowVisible($w) } $script:appWaitMs)) { throw "app window never shown" }
  return $proc
}

# Start-App plus the restore-and-settle rule a measuring suite needs. A
# minimized window still answers IsWindowVisible, so Start-App's wait can pass
# while the app is still about to run its own first ShowWindow - which for a
# backgrounded launch minimizes it again. Measured in that window: window
# 314x50 at -32000,-32000, client 0x0, iconic=True (Windows keeps the minimized
# rect as the window size, so a suite that trusts a rect there measures a frame
# no reader ever sees). Restore, then let the settle loop confirm the window is
# really back before the suite measures it.
function Launch([string]$Exe, [string]$Doc) {
  $proc = Start-App $Exe $Doc
  $w = FindAppWindow $proc.Id
  ShowNoActivate $w
  for ($settle = 0; $settle -lt 10 -and [MN]::IsIconic($w); $settle++) {
    Start-Sleep -Milliseconds 200
    ShowNoActivate $w
  }
  return $proc
}

# ---- the app.txt forge / restore rule --------------------------------------
#
# The suites that exercise the update check forge %APPDATA%\mnpdf\app.txt so the
# answer is deterministic and offline (a completed check remembered from minutes
# ago, or a stale clock that must be re-stamped). That file also holds the
# user's real titlebar / autosave / hlcolor / pincolor / palnext / updcheck /
# updtag preferences and the frame geometry winx/winy/winw/winh/winmax, so a run
# must put it back exactly as it found it - on every path, including a hard kill.

$script:prefSentinel = 'test-forged=1'

function Test-PrefForged {
  param([string]$AppPref, [string]$Sentinel)
  if (-not $AppPref) { $AppPref = $script:appPref }
  if (-not $Sentinel) { $Sentinel = $script:prefSentinel }
  if (-not $AppPref -or -not (Test-Path -LiteralPath $AppPref)) { return $false }
  # escaped and word-bounded: the rule exists here exactly once
  (Get-Content -LiteralPath $AppPref -Raw -ErrorAction SilentlyContinue) -match ('(?m)^' + [regex]::Escape($Sentinel) + '\b')
}

# leftover forge = the in-file sentinel, OR the durable marker. The in-file
# sentinel is not durable: the app rewrites app.txt from its own keys only
# (src/main.cpp writeAppPref), so a launch after an expired cooldown erases
# the sentinel while the forged defaults live on. A run killed in that window
# leaves a file no content check can tell from real prefs, so the forge also
# drops a marker file next to it (the app never touches that name):
# its presence means the real app.txt is currently a forge and must be discarded,
# never adopted as user prefs.
function Test-PrefLeftoverForge {
  param([string]$AppPref, [string]$Marker, [string]$Sentinel)
  if (-not $AppPref) { $AppPref = $script:appPref }
  if (-not $Marker) { $Marker = $script:prefMarker }
  (Test-PrefForged -AppPref $AppPref -Sentinel $Sentinel) -or (Test-Path -LiteralPath $Marker)
}

# THE one restore policy - the suites' Restore-AppPref and the watchdog child
# both land here, so they cannot drift apart again. It does NOT delete the
# backup/missing scratch files: test-release restores between cases and still
# needs its backup at that point; suites delete the scratch files after their
# FINAL restore, and the watchdog after its own.
function Restore-PrefState {
  param([string]$AppPref, [string]$Marker, [string]$Backup, [string]$Missing, [string]$Sentinel)
  # this run's forge is over: drop the durable marker FIRST so the file left
  # behind is never mistaken for a leftover forge by the next run
  Remove-Item -LiteralPath $Marker -ErrorAction SilentlyContinue
  # a leftover forgery is untrusted, not the user's state: never copy it back
  if (Test-Path -LiteralPath $Missing) { Remove-Item -LiteralPath $AppPref -ErrorAction SilentlyContinue }
  elseif (Test-Path -LiteralPath $Backup) { Copy-Item -LiteralPath $Backup -Destination $AppPref -Force -ErrorAction SilentlyContinue }
  # belt and braces: nothing may survive this call looking forged. The app's own
  # writeAppPref emits only known keys, so a sentinel here means the restore above
  # did not happen and the file is still pure forge.
  if (Test-PrefForged -AppPref $AppPref -Sentinel $Sentinel) { Remove-Item -LiteralPath $AppPref -ErrorAction SilentlyContinue }
}

# suite-facing restore: the same rule against the paths Init-PrefForge recorded
function Restore-AppPref {
  if (-not $script:appPref) { return }
  Restore-PrefState -AppPref $script:appPref -Marker $script:prefMarker `
                    -Backup $script:prefBackup -Missing $script:prefMissing `
                    -Sentinel $script:prefSentinel
}

# Call this BEFORE writing anything: it remembers where app.txt is, backs the
# user's real one up, recognizes a leftover forge from an interrupted run, and
# arms the watchdog (tests\watchdog.ps1) that puts it all back whatever happens
# to this shell. $Label keeps the per-suite temp files apart when two suites run
# side by side.
function Init-PrefForge([string]$AppPref, [string]$Label) {
  $script:appPref     = $AppPref
  $script:prefMarker  = Join-Path (Split-Path -Parent $AppPref) 'app.txt.testforge'
  $script:prefBackup  = Join-Path $env:TEMP ("mnpdf-{0}-pref-{1}.bak" -f $Label, $PID)
  $script:prefMissing = Join-Path $env:TEMP ("mnpdf-{0}-pref-{1}.missing" -f $Label, $PID)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $AppPref) | Out-Null
  if (Test-PrefLeftoverForge) {
    Write-Output "note: app.txt holds leftover forged test values (an interrupted forge); the real prefs are already gone, so it is discarded rather than treated as user state"
    Set-Content -LiteralPath $script:prefMissing -Value ''
  } elseif (Test-Path -LiteralPath $AppPref) {
    Copy-Item -LiteralPath $AppPref -Destination $script:prefBackup -Force
  } else {
    Set-Content -LiteralPath $script:prefMissing -Value ''
  }
  # a watchdog child that outlives this shell by any means (timeout, tree kill,
  # hard crash) is the only thing that restores the prefs on those paths. The
  # paths go as ARGUMENTS to a static script file, never interpolated into
  # generated code: an apostrophe in a profile path cannot break the restore
  # (pre-quoted for Start-Process's raw command line, which does not add quotes).
  $wdPath = Join-Path $PSScriptRoot 'watchdog.ps1'
  $libPath = Join-Path $PSScriptRoot 'lib.ps1'
  $q = { '"{0}"' -f $args[0] }
  [void](Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
    (& $q $wdPath),
    "$PID",                       # ParentPid
    (& $q $libPath),              # LibPath (for Restore-PrefState)
    (& $q $AppPref),
    (& $q $script:prefMarker),
    (& $q $script:prefBackup),
    (& $q $script:prefMissing),
    (& $q $script:prefSentinel)
  ))
}

# Forge a completed update check $SecondsAgo old that remembered $Tag, so the
# app's answer is deterministic and offline. $Tag defaults to the version
# README.txt ships - the same contract the release ZIP carries - so the suites
# cannot freeze a stale version of their own. Requires Init-PrefForge first
# (it decides where the prefs are and drops the durable marker).
function Set-ForgedAppPref([int]$SecondsAgo = 300, [string]$Tag) {
  if (-not $Tag) {
    $readme = Join-Path (Split-Path -Parent $PSScriptRoot) 'README.txt'
    if ((Get-Content -LiteralPath $readme -Raw) -notmatch 'mnpdf v(\d+\.\d+\.\d+)') {
      throw "README.txt carries no mnpdf vX.Y.Z line; cannot derive the forged update tag"
    }
    $Tag = 'v' + $Matches[1]
  }
  # DateTimeOffset, not Get-Date %s: PowerShell's -UFormat %s is offset by the
  # local UTC offset, and the app stamps [[DateTimeOffset]::UtcNow]
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  Set-Content -LiteralPath $script:appPref -Value ("titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`nupdcheck={0}`nupdtag={1}`n{2}`n" -f ($now - $SecondsAgo), $Tag, $script:prefSentinel)
  Set-Content -LiteralPath $script:prefMarker -Value ''   # durable: an expired-cooldown launch rewrites app.txt and erases the in-file sentinel
  return $now
}

# ---- the running-instance rule --------------------------------------------
# A suite that wipes %APPDATA%\mnpdf or forges prefs must never do it while an
# instance the user has open is running: deciding after the wipe would delete
# real state first. Skip (exit 0) is the honest outcome; the runners treat a
# SKIP as a failure, so this only ever fires when a human really has the app open.
function Assert-NoRunningApp {
  $existing = Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }
  if ($existing) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }
}

# ---- clipboard (zombie-safe) ----------------------------------------------
$script:clipOk = $true   # cleared the first time a read hangs; never touch it again

# True when the clipboard both accepts a write and returns it. A hanging read
# is only one way this box's clipboard refuses to work: measured 2026-08-10,
# a corpse owner plus a locked session makes OpenClipboard fail FAST instead
# of hanging, so a copy silently lands nowhere and GetClip answers '' - which
# read as a real test failure. The write-then-read round trip tells the two
# apart, and a suite that cannot own the clipboard skips its copy case.
function Test-ClipboardRoundTrip {
  $sentinel = 'mnpdf-clip-probe-{0}' -f [Guid]::NewGuid().ToString('n')
  $tmpW = [System.IO.Path]::GetTempFileName()
  try {
    $proc = Start-Process powershell -ArgumentList '-NoProfile','-Command',
            ("Set-Clipboard -Value '{0}' | Out-File -Encoding unicode '$tmpW'" -f $sentinel) -PassThru -WindowStyle Hidden
    if (-not $proc.WaitForExit(2000)) { try { $proc.Kill() } catch {}; return $false }
  } finally { Remove-Item $tmpW -ErrorAction SilentlyContinue }
  return ((GetClip) -eq $sentinel)
}

function GetClip {
  # on some boxes the clipboard is held by a corpse and a read hangs forever,
  # so every caller probes through here and the suite degrades to SKIPs after
  # the first hang instead of stalling
  if (-not $script:clipOk) { return '' }
  $tmp = [System.IO.Path]::GetTempFileName()
  $proc = Start-Process powershell -ArgumentList '-NoProfile','-Command',"Get-Clipboard -Raw | Out-File -Encoding unicode '$tmp'" -PassThru -WindowStyle Hidden
  if (-not $proc.WaitForExit(2000)) {
    try { $proc.Kill() } catch {}
    $script:clipOk = $false   # zombie lock: bail out for good
    return ''
  }
  return ((Get-Content $tmp -Raw -ErrorAction SilentlyContinue) -replace "\r?\n?$", '')
}

# ---- sidecar filename (the app's own hash, in one place) ------------------
# sidecar file for a pdf path: FNV-1a over UTF-16 bytes incl. the terminator,
# the same hash the app uses (sidecarPathFor, src/main.cpp); BigInteger keeps
# the 64-bit wraps exact. This is the ONE PowerShell copy of that rule.
function SidecarFor([string]$pdfPath) {
  $M = [System.Numerics.BigInteger]::Pow(2, 64)
  $hh = [System.Numerics.BigInteger]1469598103934665603
  foreach ($b in [Text.Encoding]::Unicode.GetBytes($pdfPath)) {
    $hh = (($hh -bxor [System.Numerics.BigInteger]$b) * [System.Numerics.BigInteger]1099511628211) % $M
  }
  $hh = ($hh * [System.Numerics.BigInteger]1099511628211) % $M   # trailing NUL,
  $hh = ($hh * [System.Numerics.BigInteger]1099511628211) % $M   # hashed as a full wchar_t
  # the app prints the hash with %016llx, i.e. always 16 hex digits, unsigned. On .NET
  # Framework BigInteger.ToString("x") emits a spurious extra leading 0 whenever bit 63 is
  # set (~half of all 64-bit hashes) and PadLeft never truncates, so strip that sign digit
  # and re-pad: TrimStart("0").PadLeft(16,"0") reproduces the app's exact 16-digit name.
  return Join-Path $env:APPDATA ("mnpdf\doc-" + $hh.ToString("x").TrimStart("0").PadLeft(16, "0") + ".txt")
}

# ---- the tab strip's height (the app's own rule, in one place) ------------
# The strip is chrome above the document: a press inside it belongs to a tab,
# never to the page. It scales with the window's dpi, so any point meant for the
# document must clear it - a fixed pixel count is wrong on any other display.
# tabStripH() is MulDiv(30, dpi, 96) with a floor of 26; ceiling the product is
# never below MulDiv's rounded result, so a caller that clamps to this value
# always lands on the first document row. This is the ONE PowerShell copy.
function TabStripPx([IntPtr]$Wnd) {
  $dpi = [MN]::GetDpiForWindow($Wnd)
  return [Math]::Max(26, [int][Math]::Ceiling($dpi * 30 / 96))
}
