# Release-flow check. Two release mistakes this catches:
#   1. a binary whose self-reported version disagrees with the shipped README
#      (v2.0.2 shipped old code under the current version and told users they
#      were up to date - the version claim must match the release contract)
#   2. an update notice that stops explaining the portable-ZIP flow
# Everything is observed through the app's own UI (the update dialog), driven
# by posted messages. README.txt is the exact file the release ZIP ships, so it
# is the version contract: bump one without the other and this fails.
# the MessageBox body is NOT in any child window's text (WM_GETTEXTLEN on the
# static is 0 on this shell, and cross-process GetWindowTextW is blind on
# controls) - UI Automation is the reader that actually sees the text
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
. "$PSScriptRoot\tests\lib.ps1"   # one definition of the app.txt forge/restore rule
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public delegate bool EnumWindowsProcRel(IntPtr h, IntPtr l);
public static class R {
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr cap, [Out] char[] buf);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProcRel cb, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static string Text(IntPtr h) {
    if (h == IntPtr.Zero) return "";
    var b = new char[4096];
    SendText(h, 0x000D, (IntPtr)4095, b);   // WM_GETTEXT: GetWindowTextW is blind across processes
    return new string(b).TrimEnd('\0');
  }
}
"@

$failures = New-Object System.Collections.Generic.List[string]


# top-level window of this process whose title matches (dialog titles only -
# cross-process GetWindowTextW does work on top-level windows)
function FindDialog([int]$ProcId, [string]$TitlePat) {
  $script:fdPid = $ProcId
  $script:fdPat = $TitlePat
  $script:hit = [IntPtr]::Zero
  $cb = [EnumWindowsProcRel]{ param($w, $n)
    $owner = 0
    [void][R]::GetWindowThreadProcessId($w, [ref]$owner)
    if ($owner -eq $script:fdPid -and [R]::Text($w) -like ('*' + $script:fdPat + '*')) { $script:hit = $w }
    $true }
  [void][R]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:hit
}

function DialogBody([IntPtr]$Dlg) {
  $root = [System.Windows.Automation.AutomationElement]::FromHandle($Dlg)
  $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                       [System.Windows.Automation.Condition]::TrueCondition)
  $parts = New-Object System.Collections.Generic.List[string]
  foreach ($e in $all) {
    $n = $e.Current.Name
    if ($n) { [void]$parts.Add($n) }
  }
  return ($parts -join "`n")
}

# close ONLY the process this test launched: a graceful WM_CLOSE so the app's
# debounced sidecar flush and its final app-pref write run, force-killing only
# as a last resort and only ever a child we started - never an instance the
# captain may have open (which would lose that window's graceful-exit work)
function Stop-OwnedApp($p) {
  if (-not $p) { return }
  try { $p.Refresh() } catch { return }
  if ($p.HasExited) { return }
  $h = $p.MainWindowHandle
  if ($h -ne [IntPtr]::Zero) { [void][R]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE: quit cleanly
  if (-not (Await { try { $p.Refresh(); $p.HasExited } catch { $true } } 5000)) {
    try { $p.Kill() } catch {}   # last resort, and only ever this test's own process
  }
}

$repo = $PSScriptRoot
$readmePath = Join-Path $repo 'README.txt'
$exe = Join-Path $repo 'build\mnpdf.exe'
$doc = Join-Path $repo 'build\release-test.pdf'
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'

# requires a fresh instance we own: the dialog assertions assume a clean launch
# (the invariant the other suites follow - never disturb a running instance)
$existing = Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }
if ($existing) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }

# this test FORGES update state in the user's real app.txt. Save it once, before
# anything is written, and put it back on EVERY exit path: the try/finally around
# the body covers exceptions, early returns and Ctrl-C, and a watchdog child
# process that outlives this shell guarantees it even against a timeout kill or a
# Stop-Process - so a crashed run can never leave updtag=v9.9.9 behind and make
# the next manual check announce a version that does not exist.
#
# Two limits that follow from the forge: the watchdog dies with the shell's process
# tree, so a tree kill can still leave the forged file on disk. The forgery
# therefore carries a sentinel; whenever one is found the file is known leftover
# test state, never the captain's prefs, so it is not adopted as the backup and
# is stripped instead - a forged value can never be promoted into "the user's
# real prefs".
#
# One hole the sentinel alone cannot cover: the app rewrites app.txt from its own
# keys only (src/main.cpp writeAppPref), so once the start-up update check, a
# prefs command, or the WM_CLOSE write it makes as it quits runs, the in-file
# sentinel is erased while the forged defaults survive. A run killed in that
# window leaves a file that no content check can tell from real prefs. The
# forge therefore ALSO drops a durable marker file next to app.txt (the app
# never touches it): its presence means the real app.txt is currently a forge, so
# it is discarded rather than adopted, on every run start and every restore path.
# (tests/lib.ps1 owns all of that machinery: sentinel, marker, backup, watchdog.)
Init-PrefForge $appPref 'release'
function Run-Case([string]$Name, [string]$Tag, [string]$WantTitle, [string[]]$WantSnips, [int]$DismissId) {
  # forge a completed check from 5 minutes ago that remembered $Tag, so the
  # click is answered from cache: no network, deterministic dialog
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()   # the app's clock, not Get-Date %s
  Set-Content -LiteralPath $appPref -Value ("titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`nupdcheck={0}`nupdtag={1}`n{2}`n" -f ($now - 300), $Tag, $script:prefSentinel)
  Set-Content -LiteralPath $script:prefMarker -Value ''   # durable: the app's own rewrites erase the in-file sentinel, this file it cannot touch
  # the app writes doc-<fnv1a(path)>.txt and last.txt for autosave on every quit, and
  # autosave is forged on below: run on a disposable copy in build\ so the pristine
  # fixture in tests\ - a tracked file every suite reads - is never the one that gets
  # written to. Copied before the launch, the way every other suite does it.
  Copy-Item -LiteralPath (Join-Path $repo 'tests\arc.pdf') -Destination $doc -Force
  $p = $null
  try {
    if ((Get-Content -LiteralPath $appPref -Raw -ErrorAction SilentlyContinue) -notlike ('*updtag=' + $Tag + '*')) {
      $failures.Add($Name); Write-Output ("FAIL {0}: could not forge {1}; the app would spend a real network check" -f $Name, $appPref); return
    }
    # quoted for the same reason as Launch in tests\lib.ps1: one argv entry, or the
    # app parks in the open dialog and the update-check click never lands
    $p = Start-Process -FilePath $exe -ArgumentList """$doc""" -PassThru
    if (-not (Await { (FindAppWindow $p.Id) -ne [IntPtr]::Zero } 20000)) {
      $failures.Add($Name); Write-Output ("FAIL {0}: app never showed a main window" -f $Name); return
    }
    [void][R]::PostMessageW((FindAppWindow $p.Id), 0x0111, [IntPtr]170, [IntPtr]::Zero)   # Check for updates
    if (-not (Await { (FindDialog $p.Id $WantTitle) -ne [IntPtr]::Zero } 15000)) {
      $failures.Add($Name); Write-Output ("FAIL {0}: no '{1}' dialog after the click" -f $Name, $WantTitle); return
    }
    $body = DialogBody (FindDialog $p.Id $WantTitle)
    $missing = @($WantSnips | Where-Object { $body -notmatch [regex]::Escape($_) })
    if ($missing.Count) {
      $failures.Add($Name)
      Write-Output ("FAIL {0}: dialog body is missing: {1}" -f $Name, ($missing -join ' | '))
      Write-Output ("  body was: " + ($body -replace "`n", ' / '))
    } else {
      Write-Output ("PASS {0}" -f $Name)
    }
    # dismiss with the intended button (No on the yes/no prompt, so no browser opens)
    $dlg = FindDialog $p.Id $WantTitle
    $btn = [R]::GetDlgItem($dlg, $DismissId)
    if ($btn -ne [IntPtr]::Zero) { [void][R]::SendMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }   # BM_CLICK
    [void](Await { (FindDialog $p.Id $WantTitle) -eq [IntPtr]::Zero } 8000)
  } finally {
    Stop-OwnedApp $p          # close only this test's own process, gracefully
    Restore-AppPref           # leave the user's update state exactly as we found it, on every path
  }
}

$exeDir = Split-Path $exe -Parent
try {
  if (-not (Test-Path $readmePath)) { Write-Output 'FAIL: README.txt missing (the shipped version contract)'; exit 1 }
  if ((Get-Content $readmePath -Raw) -notmatch 'mnpdf v(\d+\.\d+\.\d+)') { Write-Output 'FAIL: README.txt carries no mnpdf vX.Y.Z line'; exit 1 }
  $expected = $Matches[1]
  if (-not (Test-Path $exe)) { Write-Output 'FAIL: build\mnpdf.exe missing - run build.bat first'; exit 1 }
  Write-Output ("version contract (README.txt, the file the release ZIP ships): {0}" -f $expected)

  Run-Case 'update notice explains the portable-ZIP flow' 'v9.9.9' 'mnpdf update available' @(
    ("mnpdf {0} -> v9.9.9 is available." -f $expected)   # binary's version claim == release contract
    'This is a portable ZIP update'
    'does not replace this copy'
    $exeDir                                             # names the exact folder to extract over
    '(Last checked'                                     # a cached answer says when it last checked
  ) 7                                                   # IDNO: never open the browser from a test

  Run-Case 'up-to-date answer names the same version' ("v{0}" -f $expected) 'mnpdf updates' @(
    ("mnpdf {0} is up to date." -f $expected)
    '(Last checked'
  ) 1                                                   # IDOK
} finally {
  Restore-AppPref          # leave the user's update state exactly as we found it, on every path
  Remove-Item -LiteralPath $script:prefBackup,$script:prefMissing -Force -ErrorAction SilentlyContinue
}
Write-Output ''
if ($failures.Count) { Write-Output ("RESULT: {0} FAILURE(S)" -f $failures.Count); exit 1 }
Write-Output 'RESULT: ALL PASS'
