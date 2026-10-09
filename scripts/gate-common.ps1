# Shared plumbing for the two suite runners: scripts/test-ci.ps1 on plain
# Windows (and CI) and scripts/gate-test.sh under WSL. Everything that used to
# be written twice lives here exactly once - the build recipe, the instance
# policy, the per-suite environment isolation, and the pass/fail verdict.
$ErrorActionPreference = 'Stop'

# --- the roster -------------------------------------------------------------
# scripts/suites.txt: one suite per line, '#' opens a comment, and a trailing
# '*' marks a foreground suite (the app starts without MNPDF_BACKGROUND so its
# popup can take the keyboard).
function Read-SuiteRoster {
  param([string]$RosterPath)
  # MNPDF_SKIP_SUITES (comma-separated names) drops suites from the roster for
  # this run: a suite can be environmentally broken on one machine (the
  # clipboard suites fail when some other program holds the clipboard) while
  # staying green on CI, which never sets the variable.
  $skip = @()
  if ($env:MNPDF_SKIP_SUITES) { $skip = $env:MNPDF_SKIP_SUITES.Split(',') | ForEach-Object { $_.Trim() } }
  foreach ($raw in (Get-Content -LiteralPath $RosterPath)) {
    $line = ("$raw" -replace '#.*$', '').Trim()
    if (-not $line) { continue }
    $fg = $line.EndsWith('*')
    $name = $line.TrimEnd('*').Trim()
    if ($skip -contains $name) { continue }
    [pscustomobject]@{ Name = $name; Foreground = $fg }
  }
}

# --- the build --------------------------------------------------------------
# One recipe for both runners. The caller bounds it in time.
function Invoke-Build {
  param([string]$RepoRoot, [string]$ExePath)
  $null = New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot 'build')
  # the .bat is addressed absolutely: cmd resolves a bare script name against
  # the PATH, not the current directory
  & cmd.exe /c ('"' + (Join-Path $RepoRoot 'build.bat') + '"')
  if ($LASTEXITCODE -ne 0) { throw "build.bat exited $LASTEXITCODE" }
  if (-not (Test-Path -LiteralPath $ExePath)) { throw 'build\mnpdf.exe is missing' }
}

# --- the instance policy ----------------------------------------------------
# A snapshot of the mnpdf.exe processes running from THIS build dir right now.
function Get-TestInstanceSnapshot {
  param([string]$ExePath)
  $dir = (Split-Path -Parent $ExePath)
  $ids = @()
  Get-CimInstance Win32_Process -Filter "Name = 'mnpdf.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    $p = $null
    try { $p = $_.ExecutablePath } catch { }
    if ($p -and ((Split-Path -Parent $p) -ieq $dir)) { $ids += [int]$_.ProcessId }
  }
  return $ids
}

# End the instances a run leaked: everything running from this build dir that
# was NOT in the snapshot taken before the suites started. A copy the reader had
# open beforehand is in the snapshot and is never touched - whatever folder it
# runs from; only this run's own build dir is ever matched at all.
function Stop-TestInstances {
  param([string]$ExePath, [int[]]$Before)
  $known = @(); if ($Before) { $known = @($Before) }
  $dir = (Split-Path -Parent $ExePath)
  $leaked = @(Get-CimInstance Win32_Process -Filter "Name = 'mnpdf.exe'" -ErrorAction SilentlyContinue | Where-Object {
    $p = $null
    try { $p = $_.ExecutablePath } catch { }
    $p -and ((Split-Path -Parent $p) -ieq $dir) -and ($known -notcontains [int]$_.ProcessId)
  })
  foreach ($proc in $leaked) { Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue }
  if ($leaked) { Start-Sleep -Milliseconds 400 }
}

# The gate's pre-flight: refuse to run when an mnpdf instance is already up.
# It must not close an existing app to make a test runnable, and the suites
# would only SKIP. Exit 1 = an instance is running (the caller prints why).
function Assert-NoAppRunning {
  if (@(Get-Process mnpdf -ErrorAction SilentlyContinue).Count) { exit 1 }
  exit 0
}

# --- the per-suite run ------------------------------------------------------
# THE pass/fail rule, in one place. The suite's own RESULT line is authoritative,
# but a FAIL or SKIP line, a nonzero exit, or a missing PASS line all fail too -
# so a suite that crashes before its epilogue cannot report a pass.
function Get-SuiteVerdict {
  param([string]$LogPath, [int]$ExitCode)
  $output = @()
  if ($LogPath -and (Test-Path -LiteralPath $LogPath)) {
    $output = @(Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue)
  }
  $bad = @($output | Where-Object { $_ -match '^(FAIL|SKIP:|RESULT: [0-9]+ FAILURE)' })
  $passed = @($output | Where-Object { $_ -match '^PASS ' })
  return ($ExitCode -eq 0 -and $bad.Count -eq 0 -and $passed.Count -gt 0)
}

# Run ONE suite in this process against the environment the caller exported:
#   MNPDF_GATE_SUITE  the suite script to run
#   MNPDF_GATE_EXE    the app binary the suites drive
#   MNPDF_GATE_STATE  the suite's private state dir (APPDATA/TEMP live under it)
#   MNPDF_BACKGROUND  present = the app starts hidden; ABSENT = foreground
#                     (the app only checks that the variable exists, so a
#                     foreground suite needs it removed, not set to 0)
# Writes the suite's output to <state>\output.log, restores the environment in
# a finally, prints the single verdict line the runners match, and exits 0/1.
function Invoke-SuiteRun {
  $suitePath = $env:MNPDF_GATE_SUITE
  $exePath = $env:MNPDF_GATE_EXE
  $state = $env:MNPDF_GATE_STATE
  if (-not $suitePath -or -not $exePath -or -not $state) {
    throw 'MNPDF_GATE_SUITE / MNPDF_GATE_EXE / MNPDF_GATE_STATE must all be set'
  }
  $foreground = -not $env:MNPDF_BACKGROUND
  $appData = Join-Path $state 'appdata'
  $temp = Join-Path $state 'temp'
  $log = Join-Path $state 'output.log'
  $null = New-Item -ItemType Directory -Force -Path $appData
  $null = New-Item -ItemType Directory -Force -Path $temp
  # the record of THIS shell, for the gate's cleanup: it ends the test shell and
  # its app children by identity and exact path; CI just deletes it with the dir.
  $self = Get-Process -Id $PID
  @{ Id = $PID; Started = $self.StartTime.ToUniversalTime().Ticks } |
    ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $state 'runner.json')
  $restore = @{}
  foreach ($name in 'APPDATA', 'TEMP', 'TMP', 'MNPDF_GATE_EXE', 'MNPDF_GATE_STATE', 'MNPDF_GATE_SUITE', 'MNPDF_BACKGROUND') {
    $restore[$name] = (Get-Item "Env:$name" -ErrorAction SilentlyContinue).Value
  }
  $ok = $false
  try {
    $env:APPDATA = $appData
    $env:TEMP = $temp
    $env:TMP = $temp
    $env:MNPDF_GATE_EXE = $exePath
    $env:MNPDF_GATE_STATE = $state
    $env:MNPDF_GATE_SUITE = $suitePath
    if ($foreground) { Remove-Item Env:MNPDF_BACKGROUND -ErrorAction SilentlyContinue }
    else { $env:MNPDF_BACKGROUND = '1' }
    # LOCKSTEP: scripts/test-ci.ps1 makes the same set/remove decision for the CI
    # runner from the same roster marker, and scripts/gate-test.sh mirrors both
    # for the WSL path - the app checks existence only, so the rule is one
    # sentence and must stay identical everywhere it is applied.
    # The suite is invoked directly rather than through Start-Process -Wait:
    # that also waits for the child's inherited output handle, and the app the
    # suite launches holds it open long after the suite itself has finished.
    # Tee still streams to the console as the suite runs; the log itself is
    # written afterwards in UTF-8, because Tee-Object's own file in this
    # PowerShell is UTF-16 and the gate's grep could not match a verdict in it.
    $global:LASTEXITCODE = 0
    & $suitePath *>&1 | Tee-Object -Variable suiteOutput
    Set-Content -LiteralPath $log -Value $suiteOutput -Encoding UTF8
    $code = $global:LASTEXITCODE
    $ok = Get-SuiteVerdict -LogPath $log -ExitCode $code
  } catch {
    $ok = $false
  } finally {
    foreach ($name in $restore.Keys) {
      if ($null -ne $restore[$name]) { Set-Item -Path "Env:$name" -Value $restore[$name] }
      else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    }
  }
  if ($ok) { $verdict = 'SUITE RESULT: OK' } else { $verdict = 'SUITE RESULT: FAILED' }
  # the verdict line goes to the console AND the log, so a runner can match it
  # in either place
  Add-Content -LiteralPath $log -Value $verdict -Encoding UTF8
  Write-Output $verdict
  if (-not $ok) { exit 1 }
  exit 0
}

# End a timed-out or crashed suite's process tree: the test shell recorded in
# <state>\runner.json, then any app it launched. An app counts as launched here
# only through one of the two parents the gate alone can produce: the recorded
# shell itself, which still parents a Start-Process launch after the shell exits,
# and the WMI provider host the windowed-gate spawn goes through. A copy the
# reader opens runs from Explorer or a terminal and matches neither, so a record
# an interrupted run left behind decides nothing about it.
function Stop-RunnerTree {
  $record = Join-Path $env:MNPDF_GATE_STATE 'runner.json'
  if (-not (Test-Path -LiteralPath $record)) { exit 0 }
  $owned = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
  $runner = Get-Process -Id $owned.Id -ErrorAction SilentlyContinue
  if ($runner -and $runner.ProcessName -eq 'powershell' -and
      $runner.StartTime.ToUniversalTime().Ticks -eq $owned.Started) {
    try {
      $runner.Kill()
      if (-not $runner.WaitForExit(5000)) { throw 'Test shell did not exit' }
    } catch { if (-not $runner.HasExited) { throw } }
  }
  $gateParents = @([int]$owned.Id)
  $gateParents += @(Get-CimInstance Win32_Process -Filter "Name = 'WmiPrvSE.exe'" |
    ForEach-Object { [int]$_.ProcessId })
  $children = Get-CimInstance Win32_Process -Filter "Name = 'mnpdf.exe'" |
    Where-Object { $_.ExecutablePath -eq $env:MNPDF_GATE_EXE -and
      ($gateParents -contains [int]$_.ParentProcessId) }
  foreach ($child in $children) {
    $app = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue
    if (-not $app -or $app.Path -ne $env:MNPDF_GATE_EXE) { continue }
    try {
      [void]$app.CloseMainWindow()
      if (-not $app.WaitForExit(3000)) {
        $app.Kill()
        if (-not $app.WaitForExit(3000)) { throw 'Test app did not exit' }
      }
    } catch { if (-not $app.HasExited) { throw } }
  }
  exit 0
}
