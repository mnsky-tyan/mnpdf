# Phase-1 feature test via posted messages: no cursor movement, no focus or
# foreground games, works even while the session is locked. Posts drag/click
# and copy messages straight into mnpdf's queue, exercises double-click word
# select, triple-click line select and the search bar, and reads the window
# title and clipboard as the observables.
# the fixture lives in tests\ because build\ is all output and can be deleted
# wholesale; every run works on a fresh copy so a mutating run cannot poison
# the next one (no more git-checkout restore ritual). The copy runs only when
# this suite is actually launched: a suite dropped via MNPDF_SKIP_SUITES is
# filtered out by the caller (scripts/gate-common.ps1) and never starts.
$Pdf = Join-Path $PSScriptRoot "build\arc.pdf"
. "$PSScriptRoot\tests\lib.ps1"   # one definition of the window-resolution rule
# the app is per-monitor aware, so this process must be too: posted clicks then
# land in the same pixels the app measures (shared P/Invoke surface in lib)
[MN]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Post([IntPtr]$h, [uint32]$m, [IntPtr]$w, [IntPtr]$l) { [void][MN]::PostMessageW($h, $m, $w, $l) }

# hl= entries currently on disk for the open doc (sidecar is rewritten whole).
# The sidecar is located by the app's own filename hash (SidecarFor), not by a
# glob: this file used to take the UNsorted first doc-*.txt here and the NEWEST
# one two screens down, so one file answered two questions two different ways.
# The hash is exact, so both reads land on the same file every time.
function HlCount {
  $sc2 = SidecarFor $Pdf
  if (-not (Test-Path -LiteralPath $sc2)) { return 0 }
  @(Get-Content -LiteralPath $sc2 | Where-Object { $_ -match '^hl=' }).Count }
function SidecarRaw {
  $sc2 = SidecarFor $Pdf
  if (-not (Test-Path -LiteralPath $sc2)) { return '' }
  return (Get-Content -LiteralPath $sc2 -Raw)
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

# quit the instance this suite owns, whatever happened above it.
function Quit-OwnedApp($Proc) {
  if (-not $Proc) { return $true }
  $w = FindAppWindow $Proc.Id
  if ($w -ne [IntPtr]::Zero) { Post $w 0x0111 ([IntPtr]$CMD_QUIT) ([IntPtr]0) }
  for ($i = 0; $i -lt 60 -and -not $Proc.HasExited; $i++) { Start-Sleep -Milliseconds 200 }
  # poll OUR process, never Get-Process mnpdf: an unrelated instance the reader
  # opened must not read as "the quit failed"
  if (-not $Proc.HasExited) {
    try { $Proc | Stop-Process -Force -ErrorAction Stop } catch { }
  }
  return $Proc.HasExited
}

# One try/finally around the whole body: tests/lib.ps1 sets the error preference
# to Stop, so any bad call terminates the script - and a suite that leaves its app
# instance running fails the NEXT suite's machine-is-free pre-flight, on CI and
# locally. The body keeps this file's flat, unindented style on purpose, so the
# diff stays readable.
try {

# requires a fresh instance we own: the assertions assume a clean state
Assert-NoRunningApp
Copy-Item (Join-Path $PSScriptRoot "tests\arc.pdf") $Pdf -Force
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
  (SidecarRaw) -match 'pin=\d+,[\d.]+,[\d.]+,c\d+,hi\\nyo'
} 30000 | Out-Null
# no sidecar file is a real failure, not a null-deref: the Await above discards
# its own result, so an unguarded $sc2.FullName threw here and died with a
# PowerShell stack trace instead of ever printing a verdict or quitting the app
$raw2 = SidecarRaw
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
# The line a fixed client y lands on depends on the fit zoom (it follows the
# screen the runner gives this suite: 165% there, 173% here) and on the tab
# strip the view now reserves above the document, so a y calibrated to one
# layout misses on every other: off text the drag selects nothing, WM_COPY
# leaves the clipboard alone, and the case then reads whatever it held before.
# Sweep the block the way the word case below does and take the first drag
# whose copy carries the word, so the case tests the capability at any zoom.
# The steps are 12 px because the text lines sit 19-21 px apart at any zoom
# this suite can get, and every candidate stays below the strip (60 device px
# at 192 dpi) and clear of the bottom auto-scroll zone (clientH - 56).
$t = ''
for ($cand = 484; $cand -le 616; $cand += 12) {
  Post $h 0x201 ([IntPtr]1) (Lparam 200 $cand)                  # press left of the line's text
  foreach ($x in 240, 300, 380, 470, 570, 680, 790, 850) {
    Start-Sleep -Milliseconds 30
    Post $h 0x200 ([IntPtr]1) (Lparam $x $cand)
  }
  # copy MID-DRAG (selection survives release now; highlight happens via the menu)
  $t = CopyAndWait $h (GetClip)
  Post $h 0x202 ([IntPtr]0) (Lparam 850 $cand)
  Start-Sleep -Milliseconds 600
  if (-not $script:clipOk) { break }
  if ($t -match 'recommendation') { break }
}
if (-not $script:clipOk) { Degraded 'drag copy' 'clipboard locked' }
elseif (-not (Test-ClipboardRoundTrip)) { Degraded 'drag copy' 'clipboard not writable on this box' }
elseif ($t -match 'recommendation') { Pass "drag select + copy: [$($t.Length)] $t" }
else { Fail "drag select" "clipboard was '$t'" }
# release kept the selection: highlight it through the same command the right-click menu posts
Post $h 0x0111 ([IntPtr]$CMD_HIGHLIGHT) ([IntPtr]0)            # Highlight (default color = yellow)
Await { (HlCount) -gt 0 } 30000 | Out-Null                   # debounced flush, polled (8s was a coin flip under load)
# NOTE the parentheses around (SidecarRaw): a bare `SidecarRaw -split "`n"`
# is parsed as a COMMAND with a -split argument, not as splitting the function's
# output - it silently returned the wrong thing here and the case read as a
# failure with a correct sidecar on disk.
$hlLine = if ((HlCount) -gt 0) { ((SidecarRaw) -split "`n" | Where-Object { $_ -match '^hl=' } | Select-Object -First 1) } else { '' }
if ($hlLine) { Pass "highlight via menu command persisted: $hlLine" }
else { Fail "highlight via menu command" "no hl= in sidecar" }

# --- 3. double click = word select ---
# The pixel a word sits under depends on the fit zoom, which follows the screen
# the runner gives this suite (165% there, 173% here) - and a click past a
# line's last glyph lands on the line break, where the word expansion spills
# into the next line. Sweep the text block itself, starting well inside it, and
# take the first double-click that selects a single word, so the case tests the
# capability at any zoom instead of one resolution's layout.
$wordX = 0; $wordAt = 0; $word = $null
foreach ($y in 300, 340, 260, 380, 220, 420, 180, 460) {
  if ($word) { break }
  foreach ($x in 420, 340, 500) {
    Post $h 0x0203 ([IntPtr]1) (Lparam $x $y)                  # WM_LBUTTONDBLCLK
    Start-Sleep -Milliseconds 200
    $t = CopyAndWait $h (GetClip)
    if (-not $script:clipOk) { break }
    if ($t -and $t -notmatch '\s' -and $t.Length -le 20) { $wordX = $x; $wordAt = $y; $word = $t; break }
  }
}
if (-not $script:clipOk) { Degraded 'word select' 'clipboard locked' }
elseif (-not (Test-ClipboardRoundTrip)) { Degraded 'word select' 'clipboard not writable on this box' }
elseif ($word) { Pass "word select: '$word'" }
else { Fail "word select" "no single word in the sweep (last '$t')" }

Write-Output '[marker] before line test'
# --- 4. triple click = line select ---
# the same spot that held a word holds its line, at whatever zoom found it
$lx = if ($wordX) { $wordX } else { 420 }
$ly = if ($wordAt) { $wordAt } else { 210 }
Post $h 0x0203 ([IntPtr]1) (Lparam $lx $ly)
Start-Sleep -Milliseconds 150
Post $h 0x0201 ([IntPtr]1) (Lparam $lx $ly)                     # WM_LBUTTONDOWN right after
Start-Sleep -Milliseconds 200
$t = CopyAndWait $h (GetClip)
# the line at the word's spot must at least contain the word; it may be a
# short heading, so a prose length would assume one document's layout again
$w = if ($word) { $word.Trim() } else { '' }
if (-not $script:clipOk) { Degraded 'line select' 'clipboard locked' }
elseif (-not (Test-ClipboardRoundTrip)) { Degraded 'line select' 'clipboard not writable on this box' }
elseif ($w -and $t -and $t.Contains($w) -and $t.Length -ge $w.Length) { Pass "line select: '$t'" }
else { Fail "line select" "clipboard was '$t' (word was '$w')" }

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
  # the label is a control whose text the app rewrites at runtime, so it is read
  # with WM_GETTEXT (Control-Text). Title is GetWindowTextW, which the harness
  # documents as blind on a control: it answers with the creation text, so these
  # three assertions could only ever pass on a label that happened to be born
  # with the right text in it
  $lt0 = Control-Text $lbl
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
  $lt1 = Control-Text $lbl
  if ($lt1 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 2) {
    Pass "F3 advances the counter: '$lt1'"
  } else { Fail "F3 counter" "'$lt1'" }

  Post $edit 0x0100 ([IntPtr]0x0D) ([IntPtr]0)                  # Enter -> next match
  Await { (Title $h) -match 'recommendation 3/\d+' } 45000 | Out-Null   # same label-then-title order
  $lt2 = Control-Text $lbl
  if ($lt2 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 3) {
    Pass "Enter jumps to next match: '$lt2'"
  } else { Fail "Enter" "did not advance: '$lt2'" }

  Post $edit 0x0100 ([IntPtr]0x1B) ([IntPtr]0)                  # Esc closes the bar
  Await { (Title $h) -notmatch 'recommendation' } 5000 | Out-Null
  $title3 = Title $h
  if ($title3 -notmatch 'recommendation') { Pass "Esc close: title='$title3'" }
  else { Fail "Esc" "left search open: '$title3'" }
}

if ($p.HasExited) {
  # a terminating error here once left the app instance running and printed no
  # verdict at all; now it is an ordinary failing case with a quit on the way out
  Fail "app stayed up" "mnpdf exited unexpectedly during the test"
} else {

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
}

# --- quit with autosave on: no prompt, app exits ---
$quitOk = Quit-OwnedApp $p
if ($quitOk) { Pass "quit without prompt (autosave on)" }
else { Fail "quit" "app still running (prompt?)" }
} finally {
  # every path out of the body, exception included, ends this suite's instance
  Quit-OwnedApp $p | Out-Null
}

Complete-Suite
