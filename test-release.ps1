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
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProcRel cb, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
}
"@

# WM_GETTEXT, not GetWindowTextW: the title call is blind across processes on a
# control and answers with the creation text; the message reads what is really there
function ControlText([IntPtr]$h) {
  if ($h -eq [IntPtr]::Zero) { return '' }
  $b = New-Object char[] 4096
  [void][MN]::SendText($h, 0x000D, [IntPtr]4095, $b)
  return (-join $b).TrimEnd([char]0)
}

# top-level window of this process whose title matches (dialog titles only -
# cross-process GetWindowTextW does work on top-level windows)
function FindDialog([int]$ProcId, [string]$TitlePat) {
  $script:fdPid = $ProcId
  $script:fdPat = $TitlePat
  $script:hit = [IntPtr]::Zero
  $cb = [EnumWindowsProcRel]{ param($w, $n)
    $owner = 0
    [void][MN]::GetWindowThreadProcessId($w, [ref]$owner)
    if ($owner -eq $script:fdPid -and (ControlText $w) -like ('*' + $script:fdPat + '*')) { $script:hit = $w }
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
# user may have open (which would lose that window's graceful-exit work)
function Stop-OwnedApp($p) {
  if (-not $p) { return }
  try { $p.Refresh() } catch { return }
  if ($p.HasExited) { return }
  $h = $p.MainWindowHandle
  if ($h -ne [IntPtr]::Zero) { [void][MN]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE: quit cleanly
  if (-not (Await { try { $p.Refresh(); $p.HasExited } catch { $true } } 5000)) {
    try { $p.Kill() } catch {}   # last resort, and only ever this test's own process
  }
}

$repo = $PSScriptRoot
$readmePath = Join-Path $repo 'README.txt'
$exe = Resolve-AppExe
$doc = Join-Path $repo 'build\release-test.pdf'
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'

# requires a fresh instance we own: the dialog assertions assume a clean launch
# (the invariant the other suites follow - never disturb a running instance)
Assert-NoRunningApp

# this test FORGES update state in the user's real app.txt. Save it once, before
# anything is written, and put it back on EVERY exit path: the try/finally around
# the body covers exceptions, early returns and Ctrl-C, and a watchdog child
# process (tests\watchdog.ps1, spawned by Init-PrefForge) that outlives this
# shell guarantees it even against a timeout kill or a Stop-Process - so a
# crashed run can never leave updtag=v9.9.9 behind and make the next manual
# check announce a version that does not exist.
#
# The forgery carries a sentinel; whenever one is found the file is known
# leftover test state, never the user's prefs, so it is not adopted as the
# backup and is stripped instead. One hole the sentinel alone cannot cover: the
# app rewrites app.txt from its own keys only (src/main.cpp writeAppPref), so
# once the start-up update check, a prefs command, or the WM_CLOSE write it
# makes as it quits runs, the in-file sentinel is erased while the forged
# defaults survive. The forge therefore ALSO drops a durable marker file next to
# app.txt (the app never touches it): its presence means the real app.txt is
# currently a forge, so it is discarded rather than adopted, on every run start
# and every restore path. (tests/lib.ps1 owns all of that machinery.)
Init-PrefForge $appPref 'release'
function Run-Case([string]$Name, [string]$Tag, [string]$WantTitle, [string[]]$WantSnips, [int]$DismissId) {
  # forge a completed check from 5 minutes ago that remembered $Tag, so the
  # click is answered from cache: no network, deterministic dialog. The tag is
  # passed explicitly here; the shipped-README default lives in tests/lib.ps1.
  $now = Set-ForgedAppPref 300 $Tag
  # the app writes doc-<fnv1a(path)>.txt and last.txt for autosave on every quit, and
  # autosave is forged on below: run on a disposable copy in build\ so the pristine
  # fixture in tests\ - a tracked file every suite reads - is never the one that gets
  # written to. Copied before the launch, the way every other suite does it.
  Copy-Item -LiteralPath (Join-Path $repo 'tests\arc.pdf') -Destination $doc -Force
  $p = $null
  try {
    if ((Get-Content -LiteralPath $appPref -Raw -ErrorAction SilentlyContinue) -notlike ('*updtag=' + $Tag + '*')) {
      Fail $Name ("could not forge {0}; the app would spend a real network check" -f $appPref)
      return
    }
    # Start-App carries the same quoting rule as Launch in tests\lib.ps1: one argv
    # entry, or the app parks in the open dialog and the update-check click never lands
    try { $p = Start-App $exe $doc }
    catch {
      Fail $Name ("app never showed a main window ({0})" -f $_.Exception.Message)
      return
    }
    [void][MN]::PostMessageW((FindAppWindow $p.Id), 0x0111, [IntPtr]$CMD_CHECK_UPDATES, [IntPtr]::Zero)   # Check for updates
    if (-not (Await { (FindDialog $p.Id $WantTitle) -ne [IntPtr]::Zero } 15000)) {
      Fail $Name ("no '{0}' dialog after the click" -f $WantTitle)
      return
    }
    $body = DialogBody (FindDialog $p.Id $WantTitle)
    $missing = @($WantSnips | Where-Object { $body -notmatch [regex]::Escape($_) })
    if ($missing.Count) {
      Fail $Name ("dialog body is missing: {0}" -f ($missing -join ' | '))
      Write-Output ("  body was: " + ($body -replace "`n", ' / '))
    } else {
      Pass $Name
    }
    # dismiss with the intended button (No on the yes/no prompt, so no browser opens)
    $dlg = FindDialog $p.Id $WantTitle
    $btn = [R]::GetDlgItem($dlg, $DismissId)
    if ($btn -ne [IntPtr]::Zero) { [void][MN]::SendMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }   # BM_CLICK
    [void](Await { (FindDialog $p.Id $WantTitle) -eq [IntPtr]::Zero } 8000)
  } finally {
    Stop-OwnedApp $p          # close only this test's own process, gracefully
    Restore-AppPref           # leave the user's update state exactly as we found it, on every path
  }
}

$exeDir = Split-Path $exe -Parent
try {
  if (-not (Test-Path $readmePath)) { Fail 'release file list' 'README.txt missing (the shipped version contract)'; exit 1 }
  if ((Get-Content $readmePath -Raw) -notmatch 'mnpdf v(\d+\.\d+\.\d+)') { Fail 'version contract' 'README.txt carries no mnpdf vX.Y.Z line'; exit 1 }
  $expected = $Matches[1]
  Write-Output ("version contract (README.txt, the file the release ZIP ships): {0}" -f $expected)

  # the four files a release ZIP is built from, at the sources the ZIP is
  # assembled from: the two build outputs and the two root documents. A version
  # claim is meaningless if a ZIP assembled right now would ship half a release.
  foreach ($f in @('mnpdf.exe', 'pdfium.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $exeDir $f))) { Fail 'release file list' "build\$f is missing - run build.bat first"; exit 1 }
  }
  foreach ($f in @('README.txt', 'PDFIUM-LICENSE.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $f))) { Fail 'release file list' "$f is missing from the repo root - the ZIP would ship without it"; exit 1 }
  }

  Run-Case 'update notice explains the portable-ZIP flow' 'v9.9.9' 'mnpdf update available' @(
    ("mnpdf {0} -> v9.9.9 is available." -f $expected)   # binary's version claim == release contract
    'This is a portable ZIP update'
    'does not replace this copy'
    (Split-Path $exe -Parent)                            # names the exact folder to extract over
    '(Last checked'                                      # a cached answer says when it last checked
  ) 7                                                    # IDNO: never open the browser from a test

  Run-Case 'up-to-date answer names the same version' ("v{0}" -f $expected) 'mnpdf updates' @(
    ("mnpdf {0} is up to date." -f $expected)
    '(Last checked'
  ) 1                                                    # IDOK
} finally {
  Restore-AppPref          # leave the user's update state exactly as we found it, on every path
  Remove-Item -LiteralPath $script:prefBackup,$script:prefMissing -Force -ErrorAction SilentlyContinue
}
Write-Output ''
Complete-Suite
