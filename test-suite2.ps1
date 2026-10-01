. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window + app.txt rules
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public struct PRECT { public int L; public int T; public int R; public int B; }
public static class T2 {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int h2, bool r);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out PRECT r);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr hMenu, int nPos);
}
"@
[void][T2]::SetProcessDpiAwarenessContext([IntPtr](-4))
$failures = New-Object System.Collections.Generic.List[string]
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function Title([IntPtr]$h) { $sb = New-Object System.Text.StringBuilder 256; [void][T2]::GetWindowTextW($h, $sb, 256); $sb.ToString() }
function Cmd([IntPtr]$h, [int]$id) { [T2]::PostMessageW($h, 0x0111, [IntPtr]$id, [IntPtr]0) | Out-Null }

# sidecar file for a pdf path: FNV-1a over UTF-16 bytes incl. the terminator, same
# hash the app uses (sidecarPathFor); BigInteger keeps the 64-bit wraps exact
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

$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
# requires a machine we own, decided BEFORE anything below mutates user state:
# the wipe deletes %APPDATA%\mnpdf\* - every per-document sidecar with its page,
# zoom, fit and each hl= / dl= / pin= / rot= line - and StampPref forges app.txt,
# so a run that has to SKIP must leave all of it untouched (the invariant the
# other suites follow: never disturb a running instance)
$existing = Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }
if ($existing) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }
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

$p = Launch (Join-Path $PSScriptRoot "build\mnpdf.exe") (Join-Path $PSScriptRoot "build\save-test.pdf")
$h = FindAppWindow $p.Id
Start-Sleep -Milliseconds 500
[void][T2]::MoveWindow($h, 60, 60, 1100, 800, $true)
Start-Sleep -Milliseconds 500

# --- 1. rotate page 1 clockwise (posted command; falls back to active page) ---
Cmd $h 133
Start-Sleep -Milliseconds 500
$t = Title $h
Write-Output "rotate CW posted: '$t'  (visual check next)"

# --- 2. highlight the (rotated) page by drag, then check sidecar + dirty dot ---
# A rotation re-lays the page out, and a drag that lands while that is still moving
# selects nothing: measured once (the first full run after a reboot), the mark was
# simply absent from the sidecar. The sidecar is the witness and the command is
# cheap, so the drag is repeated until the mark exists instead of a layout race
# being reported as a defect of the app.
$scHl = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
for ($try = 0; $try -lt 4; $try++) {
  [T2]::PostMessageW($h, 0x0201, [IntPtr]1, (Lparam 400 300)) | Out-Null
  Start-Sleep -Milliseconds 50
  foreach ($x in 500, 600, 700) { [T2]::PostMessageW($h, 0x0200, [IntPtr]1, (Lparam $x 300)) | Out-Null; Start-Sleep -Milliseconds 40 }
  [T2]::PostMessageW($h, 0x0202, [IntPtr]0, (Lparam 700 300)) | Out-Null
  Start-Sleep -Milliseconds 300
  [T2]::PostMessageW($h, 0x0111, [IntPtr]135, [IntPtr]0) | Out-Null   # Highlight (default color = yellow)
  Start-Sleep -Milliseconds 400
  if ((Test-Path $scHl) -and ((Get-Content $scHl -Raw -ErrorAction SilentlyContinue) -match 'hl=\d+,\d+,\d+,\d+')) { break }
  Start-Sleep -Milliseconds 700
}
Await { (Title $h) -notmatch '^mnpdf' } 8000 | Out-Null
$t = Title $h
if ($t -notmatch '^mnpdf') { Write-Output "PASS dirty dot after edits (title prefixed)" }
else { $failures.Add("dirty dot"); Write-Output "FAIL dirty dot ('$t')" }
$sc = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
Await { (Test-Path $sc) -and (Get-Content $sc -Raw -ErrorAction SilentlyContinue) -match 'hl=\d+,\d+,\d+,\d+' } 8000 | Out-Null
if (-not (Test-Path $sc)) { $failures.Add("sidecar path"); Write-Output "FAIL sidecar missing: $sc" }
$side = if (Test-Path $sc) { Get-Content $sc -Raw } else { '' }
if ($side -match 'hl=\d+,\d+,\d+,\d+') { Write-Output "PASS highlight in sidecar" }
else { $failures.Add("hl sidecar"); Write-Output "FAIL highlight in sidecar: $side" }
if ($side -match 'rot=0,1') { Write-Output "PASS rotation in sidecar" }
else { $failures.Add("rot sidecar"); Write-Output "FAIL rotation in sidecar: $side" }

# --- 3. save (Ctrl+S path): bakes everything, reloads clean ---
Cmd $h 118
Await { (Title $h) -match '^mnpdf 1/13' } 20000 | Out-Null
$t = Title $h
if ($t -match '^mnpdf 1/13') { Write-Output "PASS save + clean reload ('$t')" }
else { $failures.Add("save"); Write-Output "FAIL save ('$t')" }
$newSize = (Get-Item (Join-Path $PSScriptRoot "build\save-test.pdf")).Length
if ($newSize -gt $origSize) { Write-Output "PASS file grew: $origSize -> $newSize bytes (baked)" }
else { $failures.Add("bake size"); Write-Output "FAIL file size $origSize -> $newSize" }
$raw = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "build\save-test.pdf"), [Text.Encoding]::GetEncoding(28591))
if ($raw -match '/Subtype\s*/Highlight') { Write-Output "PASS baked as PDF Highlight annotation (reversible)" }
else { $failures.Add("hl not baked as annot"); Write-Output "FAIL no Highlight annotation in saved file" }

# --- 4. titlebar toggle: client height grows when the caption is removed ---
$c1 = New-Object PRECT
[void][T2]::GetClientRect($h, [ref]$c1)
Cmd $h 113
Await { $c2 = New-Object PRECT; [void][T2]::GetClientRect($h, [ref]$c2); ($c2.B - $c2.T) -ne ($c1.B - $c1.T) } 8000 | Out-Null
$c2 = New-Object PRECT
[void][T2]::GetClientRect($h, [ref]$c2)
$dh = ($c2.B - $c2.T) - ($c1.B - $c1.T)
if ($dh -gt 15) { Write-Output "PASS titlebar hidden (client +$dh px)" }
else { $failures.Add("titlebar hide"); Write-Output "FAIL titlebar hide (dh=$dh)" }
Cmd $h 113
Await { $c3 = New-Object PRECT; [void][T2]::GetClientRect($h, [ref]$c3); ($c3.B - $c3.T) -ne ($c2.B - $c2.T) } 8000 | Out-Null
$c3 = New-Object PRECT
[void][T2]::GetClientRect($h, [ref]$c3)
$dh2 = ($c2.B - $c2.T) - ($c3.B - $c3.T)
if ($dh2 -gt 15) { Write-Output "PASS titlebar restored (client -$dh2 px)" }
else { $failures.Add("titlebar show"); Write-Output "FAIL titlebar show (dh=$dh2)" }

# --- 5. quit via menu, reopen baked file, verify highlight survived in the PDF ---
Cmd $h 112
Await { $p.Refresh(); $p.HasExited } 8000 | Out-Null
$p.Refresh()
if ($p.HasExited) { Write-Output "PASS quit" } else { $failures.Add("quit"); Write-Output "FAIL quit" }
$scq = SidecarFor (Join-Path $PSScriptRoot "build\save-test.pdf")
$savedPage = if (Test-Path $scq) { ([regex]::Match((Get-Content $scq -Raw), 'page=(\d+)')).Groups[1].Value } else { '1' }
$p2 = Launch (Join-Path $PSScriptRoot "build\mnpdf.exe") (Join-Path $PSScriptRoot "build\save-test.pdf")
$h2 = FindAppWindow $p2.Id
$t = Title $h2
if ($t -match "mnpdf $savedPage/13") { Write-Output "PASS baked file reopens at saved page $savedPage ('$t')" }
else { $failures.Add("reopen baked"); Write-Output "FAIL reopen: '$t'" }

# --- 5b. a mark deleted after baking must not resurrect across quit-without-save ---
function Lparam([int]$x, [int]$y) { [IntPtr]((($y -shl 16) -bor ($x -band 0xFFFF))) }
# does a context menu opened at (x,y) actually target a highlight? its branch
# carries item 117 (Delete highlight); the plain page menu never does
function CloseMenu([IntPtr]$h) {
  # a command posted while the menu is modal is swallowed: keep pressing Esc
  # until the popup window disappears, so later commands are delivered
  for ($i = 0; $i -lt 20; $i++) {
    if ([T2]::FindWindowW("#32768", $null) -eq [IntPtr]::Zero) { return }
    [void][T2]::PostMessageW($h, 0x0100, [IntPtr]0x1B, [IntPtr]0)
    Start-Sleep -Milliseconds 100
  }
}

function MenuHasHl([IntPtr]$h, [int]$x, [int]$y) {
  [void][T2]::PostMessageW($h, 0x0204, [IntPtr]2, (Lparam $x $y))
  Start-Sleep -Milliseconds 80
  [void][T2]::PostMessageW($h, 0x0205, [IntPtr]0, (Lparam $x $y))
  Start-Sleep -Milliseconds 450
  $m = [T2]::FindWindowW("#32768", $null)
  $hl = $false
  if ($m -ne [IntPtr]::Zero) {
    $hm = [T2]::GetMenu($m)
    if ($hm -ne [IntPtr]::Zero) {
      $n = [T2]::GetMenuItemCount($hm)
      for ($i = 0; $i -lt $n; $i++) { if ([T2]::GetMenuItemID($hm, $i) -eq 117) { $hl = $true; break } }
    }
    CloseMenu $h
  }
  return $hl
}

function MenuAt([IntPtr]$h, [int]$x, [int]$y) {
  [void][T2]::PostMessageW($h, 0x0204, [IntPtr]2, (Lparam $x $y))
  Start-Sleep -Milliseconds 80
  [void][T2]::PostMessageW($h, 0x0205, [IntPtr]0, (Lparam $x $y))
  Start-Sleep -Milliseconds 450
  $m = [T2]::FindWindowW("#32768", $null)
  if ($m -ne [IntPtr]::Zero) { CloseMenu $h }
  return $m -ne [IntPtr]::Zero
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
  Cmd $h2 117                           # delete the baked highlight
  Await { (Test-Path $scq2) -and (([regex]::Matches('' + (Get-Content $scq2 -Raw -ErrorAction SilentlyContinue), 'dl=h,')).Count) -gt $dlBefore } 30000 | Out-Null
  $sd = if (Test-Path $scq2) { Get-Content $scq2 -Raw } else { '' }
  $dlAfter = ([regex]::Matches($sd, 'dl=h,')).Count
  if ($dlAfter -eq $dlBefore + 1) { Write-Output "PASS baked highlight deleted after reopen" }
  else { $failures.Add("baked hl delete"); Write-Output "FAIL delete produced no dl= line ($dlBefore -> $dlAfter): $sd" }
  Cmd $h2 112
  Await { $p2.Refresh(); $p2.HasExited } 8000 | Out-Null
  $p2.Refresh()
}

# another delete attempt must find nothing left: the dl= list must not grow
$p3 = Launch (Join-Path $PSScriptRoot "build\mnpdf.exe") (Join-Path $PSScriptRoot "build\save-test.pdf")
$h3 = FindAppWindow $p3.Id
MenuHasHl $h3 500 300 | Out-Null
Cmd $h3 117
Start-Sleep -Milliseconds 2500   # negative assertion: wait long enough that any flush WOULD have landed
$dlAgain = ([regex]::Matches((Get-Content $scq2 -Raw), 'dl=h,')).Count
if ($dlAgain -gt $dlAfter) { $failures.Add("resurrect"); Write-Output "FAIL deleted baked highlight resurrected" }
else { Write-Output "PASS deleted baked highlight stays deleted" }
$p3.CloseMainWindow() | Out-Null

# ---- update-check cooldown: one network attempt per hour, persisted ----
# PowerShell's Get-Date -UFormat %s is offset by the local UTC offset, so the
# stamp must be built the same way the app does ([[DateTimeOffset]::UtcNow)
function StampPref([int]$SecondsAgo) {
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  Set-Content -LiteralPath $appPref -Value ("titlebar=1`nautosave=1`nhlcolor=0`npincolor=5`npalnext=0`nupdcheck={0}`nupdtag=v2.0.2`n{1}`n" -f ($now - $SecondsAgo), $script:prefSentinel)
  Set-Content -LiteralPath $script:prefMarker -Value ''   # durable: an expired-cooldown launch rewrites app.txt and erases the in-file sentinel
  return $now
}
function ReadStamp() {
  if (-not (Test-Path $appPref)) { return "" }
  $m = Select-String -Path $appPref -Pattern '^updcheck=(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
  return $m
}
# inside the window: the launch must spend no request, so the stamp is untouched
$now = StampPref 300
$p4 = Launch (Join-Path $PSScriptRoot "build\mnpdf.exe") (Join-Path $PSScriptRoot "build\arc.pdf")
$h4 = FindAppWindow $p4.Id
Start-Sleep -Milliseconds 6000          # long enough that a check would have landed if one ran
if ((ReadStamp) -eq ($now - 300)) { Write-Output "PASS cooldown: launch inside the window spent no request" }
else { $failures.Add("cooldown-inside"); Write-Output "FAIL cooldown inside window re-stamped the clock: $(ReadStamp), expected $($now - 300)" }
if (-not $p4.HasExited) { Cmd $h4 112; Await { $p4.HasExited } 8000 | Out-Null }
if (-not $p4.HasExited) { $p4.Kill() }
# expired window: the launch must check again and re-stamp close to now
$now = StampPref 7200
$p5 = Launch (Join-Path $PSScriptRoot "build\mnpdf.exe") (Join-Path $PSScriptRoot "build\arc.pdf")
$h5 = FindAppWindow $p5.Id
$fresh = Await { $v = ReadStamp; ($v -match '^\d+$') -and ([int]$v) -gt ($now - 7000) } 25000
if ($fresh) { Write-Output "PASS cooldown expiry: launch after an hour checked again" }
else { $failures.Add("cooldown-expired"); Write-Output "FAIL cooldown expiry did not re-check: '$(ReadStamp)' vs $now" }
if (-not $p5.HasExited) { Cmd $h5 112; Await { $p5.HasExited } 8000 | Out-Null }
if (-not $p5.HasExited) { $p5.Kill() }

Restore-AppPref          # put the user's real app.txt back on every path
Remove-Item -LiteralPath $script:prefBackup,$script:prefMissing -Force -ErrorAction SilentlyContinue
Write-Output ""
if ($failures.Count) { Write-Output "RESULT: $($failures.Count) FAILURE(S)"; exit 1 }
Write-Output "RESULT: ALL PASS"
