# Phase-1 feature test via posted messages: no cursor movement, no focus or
# foreground games, works even while the session is locked. Posts drag/click
# and copy messages straight into mnpdf's queue, exercises double-click word
# select, triple-click line select and the search bar, and reads the window
# title and clipboard as the observables.
param([string]$Pdf)
if (-not $Pdf) { $Pdf = Join-Path $PSScriptRoot "build\arc.pdf" }
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PM {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, [MarshalAs(UnmanagedType.LPWStr)] string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, IntPtr title);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool SetKeyboardState(byte[] state);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int h2, bool r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
[void][PM]::SetProcessDpiAwarenessContext([IntPtr](-4))

$failures = New-Object System.Collections.Generic.List[string]

function GetClip {
  if (-not $script:clipOk) { return '' }   # clipboard known broken: never touch it (reads can hang)
  $tmp = [System.IO.Path]::GetTempFileName()
  $proc = Start-Process powershell -ArgumentList '-NoProfile','-Command',"Get-Clipboard -Raw | Out-File -Encoding unicode '$tmp'" -PassThru -WindowStyle Hidden
  if (-not $proc.WaitForExit(2000)) { try { $proc.Kill() } catch {}; return '' }   # zombie lock: bail out
  return ((Get-Content $tmp -Raw -ErrorAction SilentlyContinue) -replace "
?
$", '')
}

function Post([IntPtr]$h, [uint32]$m, [IntPtr]$w, [IntPtr]$l) { [void][PM]::PostMessageW($h, $m, $w, $l) }

# wait until a condition holds (polled) instead of sleeping a guessed length:
# a loaded machine makes every fixed sleep a coin flip
function Await([scriptblock]$Cond, [int]$TimeoutMs = 10000, [int]$StepMs = 100) {
  $elapsed = 0
  while ($elapsed -lt $TimeoutMs) {
    if (& $Cond) { return $true }
    Start-Sleep -Milliseconds $StepMs
    $elapsed += $StepMs
  }
  return $false
}
# hl= entries currently on disk for the open doc (sidecar is rewritten whole)
function HlCount { $sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $sc2) { return 0 }
  @(Get-Content $sc2.FullName | Where-Object { $_ -match '^hl=' }).Count }
function Lparam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function Title([IntPtr]$h) {
  $sb = New-Object System.Text.StringBuilder 256
  [void][PM]::GetWindowTextW($h, $sb, 256)
  $sb.ToString()
}
function CopyAndWait([IntPtr]$h, [string]$prev) {
  if (-not $clipOk) { Post $h 0x0301 ([IntPtr]0) ([IntPtr]0); Start-Sleep -Milliseconds 300; return '' }
  Post $h 0x0301 ([IntPtr]0) ([IntPtr]0)                        # WM_COPY
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 100
    $t = GetClip
    if ($t -cne $prev) { return $t }
  }
  return $prev
}

function OpenSearch([IntPtr]$h) {
  Post $h 0x0111 ([IntPtr]2) ([IntPtr]0)                        # WM_COMMAND id 2 = Edit > Find
}

# requires a fresh instance we own: the assertions assume a clean state
$existing = Get-Process mnpdf -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero }
if ($existing) { Write-Output "SKIP: an mnpdf instance is already running (state unknown)"; exit 0 }
$env:MNPDF_VERBOSE = "1"   # verbose titles for title-based assertions
Remove-Item "$env:APPDATA\mnpdf\*" -Recurse -Force -ErrorAction SilentlyContinue   # fresh state
$p = Start-Process -FilePath (Join-Path $PSScriptRoot "build\mnpdf.exe") -ArgumentList $Pdf -PassThru
Start-Sleep -Milliseconds 1500
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "no MainWindowHandle" }
[void][PM]::MoveWindow($h, 60, 60, 1100, 800, $true)
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
  if ([PM]::FindWindowW("#32768", [IntPtr]::Zero) -eq [IntPtr]::Zero) { break }
  Post $h 0x0100 ([IntPtr]0x1B) ([IntPtr]0)
  Start-Sleep -Milliseconds 100
}
Post $h 0x0111 ([IntPtr]130) ([IntPtr]0)                      # Add pin here
Await { [PM]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero) -ne [IntPtr]::Zero } 8000 | Out-Null
$pbox = [PM]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($pbox -ne [IntPtr]::Zero) { Write-Output "PASS pin editor box opened" }
else { $failures.Add("pin box missing"); Write-Output "FAIL pin box" }

foreach ($ch in 104, 105) { Post $pbox 0x0102 ([IntPtr]$ch) ([IntPtr]0); Start-Sleep -Milliseconds 80 }
Post $pbox 0x0100 ([IntPtr]0x0D) ([IntPtr]0)                  # Enter -> newline, not commit
Start-Sleep -Milliseconds 150
foreach ($ch in 121, 111) { Post $pbox 0x0102 ([IntPtr]$ch) ([IntPtr]0); Start-Sleep -Milliseconds 80 }
Start-Sleep -Milliseconds 300
Post $h 0x0201 ([IntPtr]1) (Lparam 80 80)                     # click outside: save + close
Post $h 0x0202 ([IntPtr]0) (Lparam 80 80)
Await { [PM]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero) -eq [IntPtr]::Zero } 8000 | Out-Null
$pbox2 = [PM]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($pbox2 -eq [IntPtr]::Zero) { Write-Output "PASS outside click closed the box" }
else { $failures.Add("pin box stayed open"); Write-Output "FAIL pin close" }

Await {                                                       # debounced autosave flush, polled
  $script:sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" | Sort-Object LastWriteTime | Select-Object -Last 1
  $script:sc2 -and (Get-Content $script:sc2.FullName -Raw -ErrorAction SilentlyContinue) -match 'pin=\d+,[\d.]+,[\d.]+,c\d+,hi\\nyo'
} 8000 | Out-Null
$sc2 = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" | Sort-Object LastWriteTime | Select-Object -Last 1
$raw2 = Get-Content $sc2.FullName -Raw
if ($raw2 -match 'pin=\d+,[\d.]+,[\d.]+,c\d+,hi\\nyo') {
  Write-Output "PASS multiline pin saved (escaped newline)"
} else { $failures.Add("multiline pin not saved"); Write-Output "FAIL pin save: $raw2" }

# --- hover the dot: a tooltip shows the text ---
Post $h 0x0200 ([IntPtr]0) (Lparam $px $py)                   # WM_MOUSEMOVE over the pin
Await { [PM]::FindWindowW("mnpdfPinTip", [IntPtr]::Zero) -ne [IntPtr]::Zero } 5000 | Out-Null
$tip = [PM]::FindWindowW("mnpdfPinTip", [IntPtr]::Zero)
if ($tip -ne [IntPtr]::Zero) { Write-Output "PASS hover tooltip shown" }
else { $failures.Add("hover tip missing"); Write-Output "FAIL tip" }
Post $h 0x0200 ([IntPtr]0) (Lparam 60 60)                     # move away
$tw = $tip                                                   # the popup is only ever hidden, never destroyed: one handle
if ($tw -eq [IntPtr]::Zero) {
  $failures.Add("tooltip hide: no tip window"); Write-Output "FAIL tooltip hides on mouse-away"
} else {
  $tipHidden = Await { -not [PM]::IsWindowVisible($tw) } 5000
  if ($tipHidden) { Write-Output "PASS tooltip hides on mouse-away" }
  else { $failures.Add("tooltip did not hide on mouse-away"); Write-Output "FAIL tooltip hides on mouse-away" }
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
if (-not $clipOk) { Write-Output "SKIP drag copy: clipboard locked" }
elseif ($t -match 'recommendation') { Write-Output "PASS drag select + copy: [$($t.Length)] $t" }
else { $failures.Add("drag select: clipboard was '$t'"); Write-Output "FAIL drag select: '$t'" }
# release kept the selection: highlight it through the same command the right-click menu posts
Post $h 0x0111 ([IntPtr]135) ([IntPtr]0)                       # Highlight (default color = yellow)
Await { (HlCount) -gt 0 } 8000 | Out-Null                    # debounced autosave flush, polled
$sc = Get-ChildItem "$env:APPDATA\mnpdf\doc-*.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
$hlLine = if ($sc) { (Get-Content $sc.FullName) | Where-Object { $_ -match '^hl=' } | Select-Object -First 1 }
if ($hlLine) { Write-Output "PASS highlight via menu command persisted: $hlLine" }
else { $failures.Add("menu highlight"); Write-Output "FAIL highlight via menu command: no hl= in sidecar" }

# --- 3. double click = word select ---
Start-Sleep -Milliseconds 100
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)                     # WM_LBUTTONDBLCLK on title
Start-Sleep -Milliseconds 200
$t = CopyAndWait $h (GetClip)
if (-not $clipOk) { Write-Output "SKIP word select: clipboard locked" }
elseif ($t -and $t -notmatch '\s' -and $t.Length -le 20) { Write-Output "PASS word select: '$t'" }
else { $failures.Add("word select: clipboard was '$t'"); Write-Output "FAIL word select: '$t'" }

Write-Output '[marker] before line test'
# --- 4. triple click = line select ---
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)
Start-Sleep -Milliseconds 150
Post $h 0x0201 ([IntPtr]1) (Lparam 600 210)                     # WM_LBUTTONDOWN right after
Start-Sleep -Milliseconds 200
$t = CopyAndWait $h (GetClip)
if (-not $clipOk) { Write-Output "SKIP line select: clipboard locked" }
elseif ($t -match '\s' -and $t.Length -gt 15 -and $t -match 'Strategy') { Write-Output "PASS line select: '$t'" }
else { $failures.Add("line select: clipboard was '$t'"); Write-Output "FAIL line select: '$t'" }

Write-Output '[marker] before search test'
# --- 5. search bar: open, type, check title hits, F3, close ---
OpenSearch $h                                                    # Edit > Find
Start-Sleep -Milliseconds 300
$edit = [PM]::FindWindowExW($h, [IntPtr]::Zero, "Edit", [IntPtr]::Zero)
if ($edit -eq [IntPtr]::Zero -or -not [PM]::IsWindowVisible($edit)) {
  $failures.Add("search bar did not open"); Write-Output "FAIL search bar open"
} else {
  foreach ($ch in 'r','e','c','o','m','m','e','n','d','a','t','i','o','n') {
    Post $edit 0x0102 ([IntPtr][int][char]$ch) ([IntPtr]0)      # WM_CHAR
    Start-Sleep -Milliseconds 40
  }
  Start-Sleep -Milliseconds 600
  $title = Title $h
  if ($title -match 'recommendation 1/(\d+)' -and [int]$Matches[1] -ge 1) {
    Write-Output "PASS search open+type: title='$title'"
  } else { $failures.Add("search title: '$title'"); Write-Output "FAIL search title: '$title'" }

  $lbl = [PM]::FindWindowExW($h, [IntPtr]::Zero, "Static", [IntPtr]::Zero)
  $lt0 = Title $lbl
  if ($lt0 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 1 -and [int]$Matches[2] -ge 2) {
    Write-Output "PASS counter label: '$lt0' (document-wide total)"
  } else { $failures.Add("counter label: '$lt0'"); Write-Output "FAIL counter: '$lt0'" }

  Post $edit 0x0100 ([IntPtr]0x72) ([IntPtr]0)                  # F3 -> next match
  Await { (Title $lbl) -match '^2/\d+$' } 15000 | Out-Null       # may walk pages
  $title2 = Title $h
  if ($title2 -ne $title) { Write-Output "PASS F3 walk: title='$title2'" }
  else { $failures.Add("F3 did not move: '$title2'"); Write-Output "FAIL F3: '$title2'" }
  $lt1 = Title $lbl
  if ($lt1 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 2) {
    Write-Output "PASS F3 advances the counter: '$lt1'"
  } else { $failures.Add("F3 counter: '$lt1'"); Write-Output "FAIL F3 counter: '$lt1'" }

  Post $edit 0x0100 ([IntPtr]0x0D) ([IntPtr]0)                  # Enter -> next match
  Await { (Title $lbl) -match '^3/\d+$' } 15000 | Out-Null
  $lt2 = Title $lbl
  if ($lt2 -match '^(\d+)/(\d+)$' -and [int]$Matches[1] -eq 3) {
    Write-Output "PASS Enter jumps to next match: '$lt2'"
  } else { $failures.Add("Enter did not advance: '$lt2'"); Write-Output "FAIL Enter: '$lt2'" }

  Post $edit 0x0100 ([IntPtr]0x1B) ([IntPtr]0)                  # Esc closes the bar
  Await { (Title $h) -notmatch 'recommendation' } 5000 | Out-Null
  $title3 = Title $h
  if ($title3 -notmatch 'recommendation') { Write-Output "PASS Esc close: title='$title3'" }
  else { $failures.Add("Esc left search open: '$title3'"); Write-Output "FAIL Esc: '$title3'" }
}

if ($p.HasExited) { throw "mnpdf exited unexpectedly during the test" }

# --- undo/redo keep the sidecar in sync (autosaved state matches the screen) ---
Post $h 0x0111 ([IntPtr]114) ([IntPtr]0)                        # undo
Await { (HlCount) -eq 0 } 8000 | Out-Null                       # debounced flush, polled
$afterUndo = HlCount
Post $h 0x0111 ([IntPtr]115) ([IntPtr]0)                        # redo
Await { (HlCount) -eq 1 } 8000 | Out-Null
$afterRedo = HlCount
if ($afterUndo -eq 0 -and $afterRedo -eq 1) { Write-Output "PASS undo/redo sidecar sync (undo=$afterUndo redo=$afterRedo)" }
else { $failures.Add("undo/redo sync undo=$afterUndo redo=$afterRedo"); Write-Output "FAIL undo/redo sidecar sync (undo=$afterUndo redo=$afterRedo)" }

# --- drag near the bottom margin auto-scrolls while held ---
Post $h 0x0201 ([IntPtr]1) (Lparam 400 500)
Start-Sleep -Milliseconds 60
Post $h 0x0200 ([IntPtr]1) (Lparam 450 790)                     # hold in the bottom edge zone
Await { (Title $h) -match 'mnpdf ([2-9]|1[0-3])/13' } 8000 | Out-Null   # timer keeps scrolling
$t = Title $h
Post $h 0x0202 ([IntPtr]0) (Lparam 450 790)
Start-Sleep -Milliseconds 300
if ($t -match 'mnpdf ([2-9]|1[0-3])/13') { Write-Output "PASS drag edge auto-scroll (title '$t')" }
else { $failures.Add("edge scroll: '$t'"); Write-Output "FAIL drag edge auto-scroll ('$t')" }

# --- autosave toggle: menu item flips the app pref ---
Post $h 0x0111 ([IntPtr]109) ([IntPtr]0)                        # toggle off
Await { (Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue) -match 'autosave=0' } 8000 | Out-Null
$app1 = Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw
Post $h 0x0111 ([IntPtr]109) ([IntPtr]0)                        # toggle back on
Await { (Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw -ErrorAction SilentlyContinue) -match 'autosave=1' } 8000 | Out-Null
$app2 = Get-Content "$env:APPDATA\mnpdf\app.txt" -Raw
if ($app1 -match 'autosave=0' -and $app2 -match 'autosave=1') { Write-Output "PASS autosave toggle persists" }
else { $failures.Add("autosave toggle: '$app1' / '$app2'"); Write-Output "FAIL autosave toggle ('$app1' / '$app2')" }

# --- regression: edits AFTER an off->on cycle must still autosave (timer re-armed) ---
$hlBefore = HlCount
Post $h 0x0111 ([IntPtr]117) ([IntPtr]0)                        # delete highlight under test state? no-op safe
Start-Sleep -Milliseconds 200
# make a real edit: double-click a word, then highlight it via the default-color command
Post $h 0x0203 ([IntPtr]1) (Lparam 600 210)                     # double-click selects a word
Start-Sleep -Milliseconds 300
Post $h 0x0111 ([IntPtr]135) ([IntPtr]0)                        # Highlight (default color)
Await { (HlCount) -gt $hlBefore } 8000 | Out-Null               # debounced flush, polled
$hlAfter = HlCount
if ($hlAfter -gt $hlBefore) { Write-Output "PASS autosave still flushes after off->on cycle ($hlBefore -> $hlAfter hl lines)" }
else { $failures.Add("autosave re-arm: $hlBefore -> $hlAfter"); Write-Output "FAIL autosave still flushes after off->on cycle ($hlBefore -> $hlAfter)" }

# --- quit with autosave on: no prompt, app exits ---
Post $h 0x0111 ([IntPtr]112) ([IntPtr]0)
Await { -not (Get-Process mnpdf -ErrorAction SilentlyContinue) } 8000 | Out-Null
$gone = -not (Get-Process mnpdf -ErrorAction SilentlyContinue)
if ($gone) { Write-Output "PASS quit without prompt (autosave on)" }
else { $failures.Add("quit prompt"); Write-Output "FAIL quit: app still running (prompt?)" }

if ($failures.Count) { Write-Output "RESULT: $($failures.Count) FAILURE(S)"; exit 1 }
Write-Output "RESULT: ALL PASS"
