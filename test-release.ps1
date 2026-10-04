# Release-flow check. Three release mistakes this catches:
#   1. a binary whose self-reported version disagrees with the shipped README
#      (v2.0.2 shipped old code under the current version and told users they
#      were up to date - the version claim must match the release contract)
#   2. an update flow whose answer does not say what installing will do
#   3. an updater that cannot actually update the app: the full install is
#      driven here against a scratch copy, with the ZIP payload served locally
#      (MNPDF_UPDATE_ZIP) and the target folder pointed at the copy
#      (MNPDF_UPDATE_DIR), so the machine's real mnpdf is never touched
# Everything is observed through the app's own update dialog, driven by posted
# messages. README.txt is the exact file the release ZIP ships, so it is the
# version contract: bump one without the other and this fails.
# Control text is read with WM_GETTEXT: cross-process GetWindowTextW answers
# with the creation text, and both the static and the button change at runtime
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

# exact-title variant: the refusal box is titled 'mnpdf update', one 's' away
# from the dialog it is spawned from
function FindDialogExact([int]$ProcId, [string]$Title) {
  $script:fxPid = $ProcId
  $script:fxTitle = $Title
  $script:fxHit = [IntPtr]::Zero
  $cb = [EnumWindowsProcRel]{ param($w, $n)
    $owner = 0
    [void][MN]::GetWindowThreadProcessId($w, [ref]$owner)
    if ($owner -eq $script:fxPid) {
      $t = New-Object System.Text.StringBuilder 256
      [void][MN]::GetWindowTextW($w, $t, 256)
      if ($t.ToString() -eq $script:fxTitle) { $script:fxHit = $w; return $false }
    }
    $true }
  [void][R]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:fxHit
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
# One state, one dialog: the click is answered from a forged completed check
# (no network), the dialog is found by title, and its body and action button
# are read with WM_GETTEXT. $BtnOut hands the button text to the caller.
function Run-Case([string]$Name, [string]$Tag, [string[]]$WantSnips, [string]$WantButton) {
  # forge a completed check from 5 minutes ago that remembered $Tag, so the
  # click is answered from cache: no network, deterministic dialog. The tag is
  # passed explicitly here; the shipped-README default lives in tests/lib.ps1.
  $now = Set-ForgedAppPref 300 $Tag
  # the app writes doc-<fnv1a(path)>.txt and last.txt for autosave on every quit,
  # and autosave is forged on: run on a disposable copy in build\ so the pristine
  # fixture in tests\ is never the one that gets written to.
  Copy-Item -LiteralPath (Join-Path $repo 'tests\arc.pdf') -Destination $doc -Force
  $p = $null
  try {
    if ((Get-Content -LiteralPath $appPref -Raw -ErrorAction SilentlyContinue) -notlike ('*updtag=' + $Tag + '*')) {
      Fail $Name ("could not forge {0}; the app would spend a real network check" -f $appPref)
      return
    }
    # Start-App carries the same quoting rule as Launch in tests\lib.ps1: one argv
    # entry, or the app parks in the open dialog and the update click never lands
    try { $p = Start-App $exe $doc }
    catch {
      Fail $Name ("app never showed a main window ({0})" -f $_.Exception.Message)
      return
    }
    [void][MN]::PostMessageW((FindAppWindow $p.Id), 0x0111, [IntPtr]$CMD_CHECK_UPDATES, [IntPtr]::Zero)   # Check for updates
    if (-not (Await { (FindDialog $p.Id 'mnpdf updates') -ne [IntPtr]::Zero } 15000)) {
      Fail $Name 'no mnpdf updates dialog after the click'
      return
    }
    $dlg = FindDialog $p.Id 'mnpdf updates'
    $body = ControlText ([R]::GetDlgItem($dlg, 100))
    $btn = ControlText ([R]::GetDlgItem($dlg, 1))
    $missing = @($WantSnips | Where-Object { $body -notmatch [regex]::Escape($_) })
    if ($missing.Count) {
      Fail $Name ("dialog body is missing: {0}" -f ($missing -join ' | '))
      Write-Output ("  body was: " + ($body -replace [string][char]10, ' / '))
    } elseif ($btn -ne $WantButton) {
      Fail $Name ("the action button reads '{0}', not '{1}'" -f $btn, $WantButton)
    } else {
      Pass $Name
    }
    # dismiss with the dialog's own Close button: a test never opens the browser
    $closeBtn = [R]::GetDlgItem($dlg, 3)
    if ($closeBtn -ne [IntPtr]::Zero) { [void][MN]::SendMessageW($closeBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }   # BM_CLICK
    [void](Await { (FindDialog $p.Id 'mnpdf updates') -eq [IntPtr]::Zero } 8000)
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

  # a newer remembered tag turns the action button into the update button, and
  # the body says exactly what installing will do to this folder
  Run-Case 'an available update turns the check button into the update button' 'v9.9.9' @(
    ("mnpdf {0} -> v9.9.9 is available." -f $expected)   # binary's version claim == release contract
    'replaces mnpdf.exe and pdfium.dll in:'
    $exeDir                                              # names the exact folder it will update
    '(Last checked'                                      # a cached answer says when it last checked
    'are not touched'                                    # and says what it will not touch
  ) ('Update to v9.9.9')

  Run-Case 'an up-to-date answer leaves the button a check button' ("v{0}" -f $expected) @(
    ("mnpdf {0} is up to date." -f $expected)
    '(Last checked'
  ) 'Check for updates'

  # An unsaved document must not be replaced under the user: the pin makes the
  # document dirty, and the update click has to be refused with the reason.
  Copy-Item -LiteralPath (Join-Path $repo 'tests\arc.pdf') -Destination $doc -Force
  $now = Set-ForgedAppPref 300 'v9.9.9'
  $p = $null
  try {
    $p = Start-App $exe $doc
    $main = FindAppWindow $p.Id
    [void][MN]::PostMessageW($main, 0x0111, [IntPtr]$CMD_CHECK_UPDATES, [IntPtr]::Zero)
    if (-not (Await { (FindDialog $p.Id 'mnpdf updates') -ne [IntPtr]::Zero } 15000)) {
      Fail 'an unsaved document is not updated under the user' 'no update dialog'
    } else {
      $dlg = FindDialog $p.Id 'mnpdf updates' 
      [void](Await { (ControlText ([R]::GetDlgItem((FindDialog $p.Id 'mnpdf updates'), 1))) -eq 'Update to v9.9.9' } 8000)
      [void][MN]::PostMessageW($main, 0x0111, [IntPtr]$CMD_ROTATE_CW, [IntPtr]::Zero)    # dirty the document
      Start-Sleep -Milliseconds 300
      # The refusal is a modal box, so the click is POSTED: a sent BM_CLICK would
      # block until the box the click itself raised is dismissed.
      [void][MN]::PostMessageW([R]::GetDlgItem((FindDialog $p.Id 'mnpdf updates'), 1), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
      $warn = [IntPtr]::Zero
      if (-not (Await { (FindDialogExact $p.Id 'mnpdf update') -ne [IntPtr]::Zero } 8000)) {
        Fail 'an unsaved document is not updated under the user' 'no refusal for an unsaved document'
      } else {
        $warn = FindDialogExact $p.Id 'mnpdf update'
        $warnBody = ControlText ([R]::GetDlgItem($warn, 0xFFFF))
        if ($warnBody -match 'unsaved') { Pass 'an unsaved document is not updated under the user' }
        else {
          Fail 'an unsaved document is not updated under the user' ("the refusal says: '{0}'" -f $warnBody)
          Write-Output ("  warning text: " + ($warnBody -replace [string][char]10, ' / '))
        }
        # the refusal is MB_OK, so it has exactly one button
        [void][MN]::SendMessageW([R]::GetDlgItem($warn, 1), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)    # IDOK
      }
      if ((FindDialog $p.Id 'mnpdf updates') -eq [IntPtr]::Zero) {
        Fail 'an unsaved document is not updated under the user' 'the update dialog closed anyway'
      }
    }
  } finally {
    Stop-OwnedApp $p
    Restore-AppPref
  }

  # The install itself, end to end, against a scratch copy: the payload arrives
  # from a local ZIP (the MNPDF_UPDATE_ZIP seam), the target folder is the copy
  # (MNPDF_UPDATE_DIR), and the proof is the replaced exe, a restarted process
  # and no .old copies left behind.
  $root = Join-Path $env:TEMP ('mnpdf-selfupdate-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  foreach ($d in 'app', 'payload\stage', 'state\mnpdf', 'tmp') {
    New-Item -ItemType Directory -Force -Path (Join-Path $root $d) | Out-Null
  }
  Copy-Item -LiteralPath $exe -Destination (Join-Path $root 'app\mnpdf.exe')
  Copy-Item -LiteralPath (Join-Path $exeDir 'pdfium.dll') -Destination (Join-Path $root 'app\pdfium.dll')
  Copy-Item -LiteralPath (Join-Path $repo 'tests\arc.pdf') -Destination (Join-Path $root 'app\a.pdf')
  $beforeSize = (Get-Item (Join-Path $root 'app\mnpdf.exe')).Length
  $stage = Join-Path $root 'payload\stage'
  Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'mnpdf.exe')
  Copy-Item -LiteralPath (Join-Path $exeDir 'pdfium.dll') -Destination (Join-Path $stage 'pdfium.dll')
  $fs = [System.IO.File]::OpenWrite((Join-Path $stage 'mnpdf.exe')); $fs.Seek(0, 'End') | Out-Null
  $fs.Write((New-Object byte[] 1048576), 0, 1048576); $fs.Close()
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $root 'payload\update.zip') -Force
  $payloadSize = (Get-Item (Join-Path $stage 'mnpdf.exe')).Length
  $updcheck = [DateTimeOffset]::UtcNow.AddMinutes(-5).ToUnixTimeSeconds()
  Set-Content -LiteralPath (Join-Path $root 'state\mnpdf\app.txt') `
    -Value "titlebar=1`nautosave=1`nupdcheck=$updcheck`nupdtag=v9.9.9`nverbose=1" -Encoding ASCII
  $env:MNPDF_UPDATE_ZIP = Join-Path $root 'payload\update.zip'
  $env:MNPDF_UPDATE_DIR = Join-Path $root 'app'
  $realAppdata = $env:APPDATA; $realTemp = $env:TEMP
  $env:APPDATA = Join-Path $root 'state'
  $env:TEMP = Join-Path $root 'tmp'
  $p = $null
  try {
    $p = Start-App (Join-Path $root 'app\mnpdf.exe') (Join-Path $root 'app\a.pdf')
    [void][MN]::PostMessageW((FindAppWindow $p.Id), 0x0111, [IntPtr]$CMD_CHECK_UPDATES, [IntPtr]::Zero)
    if (-not (Await { (FindDialog $p.Id 'mnpdf updates') -ne [IntPtr]::Zero } 15000)) {
      Fail 'an update installs itself and restarts the reader' 'no update dialog in the scratch copy'
    } else {
      $dlg = FindDialog $p.Id 'mnpdf updates' 
      [void](Await { (ControlText ([R]::GetDlgItem($dlg, 1))) -eq 'Update to v9.9.9' } 8000)
      [void][MN]::SendMessageW([R]::GetDlgItem($dlg, 1), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
      $oldGone = Await { try { $p.Refresh(); $p.HasExited } catch { $true } } 40000
      $afterSize = 0
      if (Test-Path (Join-Path $root 'app\mnpdf.exe')) {
        $afterSize = (Get-Item (Join-Path $root 'app\mnpdf.exe')).Length
      }
      $newp = @(Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $p.Id })
      # the restarted copy sweeps the renamed old files itself; the old process
      # may still be exiting, so the sweep is given the same window it allows
      [void](Await { @(Get-ChildItem (Join-Path $root 'app') -Filter '*.old' -ErrorAction SilentlyContinue).Count -eq 0 } 12000)
      $leftovers = @(Get-ChildItem (Join-Path $root 'app') -Filter '*.old' -ErrorAction SilentlyContinue).Count
      if ($oldGone -and $afterSize -eq $payloadSize -and $newp.Count -ge 1 -and $leftovers -eq 0) {
        Pass 'an update installs itself and restarts the reader'
      } else {
        Fail 'an update installs itself and restarts the reader' `
          ("old exited: {0}; exe {1} -> {2} bytes (payload {3}); restarted: {4}; .old left: {5}" -f
           $oldGone, $beforeSize, $afterSize, $payloadSize, $newp.Count, $leftovers)
      }
      if ($newp.Count -ge 1) { try { $newp | Stop-Process -Force } catch {} }
    }
  } finally {
    Stop-OwnedApp $p
    $env:MNPDF_UPDATE_ZIP = $null; $env:MNPDF_UPDATE_DIR = $null
    $env:APPDATA = $realAppdata; $env:TEMP = $realTemp
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
  }
} finally {
  Restore-AppPref          # leave the user's update state exactly as we found it, on every path
  Remove-Item -LiteralPath $script:prefBackup,$script:prefMissing -Force -ErrorAction SilentlyContinue
}
Write-Output ''
Complete-Suite
