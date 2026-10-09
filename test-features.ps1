. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
# the app is per-monitor aware, so this process must be too (shared P/Invoke
# surface in lib): wheel deltas and MoveWindow then land in real pixels
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null


$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
# The real print dialog is modal and a posted-message harness cannot click it,
# so this suite drives the raster path the print dialog feeds and never opens
# one. The app reads this at print time, so it must be set before it launches.
$probe = Join-Path $env:TEMP "mnpdf-print-probe.txt"
$env:MNPDF_PRINT_PROBE = $probe
# requires a fresh launch we own: fit-mode/zoom assumptions only hold on a clean
# state, and a run that must not disturb a running instance finds out FIRST -
# deciding after the wipe would delete every per-document sidecar (page, zoom,
# fit and each hl= / dl= / pin= / rot= line) before exiting
Assert-NoRunningApp
$Pdf = Join-Path $PSScriptRoot "build\arc.pdf"
Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") $Pdf -Force   # work on a fresh copy
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
$proc = Launch (Resolve-AppExe) $Pdf
$p = $proc
$h = FindAppWindow $p.Id
Start-Sleep -Milliseconds 500

$t0 = Title $h
Write-Output "start: '$t0'"

# --- T1: scroll to page 3 with the wheel (go-to-page was removed by request) ---
# scroll notch by notch and stop as soon as page 3 is current: a fixed notch
# count lands somewhere else whenever the machine is loaded
$reached3 = $false
for ($i = 0; $i -lt 60 -and -not $reached3; $i++) {
  [MN]::PostMessageW($h, 0x020A, [IntPtr]0xFF880000, (Lparam 550 400)) | Out-Null   # wheel down
  $reached3 = Await { (Title $h) -match "mnpdf 3/$FixturePages" } 500
}
$t = Title $h
if ($reached3) { Pass "wheel scroll to page 3 ('$t')" }
else { Fail "wheel scroll to page 3" "'$t'" }

# --- T2: autosave after wheel scrolling ---
$w = [IntPtr]((-120) -shl 16)
for ($i = 0; $i -lt 4; $i++) { [MN]::PostMessageW($h, 0x020A, $w, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 80 }
$tScroll = Title $h
$wantPage = if ($tScroll -match 'mnpdf (\d+)/') { [int]$Matches[1] } else { 0 }
$sidecar = $null
Await {                                                   # 800ms debounce tick, polled
  $script:sidecar = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
  $script:sidecar -and (Get-Content $script:sidecar.FullName -Raw -ErrorAction SilentlyContinue) -match "page=$wantPage`n"
} 8000 | Out-Null
$sidecar = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($sidecar) {
  $content = Get-Content $sidecar.FullName -Raw
  Write-Output ("sidecar: " + ($content -replace "`n", ' | '))
  if ($content -match 'page=(\d+)') { Pass "autosave wrote page=$($Matches[1]) (title was '$tScroll')" }
  else { Fail "sidecar content" $content }
} else { Fail "autosave file" "no sidecar file" }
if (Test-Path "$env:APPDATA\mnpdf\last.txt") { Pass "last.txt written" }
else { Fail "last.txt" "not written" }

# --- T3: cross-page drag selection (zoom out until two pages fit) ---
for ($i = 0; $i -lt 8; $i++) {
  [MN]::PostMessageW($h, 0x0100, [IntPtr]0x6D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 60
  [MN]::PostMessageW($h, 0x0101, [IntPtr]0x6D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 300
$t = Title $h
Write-Output ("zoomed out: '$t'")
# drag from lower page 1 across the gap into upper page 2
[MN]::PostMessageW($h, 0x0201, [IntPtr]1, (Lparam 540 260)) | Out-Null
Start-Sleep -Milliseconds 60
foreach ($y in 320, 380, 440, 500, 560) { [MN]::PostMessageW($h, 0x0200, [IntPtr]1, (Lparam 540 $y)) | Out-Null; Start-Sleep -Milliseconds 40 }
[MN]::PostMessageW($h, 0x0202, [IntPtr]0, (Lparam 540 560)) | Out-Null
Start-Sleep -Milliseconds 200
$prev = GetClip
[MN]::PostMessageW($h, 0x0301, [IntPtr]0, [IntPtr]0) | Out-Null          # WM_COPY
# nothing to poll for: the clipboard gate is closed on this box, so GetClip
# always returns '' and the check below can only take the SKIP branch
Start-Sleep -Milliseconds 4000
$t = GetClip
$pages = ($t -split "`r`n").Count
if (-not $script:clipOk) { Write-Output "SKIP cross-page copy: clipboard locked (verified previously)" }
elseif (-not (Test-ClipboardRoundTrip)) { Write-Output "SKIP cross-page copy: clipboard not writable on this box" }
elseif ($t -and $t -match "`r`n") { Pass "cross-page copy: [$($t.Length) chars, $pages lines] '$($t.Substring(0, [Math]::Min(60, $t.Length)))' ..." }
else { Fail "cross-page copy" "got: '$t'" }

# --- T4: quit, then plain-launch reopen restores the saved page ---
$scq = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
$savedPage = if ($scq) { [int]([regex]::Match((Get-Content $scq.FullName -Raw), 'page=(\d+)')).Groups[1].Value } else { 0 }
[MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]0) | Out-Null       # Quit
Await { $p.Refresh(); $p.HasExited } 8000 | Out-Null
$p.Refresh()
if ($p.HasExited) { Pass "quit menu exited the app" }
else { Fail "quit" "still running" }
# NO arguments (the shared Start-App starts it argument-less for an empty Doc), so
# the app reopens whatever last.txt points at - which is what this asserts.
$p2 = Launch (Resolve-AppExe) ''
$h2 = FindAppWindow $p2.Id
# wait for the page the sidecar promised, not just for any title: the zoom and
# fit restore land before the scroll-to-saved-page does, so a bare 'mnpdf N/13'
# can be an intermediate frame (the old fixed sleep just happened to outlast it)
[void](Await { (Title $h2) -match "mnpdf $savedPage/$FixturePages" } 15000)
$t = Title $h2
if ($t -match "mnpdf $savedPage/$FixturePages") { Pass "reopen last doc at page $savedPage ('$t')" }
else { Fail "reopen page" "expected page $savedPage, got '$t'" }

# --- T5: print renders clean (no annotations) and never moves the view ---
# The real print dialog is modal, and a posted-message harness cannot click it,
# so the probe renders the raster path for real and skips the dialog. It writes
# the clean checksum, the same page's annotated checksum, the ink count and the
# page's own annotation count, which is what separates "a page rendered" from
# "the print path dropped the annotations".
Remove-Item $probe -ErrorAction SilentlyContinue
$tBefore = Title $h2
[MN]::PostMessageW($h2, 0x0111, [IntPtr]$CMD_PRINT, [IntPtr]0) | Out-Null          # Print
# wait for the CONTENT, never just the file's existence: a poll that lands
# mid-write reads a 0-byte file, PowerShell casts $null to 0, and the run fails
# for a reason that has nothing to do with the code
[void](Await { (Get-Content $probe -Raw -ErrorAction SilentlyContinue) -match '^pages=\d+' } 15000)
function Probe([string]$Path) {
  $txt = Get-Content $Path -Raw
  $kv = @{}
  # split into key=value tokens: a regex like '.*annots=(\d+)' also matches inside
  # 'inkannots=', so fields are parsed by name, never by substring
  foreach ($tok in ($txt -split '\s+')) {
    if ($tok -match '^([A-Za-z]+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
  }
  @{ pages = [int]$kv['pages']; w = [int]$kv['w']; h = [int]$kv['h']; ink = [long]$kv['ink'];
     annots = [int]$kv['annots']; inkannots = [int]$kv['inkannots'];
     clean = [string]$kv['clean']; annot = [string]$kv['annot'] }
}
function CheckPrintProbe([string]$Doc, [int]$Pages) {
  if (-not (Test-Path $probe)) { Fail "print probe $Doc" "no probe output from $Doc"; return }
  $pr = Probe $probe
  if ($pr.pages -ne $Pages -or $pr.w -le 0 -or $pr.h -le 0) {
    Fail "print raster on $Doc" "pages=$($pr.pages) w=$($pr.w) h=$($pr.h)"
  } elseif ($pr.ink -le 0) {
    Fail "print ink on $Doc" "clean page had no ink"
  } elseif ($pr.inkannots -gt 0 -and $pr.clean -eq $pr.annot) {
    Fail "print clean on $Doc" "page has $($pr.inkannots) ink-drawing annotations but clean=$($pr.clean) equals annot=$($pr.annot) - the print path did NOT drop them"
  } elseif ($pr.inkannots -eq 0 -and $pr.clean -ne $pr.annot) {
    Fail "print clean on $Doc" "no ink annotations yet clean<>annot"
  } else {
    Pass "print renders clean ($($pr.pages) pages, $($pr.w)x$($pr.h), $($pr.annots) annots/$($pr.inkannots) with ink, ink=$($pr.ink))"
  }
}
CheckPrintProbe 'arc.pdf' $FixturePages
$t = Title $h2
if ($t -eq $tBefore) { Pass "print left the view untouched on arc.pdf" }
else { Fail "print moved the view on arc.pdf" "'$tBefore' -> '$t'" }
# the same contract on a page that really carries visible annotation ink: the
# shipped fixture arc.pdf has only Link annotations, which pdfium paints no ink
# for, so clean==annot there is correct and asserts nothing
$pa = Launch (Resolve-AppExe) (Join-Path $PSScriptRoot "tests\arc-annot.pdf")
$ha = FindAppWindow $pa.Id
[void](Await { (Title $ha) -match "mnpdf 1/$FixturePages" } 15000)
$taBefore = Title $ha
Remove-Item $probe -ErrorAction SilentlyContinue
[MN]::PostMessageW($ha, 0x0111, [IntPtr]$CMD_PRINT, [IntPtr]0) | Out-Null
[void](Await { (Get-Content $probe -Raw -ErrorAction SilentlyContinue) -match '^pages=\d+' } 15000)
CheckPrintProbe 'arc-annot.pdf' 1
$ta = Title $ha
if ($ta -eq $taBefore) { Pass "print left the view untouched on arc-annot.pdf" }
else { Fail "print moved the view on arc-annot.pdf" "'$taBefore' -> '$ta'" }
$pa | Stop-Process -Force

# the reopen instance goes home too: a suite that leaves an app running fails
# the next suite's machine-is-free pre-flight
if (-not $p2.HasExited) {
  [void][MN]::PostMessageW($h2, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]::Zero)
  [void](Await { $p2.HasExited } 15000)
}
if (-not $p2.HasExited) { $p2 | Stop-Process -Force }

Write-Output ""
Complete-Suite
