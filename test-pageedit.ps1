. "$PSScriptRoot\tests\lib.ps1"
# the app is per-monitor aware, so this process must be too (shared P/Invoke
# surface in lib): the drawer's own pixel sizes then land in real pixels
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Title([IntPtr]$h) { $sb = New-Object System.Text.StringBuilder 256; [void][MN]::GetWindowTextW($h, $sb, 256); $sb.ToString() }
function PageOf([string]$t) { if ($t -match 'mnpdf (\d+)/(\d+)') { return @([int]$Matches[1], [int]$Matches[2]) } return @(0, 0) }
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function Text-Of([IntPtr]$h) { $sb = New-Object System.Text.StringBuilder 256; [void][MN]::GetWindowTextW($h, $sb, 256); $sb.ToString() }
function Find-Class([string]$cl) { [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, $cl, [IntPtr]::Zero) }
function Wait-Class([string]$cl) { $script:found = [IntPtr]::Zero; [void](Await { $script:found = Find-Class $cl } 10000); return $script:found }
function Quit-Proc($p) {
  if (-not $p -or $p.HasExited) { return }
  $w = FindAppWindow $p.Id
  if ($w -ne [IntPtr]::Zero) { [MN]::PostMessageW($w, 0x0111, [IntPtr]$CMD_QUIT, [IntPtr]0) | Out-Null }
  if (-not $p.WaitForExit(8000)) { $p | Stop-Process -Force }
}

# Most of these commands sit behind a modal file dialog, which a posted-message
# harness cannot click. The app therefore reads MNPDF_HOOK at launch and calls the
# same function its menu calls, with the arguments the dialog would have
# collected; "verb|a|b" runs one verb and "a;;b" runs several in order. Each case
# gets its own instance, so no state leaks from one to the next.
$env:MNPDF_VERBOSE = "1"

Assert-NoRunningApp
$exe = Resolve-AppExe
$appPref = Join-Path $env:APPDATA 'mnpdf\app.txt'
$arc = Join-Path $PSScriptRoot 'tests\arc.pdf'
$jpg = Join-Path $PSScriptRoot 'tests\sig-sample.jpg'
$work = Join-Path $PSScriptRoot 'build\pageedit'
$scratch = Join-Path $env:TEMP "mnpdf-pageedit-$PID"
foreach ($d in @($work, $scratch)) {
  if (Test-Path $d) { Remove-Item $d -Recurse -Force }
  New-Item -ItemType Directory -Path $d -Force | Out-Null
}

# Every case gets its own copy of the fixture, because the sidecar that hangs off
# a document remembers rotation, zoom and page: cases that shared one path would
# inherit each other's results (a rotated page 3 was still rotated three cases
# later, and "rotate page 3" then correctly changed nothing). A fresh copy has no
# sidecar, so each case starts from the fixture as shipped.
$script:caseNo = 0
function New-Case {
  $script:caseNo++
  $dst = Join-Path $scratch ("case-{0}.pdf" -f $script:caseNo)
  Copy-Item $arc $dst -Force
  return $dst
}

# Launch one hooked instance, read what the reader can see (title, page rasters,
# whatever the run left on disk), then quit it.
function Run-Hooked([string]$Spec, [string]$Doc, [string]$HashFile = '') {
  if (-not $Doc) { $Doc = New-Case }
  if ($HashFile) { Remove-Item $HashFile -ErrorAction SilentlyContinue }
  $env:MNPDF_HOOK = $Spec
  $p = $null
  try {
    $p = Launch $exe $Doc
    $h = FindAppWindow $p.Id
    [void](Await { (Title $h) -match 'mnpdf \d+/\d+' } 15000)
    $title = Title $h
    $hashes = @()
    if ($HashFile) {
      $hashes = @(Get-Content $HashFile -ErrorAction SilentlyContinue |
                  Where-Object { $_ -match '^\d+ \d+$' } |
                  ForEach-Object { [uint64]($_ -split ' ')[1] })
    }
    return [pscustomobject]@{ Title = $title; Page = (PageOf $title); Hashes = $hashes }
  } finally {
    Quit-Proc $p
    $env:MNPDF_HOOK = $null
  }
}

try {
  Init-PrefForge $appPref 'pageedit'
  Write-Output '=== page editing, drawers and stamps ==='

  # --- 1. the reference document ---------------------------------------------
  $refFile = Join-Path $scratch 'ref.txt'
  $ref = Run-Hooked ("pagehash|{0}" -f $refFile) $null $refFile
  if ($ref.Page[1] -eq $FixturePages -and $ref.Hashes.Count -eq $FixturePages) {
    Pass "fixture has $FixturePages pages and every page rasterizes"
  } else {
    Fail 'fixture' "title '$($ref.Title)', $($ref.Hashes.Count) page hashes"
  }

  # --- 2. split exports exactly the range it was asked for --------------------
  $split = Join-Path $scratch 'pages-1-3.pdf'
  [void](Run-Hooked ("split|1-3|{0}" -f $split) (New-Case))
  [void](Await { Test-Path $split } 5000)
  if (Test-Path $split) {
    $back = Run-Hooked 'night|0' $split
    if ($back.Page[1] -eq 3) { Pass 'split 1-3 exported a 3-page document' }
    else { Fail 'split 1-3 page count' "reopened as $($back.Page[1]) pages" }
  } else { Fail 'split 1-3' "no file at $split" }

  # a range the reader can mistype must export nothing, not the wrong pages
  $bad = Join-Path $scratch 'pages-bad.pdf'
  [void](Run-Hooked ("split|zzz|{0}" -f $bad) (New-Case))
  if (-not (Test-Path $bad)) { Pass 'an unreadable range exports nothing' }
  else { Fail 'unreadable range' "a file appeared at $bad" }

  # --- 3. merge puts the added file after the current pages -------------------
  $add = Join-Path $work 'added.pdf'
  Copy-Item $arc $add -Force   # the file the merge adds; it is read, never written
  $r = Run-Hooked ("merge|{0}" -f $add) (New-Case)
  if ($r.Page[1] -eq $FixturePages * 2) {
    Pass "merge made $($r.Page[1]) pages (two copies of $FixturePages)"
  } else { Fail 'merge page count' "'$($r.Title)' (expected $($FixturePages * 2))" }

  # --- 4. one page deleted: the page that takes its place is the next one -----
  $delFile = Join-Path $scratch 'del.txt'
  $r = Run-Hooked ("delpage|0;;pagehash|{0}" -f $delFile) (New-Case) $delFile
  if ($r.Page[1] -eq $FixturePages - 1) { Pass 'deleting a page removed exactly one page' }
  else { Fail 'delete page count' "'$($r.Title)'" }
  if ($r.Hashes.Count -eq $FixturePages - 1 -and $r.Hashes[0] -eq $ref.Hashes[1]) {
    Pass 'delete page kept the right content in its place'
  } else { Fail 'delete page content' 'page 1 is not the original page 2' }

  # --- 5. one page moved: the order changes and the content travels with it ---
  $moveFile = Join-Path $scratch 'move.txt'
  $r = Run-Hooked ("movepage|0|3;;pagehash|{0}" -f $moveFile) (New-Case) $moveFile
  $movedOk = ($r.Hashes.Count -eq $FixturePages) -and
             ($r.Hashes[0] -eq $ref.Hashes[1]) -and
             ($r.Hashes[2] -eq $ref.Hashes[3]) -and
             ($r.Hashes[3] -eq $ref.Hashes[0]) -and
             ($r.Hashes[$FixturePages - 1] -eq $ref.Hashes[$FixturePages - 1])
  if ($movedOk) { Pass 'moving page 1 to position 4 carried its content along' }
  else { Fail 'move page' 'the page hashes are not in the expected order' }

  # --- 6. one page rotated: that page changes, the rest do not ---------------
  $rotFile = Join-Path $scratch 'rot.txt'
  $r = Run-Hooked ("rotatepage|2|1;;pagehash|{0}" -f $rotFile) (New-Case) $rotFile
  $changed = @()
  for ($i = 0; $i -lt $r.Hashes.Count; $i++) { if ($r.Hashes[$i] -ne $ref.Hashes[$i]) { $changed += $i } }
  if ($changed.Count -eq 1 -and $changed[0] -eq 2) { Pass 'rotating page 3 changed only page 3' }
  else { Fail 'rotate page' "changed pages: $($changed -join ',')" }

  # --- 6b. a rotated page keeps its shape through a rebuild ------------------
  $rotHash = Join-Path $scratch 'rothash.txt'
  $r = Run-Hooked ("rotatepage|2|1;;pagehash|{0}" -f $rotHash) (New-Case) $rotHash
  $rotatedPage3 = $r.Hashes[2]
  $addForMerge = Join-Path $work 'merge-base.pdf'
  Copy-Item $arc $addForMerge -Force
  $r = Run-Hooked ("rotatepage|2|1;;merge|{0};;pagehash|{1}" -f $addForMerge, $rotHash) (New-Case) $rotHash
  if ($r.Hashes.Count -eq $FixturePages * 2 -and $r.Hashes[2] -eq $rotatedPage3) {
    Pass 'a rotated page keeps its rotation and its shape through a merge'
  } else { Fail 'rotated page through a merge' 'page 3 rasterizes differently after the merge' }
  $r = Run-Hooked ("rotatepage|2|1;;rotatepage|2|-1;;pagehash|{0}" -f $rotHash) (New-Case) $rotHash
  if ($r.Hashes[2] -eq $ref.Hashes[2]) { Pass 'rotating a page back returns it to itself' }
  else { Fail 'rotate back' 'page 3 does not match the original after two rotations' }

  # --- 7. a signature stamp: sidecar now, the file on save -------------------
  $sigDoc = New-Case
  [void](Run-Hooked ("sig|{0}|0" -f $jpg) $sigDoc)
  $sidecar = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue |
    Where-Object { (Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue) -match 'sig=' } | Select-Object -First 1
  if ($sidecar) { Pass 'placing a signature wrote a sig line to the sidecar' }
  else { Fail 'signature sidecar' 'no sig line in any sidecar' }

  $before = (Get-Item $sigDoc).Length
  $env:MNPDF_HOOK = ("sig|{0}|0" -f $jpg)
  $p = Launch $exe $sigDoc
  $h = FindAppWindow $p.Id
  [void](Await { (Title $h) -match 'mnpdf \d+/\d+' } 15000)
  $dirtyTitle = Title $h
  if ($dirtyTitle -match "\u2022") { Pass 'the stamped document is marked unsaved' }
  else { Fail 'signature dirty flag' "'$dirtyTitle'" }
  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_SAVE, [IntPtr]0) | Out-Null
  [void](Await { (Get-Item $sigDoc).Length -ne $before } 15000)
  $env:MNPDF_HOOK = $null
  $after = (Get-Item $sigDoc).Length
  if ($after -ne $before) { Pass "saving wrote the stamp into the file ($before -> $after bytes)" }
  else { Fail 'signature save' "file unchanged at $before bytes" }
  $still = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue |
    Where-Object { (Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue) -match 'sig=' } | Select-Object -First 1
  if (-not $still) { Pass 'a saved stamp needs no sidecar line any more' }
  else { Fail 'sidecar after save' 'the sig line survived a save' }
  Quit-Proc $p

  # the stamp has to be in the page itself, not only in the sidecar: a page
  # that carries one must rasterize differently from the fixture.
  $stamped = Join-Path $scratch 'stamped.txt'
  $r = Run-Hooked ("sig|{0}|0;;pagehash|{1}" -f $jpg, $stamped) (New-Case) $stamped
  $r2Hash = $r.Hashes[0]
  if ($r.Hashes.Count -eq $FixturePages -and $r.Hashes[0] -ne $ref.Hashes[0]) {
    Pass 'the stamp really lands on the page'
  } else { Fail 'stamp on the page' 'page 1 rasterizes as it did before the stamp' }

  # Clearing has to take the object off the page, not only out of the sidecar:
  # the page must rasterize exactly as it did before the stamp went on.
  $cleared = Join-Path $scratch 'cleared.txt'
  $r = Run-Hooked ("sig|{0}|0;;clearsigs;;pagehash|{1}" -f $jpg, $cleared) (New-Case) $cleared
  if ($r.Hashes.Count -eq $FixturePages -and $r.Hashes[0] -eq $ref.Hashes[0]) {
    Pass 'clear signatures takes the stamp back off the page'
  } else { Fail 'clear signatures' 'page 1 still rasterizes as the stamped one' }

  # A file that is not a JPEG must be refused, not fed to the decoder: a PNG,
  # a GIF and a text file renamed .jpg all took the process down (0xC0000005).
  $notJpeg = Join-Path $scratch 'not-an-image.jpg'
  Set-Content -LiteralPath $notJpeg -Value 'this is not a JPEG' -Encoding ASCII
  $r = Run-Hooked ("sig|{0}|0;;pagehash|{1}" -f $notJpeg, (Join-Path $scratch 'png.txt')) (New-Case) (Join-Path $scratch 'png.txt')
  if ($r.Hashes.Count -eq $FixturePages -and $r.Hashes[0] -eq $ref.Hashes[0]) {
    Pass 'a file that is not a JPEG is refused instead of placed'
  } else { Fail 'non-JPEG signature' 'the reader did not come back with the page unchanged' }

  # A stamp recovered from the sidecar is in the page but not in the PDF, so
  # the document really is unsaved: Save has to do something.
  $recovered = New-Case
  [void](Run-Hooked ("sig|{0}|0" -f $jpg) $recovered)
  $r = Run-Hooked '' $recovered
  if ($r.Title -match "\u2022") { Pass 'a stamp recovered from the sidecar marks the document unsaved' }
  else { Fail 'recovered stamp dirty' "'$($r.Title)' - Save would do nothing" }

  # Reopen last goes back to the document this one replaced. last.txt always
  # names the document on screen (autosave rewrites it), so reading it would
  # reopen the same file and look like nothing happened: the 1-page fixture is
  # opened on top of the 13-page one, and the command has to bring 13 back.
  $env:MNPDF_HOOK = "open|" + (Join-Path $PSScriptRoot 'tests\arc-annot.pdf')
  $p = Launch $exe (New-Case)
  $h = FindAppWindow $p.Id
  [void](Await { (Title $h) -match 'mnpdf 1/1' } 15000)
  $onOne = (Title $h) -match 'mnpdf 1/1'
  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_REOPEN_LAST, [IntPtr]0) | Out-Null
  $back = Await { (Title $h) -match 'mnpdf 1/' + $FixturePages } 8000
  $env:MNPDF_HOOK = $null
  Quit-Proc $p
  if ($onOne -and $back) { Pass 'reopen last document returns to the document it replaced' }
  else { Fail 'reopen last document' "opened the 1-page fixture: $onOne, came back: $back" }

  # The sidecar is the only memory a stamp has before a save, so a quit and
  # relaunch of the same path must put it back on the page exactly where it was.
  $again = Join-Path $scratch 'again.txt'
  $keep = New-Case
  [void](Run-Hooked ("sig|{0}|0;;pagehash|{1}" -f $jpg, $again) $keep $again)
  $afterRestart = Run-Hooked ("pagehash|{0}" -f (Join-Path $scratch 'restart.txt')) $keep (Join-Path $scratch 'restart.txt')
  if ($afterRestart.Hashes.Count -eq $FixturePages -and $afterRestart.Hashes[0] -eq $r2Hash) {
    Pass 'a stamp survives a quit and comes back on relaunch'
  } else { Fail 'stamp replay' 'page 1 does not match the stamped page after a restart' }

  # --- 8. the drawers are windows of their own --------------------------------
  $p = Launch $exe (New-Case)
  $h = FindAppWindow $p.Id
  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_THUMBS, [IntPtr]0) | Out-Null
  $thumb = Wait-Class 'MNThumbs'
  if ($thumb -ne [IntPtr]::Zero) { Pass 'the thumbnails drawer opened' }
  else { Fail 'thumbnails drawer' 'no MNThumbs window' }

  if ($thumb -ne [IntPtr]::Zero) {
    # The drawer draws one box per page, page-aspect tall, so the row that holds
    # page 3 is not a fixed number of pixels. Click down the drawer until the
    # reader is on page 3 - that is the behaviour under test, and the click
    # positions are found rather than assumed.
    $cr = New-Object MNRect
    [void][MN]::GetClientRect($thumb, [ref]$cr)
    $clicked = $false
    for ($yy = 10; $yy -lt ($cr.B - $cr.T) - 10 -and -not $clicked; $yy += 15) {
      [MN]::PostMessageW($thumb, 0x0201, [IntPtr]1, (Lparam 60 $yy)) | Out-Null
      [MN]::PostMessageW($thumb, 0x0202, [IntPtr]0, (Lparam 60 $yy)) | Out-Null
      [void](Await { (Title $h) -match 'mnpdf 3/' } 300)
      $clicked = ((Title $h) -match 'mnpdf 3/')
    }
    if ($clicked) { Pass 'clicking a thumbnail jumps to that page' }
    else { Fail 'thumbnail click' "'$(Title $h)'" }

    [MN]::PostMessageW($thumb, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null     # WM_CLOSE
    if (Await { (Find-Class 'MNThumbs') -eq [IntPtr]::Zero } 5000) { Pass 'the thumbnails drawer closes' }
    else { Fail 'thumbnails drawer close' 'the window stayed' }
  }

  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_OUTLINE, [IntPtr]0) | Out-Null
  $outline = Wait-Class 'MNOutline'
  if ($outline -ne [IntPtr]::Zero) {
    [MN]::PostMessageW($outline, 0x0201, [IntPtr]1, (Lparam 40 14)) | Out-Null
    [MN]::PostMessageW($outline, 0x0202, [IntPtr]0, (Lparam 40 14)) | Out-Null
    Start-Sleep -Milliseconds 400
    if (-not $p.HasExited -and (Title $h) -match 'mnpdf \d+/\d+') {
      Pass 'clicking an outline entry leaves the reader on a real page'
    } else { Fail 'outline click' "'$(Title $h)'" }
    [MN]::PostMessageW($outline, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
  } else { Fail 'outline drawer' 'no MNOutline window' }
  Quit-Proc $p

  # --- 9. the split prompt is a real window with a real cancel ----------------
  $p = Launch $exe (New-Case)
  $h = FindAppWindow $p.Id
  [MN]::PostMessageW($h, 0x0111, [IntPtr]$CMD_SPLIT, [IntPtr]0) | Out-Null
  $range = Wait-Class 'MNRange'
  if ($range -ne [IntPtr]::Zero) {
    if ((Text-Of $range) -eq 'Split pages') { Pass 'the split prompt opened with its own title' }
    else { Fail 'split prompt title' "'$(Text-Of $range)'" }
    # the prompt lays its buttons out left to right: Choose file... then Cancel
    $first = [MN]::FindWindowExW($range, [IntPtr]::Zero, 'BUTTON', [IntPtr]::Zero)
    $cancel = if ($first -ne [IntPtr]::Zero) { [MN]::FindWindowExW($range, $first, 'BUTTON', [IntPtr]::Zero) } else { [IntPtr]::Zero }
    $btnText = if ($cancel -ne [IntPtr]::Zero) { Text-Of $cancel } else { '' }
    if ($btnText -eq 'Cancel') { Pass 'the split prompt has a Cancel button' }
    else { Fail 'split prompt cancel' "the second button reads '$btnText'" }
    [MN]::PostMessageW($range, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
    if (Await { (Find-Class 'MNRange') -eq [IntPtr]::Zero } 5000) { Pass 'the split prompt closes' }
    else { Fail 'split prompt close' 'the window stayed' }
  } else { Fail 'split prompt' 'no MNRange window' }
  Quit-Proc $p

  # --- 10. night mode is remembered, and does not outlive its own effect ------
  [void](Run-Hooked 'night|1' (New-Case))
  $pref = Get-Content $appPref -Raw -ErrorAction SilentlyContinue
  if ($pref -match '(?m)^night=1\s*$') { Pass 'night mode is remembered in app.txt' }
  else { Fail 'night pref' (($pref -replace "`n", ' | ')) }
  $env:MNPDF_HOOK = 'night|0'
  $p = Launch $exe (New-Case)
  $h = FindAppWindow $p.Id
  [void](Await { (Title $h) -match 'mnpdf \d+/\d+' } 15000)
  $env:MNPDF_HOOK = $null
  # A drawer window can still be on screen for a moment while the instance that
  # owned it finishes quitting, so let that clear before calling it a drawer that
  # came back by itself.
  [void](Await { (Find-Class 'MNThumbs') -eq [IntPtr]::Zero } 3000)
  $stale = Find-Class 'MNThumbs'
  if ($stale -ne [IntPtr]::Zero) {
    $stalePid = [MN]::GetWindowThreadProcessId($stale, [ref]$script:owner)
    Quit-Proc $p
    $live = (Get-Process mnpdf -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ','
    Fail 'drawers do not reopen by themselves' "window $stale belongs to pid $script:owner (mnpdf running: $live)"
  } else { Pass 'the drawers do not reopen by themselves' }
  Quit-Proc $p
} finally {
  Get-Process mnpdf -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
  foreach ($d in @($work, $scratch)) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
  Restore-AppPref          # the reader's real app.txt goes back on every path
}

Write-Output ''
Complete-Suite