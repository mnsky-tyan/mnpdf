. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window + app.txt rules
# the app is per-monitor aware, so this process must be too (shared P/Invoke
# surface in lib): hit tests and rect reads then share the app's pixels
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class T2 {
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr hMenu, int nPos);
}
"@
function Cmd([IntPtr]$h, [int]$id) { [MN]::PostMessageW($h, 0x0111, [IntPtr]$id, [IntPtr]0) | Out-Null }

$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
# requires a machine we own, decided BEFORE anything below mutates user state:
# the wipe deletes %APPDATA%\mnpdf\* - every per-document sidecar with its page,
# zoom, fit and each hl= / dl= / pin= / rot= line - and Set-ForgedAppPref forges
# app.txt, so a run that has to SKIP must leave all of it untouched
Assert-NoRunningApp
$appPref = Join-Path $env:APPDATA "mnpdf\app.txt"
# the forge/restore rule (sentinel, durable marker, backup, watchdog) lives in
# tests/lib.ps1; it needs to know where the prefs are and a per-suite label.
# This must run before the wipe below: it is what captures the user's real prefs.
Init-PrefForge $appPref 'suite2'
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue
# the fixture lives in tests\ because build\ is all output and can be deleted
# wholesale; refresh build\arc.pdf (opened by the cooldown section below) and
# derive the mutating copy from the pristine fixture
Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") (Join-Path $PSScriptRoot "build\arc.pdf") -Force
Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") (Join-Path $PSScriptRoot "build\save-test.pdf") -Force
$origSize = (Get-Item (Join-Path $PSScriptRoot "build\save-test.pdf")).Length

$p = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "build\save-test.pdf")
$h = FindAppWindow $p.Id
Start-Sleep -Milliseconds 500
[void][MN]::MoveWindow($h, 60, 60, 1100, 800, $true)
Start-Sleep -Milliseconds 500

# --- 1. rotate page 1 clockwise (posted command; falls back to active page) ---
Cmd $h $CMD_ROTATE_CW
Start-Sleep -Milliseconds 500
$t = Title $h
Write-Output "rotate CW posted: '$t'  (visual check next)"
# pin the view to page 1: a rotation re-lays the layout, and when the relayout
# settles while the machine is loaded the viewport's current page can come to
# rest on the next page. Every assertion below is anchored to page 1 - the drag
# target, the page the save resumes at, the right-click that deletes the mark -
# so the view is driven back before any of them, not assumed.
for ($i = 0; $i -lt 20; $i++) {
  if ((Title $h) -match '^mnpdf 1/') { break }
  [void][MN]::PostMessageW($h, 0x0100, [IntPtr]0x21, [IntPtr]0)   # VK_PRIOR: page up
  Start-Sleep -Milliseconds 150
}

# --- 2. highlight the (rotated) page by drag, then check sidecar + dirty dot ---
# A rotation re-lays the page out, and a drag that lands while that is still moving
# selects nothing: measured once (the first full run after a reboot), the mark was
# simply absent from the sidecar. The sidecar is the witness and the command is
# cheap, so the drag is repeated until the mark exists instead of a layout race
# being reported as a defect of the app.
$scHl = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
for ($try = 0; $try -lt 4; $try++) {
  [MN]::PostMessageW($h, 0x0201, [IntPtr]1, (Lparam 400 300)) | Out-Null
  Start-Sleep -Milliseconds 50
  foreach ($x in 500, 600, 700) { [MN]::PostMessageW($h, 0x0200, [IntPtr]1, (Lparam $x 300)) | Out-Null; Start-Sleep -Milliseconds 40 }
  [MN]::PostMessageW($h, 0x0202, [IntPtr]0, (Lparam 700 300)) | Out-Null
  Start-Sleep -Milliseconds 300
  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_HIGHLIGHT, [IntPtr]0) | Out-Null   # Highlight (default color = yellow)
  Start-Sleep -Milliseconds 400
  if ((Test-Path $scHl) -and ((Get-Content $scHl -Raw -ErrorAction SilentlyContinue) -match 'hl=\d+,\d+,\d+,\d+')) { break }
  Start-Sleep -Milliseconds 700
}
Await { (Title $h) -notmatch '^mnpdf' } 8000 | Out-Null
$t = Title $h
if ($t -notmatch '^mnpdf') { Pass "dirty dot after edits (title prefixed)" }
else { Fail "dirty dot" "'$t'" }
$sc = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
Await { (Test-Path $sc) -and (Get-Content $sc -Raw -ErrorAction SilentlyContinue) -match 'hl=\d+,\d+,\d+,\d+' } 8000 | Out-Null
if (-not (Test-Path $sc)) { Fail "sidecar path" "missing: $sc" }
$side = if (Test-Path $sc) { Get-Content $sc -Raw -ErrorAction SilentlyContinue } else { '' }
if ($side -match 'hl=\d+,\d+,\d+,\d+') { Pass "highlight in sidecar" }
else { Fail "highlight in sidecar" "$side" }
if ($side -match 'rot=0,1') { Pass "rotation in sidecar" }
else { Fail "rotation in sidecar" "$side" }

# --- 3. save (Ctrl+S path): bakes everything, reloads clean ---
Cmd $h $CMD_SAVE
Await { (Title $h) -match "^mnpdf 1/$FixturePages" } 20000 | Out-Null
$t = Title $h
if ($t -match "^mnpdf 1/$FixturePages") { Pass "save + clean reload ('$t')" }
else { Fail "save" "'$t'" }
$newSize = (Get-Item (Join-Path $PSScriptRoot "build\save-test.pdf")).Length
if ($newSize -gt $origSize) { Pass "file grew: $origSize -> $newSize bytes (baked)" }
else { Fail "bake size" "file size $origSize -> $newSize" }
$raw = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "build\save-test.pdf"), [Text.Encoding]::GetEncoding(28591))
if ($raw -match '/Subtype\s*/Highlight') { Pass "baked as PDF Highlight annotation (reversible)" }
else { Fail "hl not baked as annot" "no Highlight annotation in saved file" }

# --- 4. titlebar toggle: client height grows when the caption is removed ---
$c1 = New-Object MNRect
[void][MN]::GetClientRect($h, [ref]$c1)
Cmd $h $CMD_TITLEBAR
Await { $c2 = New-Object MNRect; [void][MN]::GetClientRect($h, [ref]$c2); ($c2.B - $c2.T) -ne ($c1.B - $c1.T) } 8000 | Out-Null
$c2 = New-Object MNRect
[void][MN]::GetClientRect($h, [ref]$c2)
$dh = ($c2.B - $c2.T) - ($c1.B - $c1.T)
if ($dh -gt 15) { Pass "titlebar hidden (client +$dh px)" }
else { Fail "titlebar hide" "dh=$dh" }
Cmd $h $CMD_TITLEBAR
Await { $c3 = New-Object MNRect; [void][MN]::GetClientRect($h, [ref]$c3); ($c3.B - $c3.T) -ne ($c2.B - $c2.T) } 8000 | Out-Null
$c3 = New-Object MNRect
[void][MN]::GetClientRect($h, [ref]$c3)
$dh2 = ($c2.B - $c2.T) - ($c3.B - $c3.T)
if ($dh2 -gt 15) { Pass "titlebar restored (client -$dh2 px)" }
else { Fail "titlebar show" "dh=$dh2" }

# --- 5. quit via menu, reopen baked file, verify highlight survived in the PDF ---
Cmd $h $CMD_QUIT
Await { $p.Refresh(); $p.HasExited } 8000 | Out-Null
$p.Refresh()
if ($p.HasExited) { Pass "quit" } else { Fail "quit" "still running" }
$scq = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
$savedPage = if (Test-Path $scq) { ([regex]::Match((Get-Content $scq -Raw), 'page=(\d+)')).Groups[1].Value } else { '1' }
$p2 = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "build\save-test.pdf")
$h2 = FindAppWindow $p2.Id
$t = Title $h2
if ($t -match "mnpdf $savedPage/$FixturePages") { Pass "baked file reopens at saved page $savedPage ('$t')" }
else { Fail "reopen baked" "'$t'" }

# --- 5b. a mark deleted after baking must not resurrect across quit-without-save ---
# does a context menu opened at (x,y) actually target a highlight? its branch
# carries item 117 (Delete highlight); the plain page menu never does
function CloseMenu([IntPtr]$h) {
  # a command posted while the menu is modal is swallowed: keep pressing Esc
  # until the popup window disappears, so later commands are delivered
  for ($i = 0; $i -lt 20; $i++) {
    if ([MN]::FindWindowW("#32768", [IntPtr]::Zero) -eq [IntPtr]::Zero) { return }
    [void][MN]::PostMessageW($h, 0x0100, [IntPtr]0x1B, [IntPtr]0)
    Start-Sleep -Milliseconds 100
  }
}

function MenuHasHl([IntPtr]$h, [int]$x, [int]$y) {
  [void][MN]::PostMessageW($h, 0x0204, [IntPtr]2, (Lparam $x $y))
  Start-Sleep -Milliseconds 80
  [void][MN]::PostMessageW($h, 0x0205, [IntPtr]0, (Lparam $x $y))
  Start-Sleep -Milliseconds 450
  $m = [MN]::FindWindowW("#32768", [IntPtr]::Zero)
  $hl = $false
  if ($m -ne [IntPtr]::Zero) {
    $hm = [T2]::GetMenu($m)
    if ($hm -ne [IntPtr]::Zero) {
      $n = [T2]::GetMenuItemCount($hm)
      for ($i = 0; $i -lt $n; $i++) { if ([T2]::GetMenuItemID($hm, $i) -eq $CMD_DELETE_HL) { $hl = $true; break } }
    }
    CloseMenu $h
  }
  return $hl
}
if ($p2) {
  # a real deletion only reaches the sidecar when the highlight itself is targeted
  $scq2 = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
  $dlBefore = 0
  if (Test-Path $scq2) { $dlBefore = ([regex]::Matches((Get-Content $scq2 -Raw), 'dl=h,')).Count }
  # right-click over the baked highlight (RB up/down lets the shell raise
  # WM_CONTEXTMENU itself), read the menu, Esc closes it, THEN the queued
  # command can be delivered - a command posted while the menu is modal is swallowed
  MenuHasHl $h2 500 300 | Out-Null     # open the menu over the mark, Esc closes it
  Cmd $h2 $CMD_DELETE_HL               # delete the baked highlight
  Await { (Test-Path $scq2) -and (([regex]::Matches('' + (Get-Content $scq2 -Raw -ErrorAction SilentlyContinue), 'dl=h,')).Count) -gt $dlBefore } 30000 | Out-Null
  $sd = if (Test-Path $scq2) { Get-Content $scq2 -Raw -ErrorAction SilentlyContinue } else { '' }
  $dlAfter = ([regex]::Matches($sd, 'dl=h,')).Count
  if ($dlAfter -eq $dlBefore + 1) { Pass "baked highlight deleted after reopen" }
  else { Fail "baked hl delete" "delete produced no dl= line ($dlBefore -> $dlAfter): $sd" }
  Cmd $h2 $CMD_QUIT
  Await { $p2.Refresh(); $p2.HasExited } 8000 | Out-Null
  $p2.Refresh()
}

# another delete attempt must find nothing left: the dl= list must not grow
$p3 = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "build\save-test.pdf")
$h3 = FindAppWindow $p3.Id
MenuHasHl $h3 500 300 | Out-Null
Cmd $h3 $CMD_DELETE_HL
Start-Sleep -Milliseconds 2500   # negative assertion: wait long enough that any flush WOULD have landed
$dlAgain = ([regex]::Matches('' + (Get-Content $scq2 -Raw -ErrorAction SilentlyContinue), 'dl=h,')).Count
if ($dlAgain -gt $dlAfter) { Fail "deleted baked highlight resurrected" "$dlAfter -> $dlAgain" }
else { Pass "deleted baked highlight stays deleted" }
if (-not $p3.HasExited) { Cmd $h3 $CMD_QUIT; Await { $p3.HasExited } 8000 | Out-Null }
if (-not $p3.HasExited) { $p3.Kill() }   # same graceful-then-force rule as p4/p5

# ---- update-check cooldown: one network attempt per hour, persisted ----
# inside the window: the launch must spend no request, so the stamp is untouched
# (the forge template, and the tag it remembers, come from tests/lib.ps1 and the
# shipped README.txt, so nothing here freezes its own copy of either)
$now = Set-ForgedAppPref 300
$p4 = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "build\arc.pdf")
$h4 = FindAppWindow $p4.Id
Start-Sleep -Milliseconds 6000          # long enough that a check would have landed if one ran
$stamped = if (Test-Path $appPref) { ([regex]::Match((Get-Content $appPref -Raw), '^updcheck=(\d+)', 'Multiline')).Groups[1].Value } else { '' }
if ($stamped -eq ($now - 300)) { Pass "cooldown: launch inside the window spent no request" }
else { Fail "cooldown inside window" "re-stamped the clock: $stamped, expected $($now - 300)" }
if (-not $p4.HasExited) { Cmd $h4 $CMD_QUIT; Await { $p4.HasExited } 8000 | Out-Null }
if (-not $p4.HasExited) { $p4.Kill() }
# expired window: the launch must check again and re-stamp close to now
$now = Set-ForgedAppPref 7200
$p5 = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "build\arc.pdf")
$h5 = FindAppWindow $p5.Id
$fresh = Await { $v = if (Test-Path $appPref) { ([regex]::Match((Get-Content $appPref -Raw), '^updcheck=(\d+)', 'Multiline')).Groups[1].Value } else { '' }; ($v -match '^\d+$') -and ([int]$v) -gt ($now - 7000) } 25000
if ($fresh) { Pass "cooldown expiry: launch after an hour checked again" }
else { Fail "cooldown expiry" "did not re-check" }
if (-not $p5.HasExited) { Cmd $h5 $CMD_QUIT; Await { $p5.HasExited } 8000 | Out-Null }
if (-not $p5.HasExited) { $p5.Kill() }

Restore-AppPref          # put the user's real app.txt back on every path
Remove-Item -LiteralPath $script:prefBackup,$script:prefMissing -Force -ErrorAction SilentlyContinue
Write-Output ""
Complete-Suite
