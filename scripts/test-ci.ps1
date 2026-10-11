# The suites, on a plain Windows machine. scripts/gate-test.sh is the WSL
# wrapper: it adds a drive-letter guard, a checkout lock and worktree
# isolation, and it is what the local gate calls. This runner is what CI (and
# any Windows shell without WSL) calls, so both entry points run the same suite
# roster (scripts/suites.txt) through the same shared plumbing
# (scripts/gate-common.ps1) against the same isolated preferences and a freshly
# built binary.
$ErrorActionPreference = 'Stop'
# this file lives in scripts\, so the repo root is one level up
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\mnpdf.exe'
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
. (Join-Path $PSScriptRoot 'gate-common.ps1')

Write-Output '=== build'
Invoke-Build -RepoRoot $repo -ExePath $exe

# bound the evidence this runner keeps: drop failed-run state dirs older than a
# week (the gate bounds its own the same way)
$realTemp = $env:TEMP
Get-ChildItem $realTemp -Filter 'mnpdf-ci-*' -Directory -ErrorAction SilentlyContinue |
  Where-Object { $_.CreationTime -lt (Get-Date).AddDays(-7) } |
  Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# never kill what the reader had open before this runner started: only
# instances that appear from here on, and only ones running OUR build output
$before = Get-TestInstanceSnapshot -ExePath $exe

$suiteFailed = $null
try {
  # Announce the plan once, naming any suite MNPDF_SKIP_SUITES drops. The roster
  # parser itself cannot print (its output IS the roster), and without this a
  # skipped suite is indistinguishable in the log from one lost to a parse bug -
  # "all suites OK" must never quietly mean "all but one".
  $plan = @(Read-SuiteRoster (Join-Path $PSScriptRoot 'suites.txt'))
  if ($env:MNPDF_SKIP_SUITES) {
    $skipped = @($env:MNPDF_SKIP_SUITES.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    Write-Output ("CI: roster {0} suite(s); MNPDF_SKIP_SUITES drops: {1}" -f $plan.Count, ($skipped -join ', '))
  } else {
    Write-Output ("CI: roster {0} suite(s); none skipped" -f $plan.Count)
  }
  foreach ($s in $plan) {
    # $state is built from the real TEMP captured above: a suite rewrites
    # TEMP only inside its own child shell, never here
    $state = Join-Path $realTemp ("mnpdf-ci-" + [Guid]::NewGuid().ToString('n'))
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $state 'appdata')
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $state 'temp')
    Write-Output ("=== {0}" -f $s.Name)
    # the suite runs in a child process with the isolation in its own
    # environment, so nothing leaks into this runner or the next suite; the
    # child also restores its env in a finally (gate-common Invoke-SuiteRun)
    $restoreBg = (Get-Item Env:MNPDF_BACKGROUND -ErrorAction SilentlyContinue).Value
    $gateCommon = Join-Path $PSScriptRoot 'gate-common.ps1'
    try {
      $env:MNPDF_GATE_EXE = $exe
      $env:MNPDF_GATE_STATE = $state
      $env:MNPDF_GATE_SUITE = Join-Path $repo ($s.Name + '.ps1')
      if ($s.Foreground) { Remove-Item Env:MNPDF_BACKGROUND -ErrorAction SilentlyContinue }
      else { $env:MNPDF_BACKGROUND = '1' }
      & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command (". '{0}'; Invoke-SuiteRun" -f ($gateCommon -replace "'", "''"))
    } finally {
      foreach ($name in 'MNPDF_GATE_EXE', 'MNPDF_GATE_STATE', 'MNPDF_GATE_SUITE') {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
      }
      if ($null -ne $restoreBg) { $env:MNPDF_BACKGROUND = $restoreBg }
      else { Remove-Item Env:MNPDF_BACKGROUND -ErrorAction SilentlyContinue }
    }
    Stop-TestInstances -ExePath $exe -Before $before
    if ($LASTEXITCODE -ne 0) {
      $log = Join-Path $state 'output.log'
      if (Test-Path $log) {
        Get-Content $log | Where-Object { $_ -match '^(PASS|FAIL|SKIP|RESULT)' } |
          ForEach-Object { Write-Output "  $_" }
      }
      $suiteFailed = $s.Name
      Write-Output "CI: $($s.Name) FAILED; stopping before another suite launches."
      break
    }
    Write-Output "CI: $($s.Name) OK"
    Remove-Item -LiteralPath $state -Recurse -Force -ErrorAction SilentlyContinue
  }
} finally {
  Stop-TestInstances -ExePath $exe -Before $before
}
Write-Output ''
if ($suiteFailed) {
  Write-Output ("CI RESULT: {0} FAILED" -f $suiteFailed)
  exit 1
}
Write-Output 'CI RESULT: ALL SUITES OK'
