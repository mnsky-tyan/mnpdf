# Phase-1 feature test via posted messages: no cursor movement, no focus or
# foreground games, works even while the session is locked. Posts drag/click
# and copy messages straight into mnpdf's queue, exercises double-click word
# select, triple-click line select and the search bar, and reads the window
# title and clipboard as the observables.
param([string]$Pdf)
# the fixture lives in tests\ because build\ is all output and can be deleted
# wholesale; every run works on a fresh copy so a mutating run cannot poison
# the next one (no more git-checkout restore ritual). Copied after the SKIP
# guard below, so a skipped run leaves the checkout untouched.
$useDefaultPdf = -not $Pdf
if ($useDefaultPdf) { $Pdf = Join-Path $PSScriptRoot "build\arc.pdf" }
. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
# the app is per-monitor aware, so this process must be too: posted clicks then
# land in the same pixels the app measures (shared P/Invoke surface in lib)
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Post([IntPtr]$h, [uint32]$m, [IntPtr]$w, [IntPtr]$l) { [void][MN]::PostMessageW($h, $m, $w, $l) }

# hl= entries currently on disk for the open doc (sidecar is rewritten whole)
function HlCount { $sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $sc2) { return 0 }
  @(Get-Content $sc2.FullName | Where-Object { $_ -match '^hl=' }).Count }
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function Title([IntPtr]$h) {
  $sb = New-Object System.Text.StringBuilder 256
  [void][MN]::GetWindowTextW($h, $sb, 256)
  $sb.ToString()
}
function CopyAndWait([IntPtr]$h, [string]$prev) {
  if (-not $script:clipOk) { Post $h 0x0301 ([IntPtr]0) ([IntPtr]0); Start-Sleep -Milliseconds 300; return '' }
  Post $h 0x0301 ([IntPtr]0) ([IntPtr]0)                        # WM_COPY
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 100
    $t = GetClip
    if ($t -cne $prev) { return $t }
  }
  return $prev
}

function OpenSearch([IntPtr]$h) {
  Post $h 0x0111 ([IntPtr]$CMD_MENU_FIND) ([IntPtr]0)           # WM_COMMAND Edit > Find
}

# requires a fresh instance we own: the assertions assume a clean state
Assert-NoRunningApp
if ($useDefaultPdf) { Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") $Pdf -Force }
$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
# shared Launch (tests/lib.ps1) resolves the real window and restores it without
# activating: these tests post clicks at client coords, so they need a visible
# window, and a window the user already has open must never come to the front
$p = Launch (Resolve-AppExe) $Pdf
$h = FindAppWindow $p.Id
[void][MN]::MoveWindow($h, 60, 60, 1100, 800, $true)
Start-Sleep -Milliseconds 400

# --- 1. pin editor: floating box beside the pin, Enter = newline, click outside commits ---
# pin the spot first so the suite knows exactly where the dot is
$px = 500; $py = 400
Post $h 0x0204 ([IntPtr]0) (Lparam $px $py)                    # WM_RBUTTONDOWN
Post $h 0x0205 ([IntPtr]0) (Lparam $px $py)                    # WM_RBUTTONUP -> context menu
Start-Sleep -Milliseconds 500
# a command posted while the menu is modal is swallowed: keep pressing Esc
# until the popup window is really gone, then the command can be delivered
for ($i = 0; $i -lt 20; $i++) {
  if ([MN]::FindWindowW("#32768", [IntPtr]::Zero) -eq [IntPtr]::Zero) { break }
  Post $h 0x0100 ([IntPtr]0x1B) ([IntPtr]0)
  Start-Sleep -Milliseconds 100
}
Post $h 0x0111 ([IntPtr]$CMD_ADD_PIN) ([IntPtr]0)              # Add pin here
Await { [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero) -ne [IntPtr]::Zero } 30000 | Out-Null
$pbox = [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($pbox -ne [IntPtr]::Zero) { Pass "pin editor box opened" }
else { Fail "pin box" "missing" }

foreach ($ch in 104, 105) { Post $pbox 0x0102 ([IntPtr]$ch) ([IntPtr]0); Start-Sleep -Milliseconds 80 }
Post $pbox 0x0100 ([IntPtr]0x0D) ([IntPtr]0)                  # Enter -> newline, not commit
Start-Sleep -Milliseconds 150
foreach ($ch in 121, 111) { Post $pbox 0x0102 ([IntPtr]$ch) ([IntPtr]0); Start-Sleep -Milliseconds 80 }
Start-Sleep -Milliseconds 300
Post $h 0x0201 ([IntPtr]1) (Lparam 80 80)                     # click outside: save + close
Post $h 0x0202 ([IntPtr]0) (Lparam 80 80)
# a posted click does not move activation, so the app's commit-on-deactivation
# never fires on its own and this case used to ride on ambient foreground
# churn from whatever window was active at the time. The box commits on focus
# loss past its 400 ms opening grace (src/main.cpp pinBoxProc), and a posted
# WM_KILLFOCUS is the one message that reaches that path without a real mouse.
Start-Sleep -Milliseconds 500
Post $pbox 0x0008 ([IntPtr]0) ([IntPtr]0)                      # WM_KILLFOCUS: save + close
Await { [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero) -eq [IntPtr]::Zero } 8000 | Out-Null
$pbox2 = [MN]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($pbox2 -eq [IntPtr]::Zero) { Pass "outside click closed the box" }
else { Fail "pin close" "box stayed open" }

Await {                                                       # debounced autosave flush, polled
  $script:sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
  $script:sc2 -and (Get-Content $script:sc2.FullName -Raw -ErrorAction SilentlyContinue) -match 'pin=\d+,[\d.]+,[\d.]+,c\d+,hi\\nyo'
} 30000 | Out-Null
$sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
$raw2 = Get-Content $sc2.FullName -Raw
if ($raw2 -match 'pin=\d+,[\d.]+,[\d.]+,c\d+,hi\\nyo') {
  Pass "multiline pin saved (escaped newline)"
} else { Fail "pin save" "multiline pin not saved: $raw2" }

# --- hover the dot: a tooltip shows the text ---
Post $h 0x0200 ([IntPtr]0) (Lparam $px $py)                   # WM_MOUSEMOVE over the pin
Await { [MN]::FindWindowW("mnpdfPinTip", [IntPtr]::Zero) -ne [IntPtr]::Zero } 5000 | Out-Null
$tip = [MN]::FindWindowW("mnpdfPinTip", [IntPtr]::Zero)
if ($tip -ne [IntPtr]::Zero) { Pass "hover tooltip shown" }
else { Fail "tip" "hover tip missing" }
Post $h 0x0200 ([IntPtr]0) (Lparam 60 60)                     # move away
$tw = $tip                                                   # the popup is only ever hidden, never destroyed: one handle
if ($tw -eq [IntPtr]::Zero) {
  Fail "tooltip hides on mouse-away" "no tip window"
} else {
  $tipHidden = Await { -not [MN]::IsWindowVisible($tw) } 5000
  if ($tipHidden) { Pass "tooltip hides on mouse-away" }
  else { Fail "tooltip hides on mouse-away" "still visible after mouse-away" }
}


# --- 2. drag select + WM_COPY ---
$y = 520
Post $h 0x201 ([IntPtr]1) (Lparam 200 $y)
foreach ($x in 240, 300, 380, 470, 570, 680, 790, 850) {
  Start-Sleep -Milliseconds 30
  Post $h 0x200 ([IntPtr]1) (Lparam $x $y)
}
# copy MID-DRAG (selection survives release now; highlight happens via the menu)
$prevClip = GetClip
$t = CopyAndWait $h $prevClip
Post $h 0x202 ([IntPtr]0) (Lparam 850 $y)
Start-Sleep -Milliseconds 600
if (-not $script:clipOk) { Write-Output "SKIP drag copy: clipboard locked" }
elseif ($t -match 'recommendation') { Pass "drag select + copy: [$($t.Length)] $t" }
else { Fail "drag select" "clipboard was '$t'" }
# release kept the selection: highlight it through the same command the right-click menu posts
Post $h 0x0111 ([IntPtr]$CMD_HIGHLIGHT) ([IntPtr]0)            # Highlight (default color = yellow)
Await { (HlCount) -gt 0 } 30000 | Out-Null                   # debounced flush, polled (8s was a coin flip under load)
$sc = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
$hlLine = if ($sc) { (Get-Content $sc.FullName) | Where-Object { $_ -match '^hl=' } | Select-Object -First 1 }
if ($hlLine) { Pass "highlight via menu command persisted: $hlLine" }
else { Fail "highlight via menu command" "no hl= in sidecar" }

# --- 3. double click = word select ---
Start-Sleep -Milliseconds 100
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)                     # WM_LBUTTONDBLCLK on title
Start-Sleep -Milliseconds 200
$t = CopyAndWait $h (GetClip)
if (-not $script:clipOk) { Write-Output "SKIP word select: clipboard locked" }
elseif ($t -and $t -notmatch '\s' -and $t.Length -le 20) { Pass "word select: '$t'" }
else { Fail "word select" "clipboard was '$t'" }

Write-Output '[marker] before line test'
# --- 4. triple click = line select ---
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)
Start-Sleep -Milliseconds 150
Post $h 0x0201 ([IntPtr]1) (Lparam 600 210)                     # WM_LBUTTONDOWN right after
Start-Sleep -Milliseconds 200
$t = CopyAndWait $h (GetClip)
if (-not $script:clipOk) { Write-Output "SKIP line select: clipboard locked" }
elseif ($t -match '\s' -and $t.Length -gt 15 -and $t -match 'Strategy') { Pass "line select: '$t'" }
else { Fail "line select" "clipboard was '$t'" }

Write-Output '[marker] before search test'
# --- 5. search bar: open, type, check title hits, F3, close ---
OpenSearch $h                                                    # Edit > Find
Start-Sleep -Milliseconds 300
$edit = [MN]::FindWindowExW($h, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($edit -eq [IntPtr]::Zero -or -not [MN]::IsWindowVisible($edit)) {
  Fail "search bar open" "did not open"
} else {
  foreach ($ch in 'r','e','c','o','m','m','e','n','d','a','t','i','o','n') {
    Post $edit 0x0102 ([IntPtr][int][char]$ch) ([IntPtr]0)      # WM_CHAR
    Start-Sleep -Milliseconds 40
  }
  Start-Sleep -Milliseconds 600
  $title = Title $h
  if ($title -match 'recommendation 1/(\d+)' -and [int]$Matches[1] -ge 1) {
    Pass "search open+type: title='$title'"
  } else { Fail "search title" "'$title'" }

  $lbl = [MN]::FindWindowExW($h, [IntPtr]::Zero, "Static", [IntPtr]::Zero)
  $lt0 = Title $lbl
  if ($lt0 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 1 -and [int]$Matches[2] -ge 2) {
    Pass "counter label: '$lt0' (document-wide total)"
  } else { Fail "counter label" "'$lt0'" }

  Post $edit 0x0100 ([IntPtr]0x72) ([IntPtr]0)                  # F3 -> next match
  # nextMatch() publishes the counter label first and the window title after it,
  # so waiting on the label alone reads the intermediate state: the title still
  # shows the old match number for a moment. Wait on what is actually asserted.
  Await { (Title $h) -match 'recommendation 2/\d+' } 45000 | Out-Null
  $title2 = Title $h
  if ($title2 -ne $title) { Pass "F3 walk: title='$title2'" }
  else { Fail "F3" "did not move: '$title2'" }
  $lt1 = Title $lbl
  if ($lt1 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 2) {
    Pass "F3 advances the counter: '$lt1'"
  } else { Fail "F3 counter" "'$lt1'" }

  Post $edit 0x0100 ([IntPtr]0x0D) ([IntPtr]0)                  # Enter -> next match
  Await { (Title $h) -match 'recommendation 3/\d+' } 45000 | Out-Null   # same label-then-title order
  $lt2 = Title $lbl
  if ($lt2 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 3) {
    Pass "Enter jumps to next match: '$lt2'"
  } else { Fail "Enter" "did not advance: '$lt2'" }

  Post $edit 0x0100 ([IntPtr]0x1B) ([IntPtr]0)                  # Esc closes the bar
  Await { (Title $h) -notmatch 'recommendation' } 5000 | Out-Null
  $title3 = Title $h
  if ($title3 -notmatch 'recommendation') { Pass "Esc close: title='$title3'" }
  else { Fail "Esc" "left search open: '$title3'" }
}

if ($p.HasExited) { throw "mnpdf exited unexpectedly during the test" }

# --- undo/redo keep the sidecar in sync (autosaved state matches the screen) ---
Post $h 0x0111 ([IntPtr]$CMD_UNDO) ([IntPtr]0)                  # undo
Await { (HlCount) -eq 0 } 30000 | Out-Null                      # debounced flush, polled
$afterUndo = HlCount
Post $h 0x0111 ([IntPtr]$CMD_REDO) ([IntPtr]0)                  # redo
Await { (HlCount) -eq 1 } 30000 | Out-Null
$afterRedo = HlCount
if ($afterUndo -eq 0 -and $afterRedo -eq 1) { Pass "undo/redo sidecar sync (undo=$afterUndo redo=$afterRedo)" }
else { Fail "undo/redo sidecar sync" "undo=$afterUndo redo=$afterRedo" }

# --- drag near the bottom margin auto-scrolls while held ---
Post $h 0x0201 ([IntPtr]1) (Lparam 400 500)
Start-Sleep -Milliseconds 60
Post $h 0x0200 ([IntPtr]1) (Lparam 450 790)                     # hold in the bottom edge zone
Await { (Title $h) -match "mnpdf ([2-9]|1[0-3])/$FixturePages" } 30000 | Out-Null   # timer keeps scrolling
$t = Title $h
Post $h 0x0202 ([IntPtr]0) (Lparam 450 790)
Start-Sleep -Milliseconds 300
if ($t -match "mnpdf ([2-9]|1[0-3])/$FixturePages") { Pass "drag edge auto-scroll (title '$t')" }
else { Fail "drag edge auto-scroll" "'$t'" }

# --- autosave toggle: menu item flips the app pref ---
Post $h 0x0111 ([IntPtr]$CMD_AUTOSAVE) ([IntPtr]0)              # toggle off
Await { (Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue) -match 'autosave=0' } 30000 | Out-Null
$app1 = Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue
Post $h 0x0111 ([IntPtr]$CMD_AUTOSAVE) ([IntPtr]0)              # toggle back on
Await { (Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue) -match 'autosave=1' } 30000 | Out-Null
$app2 = Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue
if ($app1 -match 'autosave=0' -and $app2 -match 'autosave=1') { Pass "autosave toggle persists" }
else { Fail "autosave toggle" "'$app1' / '$app2'" }

# --- regression: edits AFTER an off->on cycle must still autosave (timer re-armed) ---
$hlBefore = HlCount
Post $h 0x0111 ([IntPtr]$CMD_DELETE_HL) ([IntPtr]0)             # delete highlight under test state? no-op safe
Start-Sleep -Milliseconds 200
# make a real edit: double-click a word, then highlight it via the default-color command
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)                     # double-click selects a word
Start-Sleep -Milliseconds 300
Post $h 0x0111 ([IntPtr]$CMD_HIGHLIGHT) ([IntPtr]0)             # Highlight (default color)
Await { (HlCount) -gt $hlBefore } 30000 | Out-Null               # debounced flush, polled
$hlAfter = HlCount
if ($hlAfter -gt $hlBefore) { Pass "autosave still flushes after off->on cycle ($hlBefore -> $hlAfter hl lines)" }
else { Fail "autosave still flushes after off->on cycle" "$hlBefore -> $hlAfter" }

# --- quit with autosave on: no prompt, app exits ---
Post $h 0x0111 ([IntPtr]$CMD_QUIT) ([IntPtr]0)
Await { -not (Get-Process mnpdf -ErrorAction SilentlyContinue) } 8000 | Out-Null
$gone = -not (Get-Process mnpdf -ErrorAction SilentlyContinue)
if ($gone) { Pass "quit without prompt (autosave on)" }
else { Fail "quit" "app still running (prompt?)" }

Complete-Suite
