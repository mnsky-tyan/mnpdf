# The suites, on a plain Windows machine. scripts/gate-test.sh is the WSL
# wrapper: it adds a drive-letter guard, a checkout lock and worktree
# isolation, and it is what the local gate calls. This runner is what CI (and
# any Windows shell without WSL) calls, so both entry points run the same seven
# suites against the same isolated preferences and a freshly built binary.
#
# Every suite gets its own APPDATA and TEMP, so a suite that writes preferences
# or a cache cannot hand state to the next one - and a leftover window from an
# earlier suite cannot make a later one fail on a false positive.
$ErrorActionPreference = 'Stop'
# this file lives in scripts\, so the repo root is one level up
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\mnpdf.exe'
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

# the colour suite types into a window of the app: a minimised owner can never
# give its colour popup the keyboard, so that one runs in the foreground
$suites = @('select-msg-test', 'test-features', 'test-suite2', 'test-continuous', 'test-release', 'test-captionless', 'test-colors')
$foreground = @('test-colors')

Write-Output '=== build'
Push-Location $repo
try {
  if (-not (Test-Path -LiteralPath 'build')) { New-Item -ItemType Directory -Path build | Out-Null }
  # the .bat is addressed absolutely: cmd resolves a bare script name against
  # the PATH, not the current directory
  & cmd.exe /c ('"' + (Join-Path $repo 'build.bat') + '"')
  if ($LASTEXITCODE -ne 0) { throw "build.bat exited $LASTEXITCODE" }
  if (-not (Test-Path -LiteralPath $exe)) { throw "build\mnpdf.exe is missing" }
  Write-Output 'BUILD OK'
} finally {
  Pop-Location
}

function Stop-TestInstances {
  # only ever instances of the binary this run built: a copy the reader has
  # open themselves is never touched
  $own = @()
  Get-CimInstance Win32_Process -Filter "Name = 'mnpdf.exe'" -ErrorAction SilentlyContinue |
    ForEach-Object {
      $exePath = $null
      try { $exePath = $_.ExecutablePath } catch { }
      if ($exePath -and (Split-Path $exePath -Parent) -ieq (Split-Path $exe -Parent)) {
        $own += $_
      }
    }
  foreach ($p in $own) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  if ($own.Count) { Start-Sleep -Milliseconds 400 }
}

$failed = New-Object System.Collections.Generic.List[string]
try {
  $script:realTemp = $env:TEMP
  foreach ($s in $suites) {
    # $state is built from the real TEMP, captured before the first suite
    # rewrote it: the isolated environment must not nest inside a previous one
    $state = Join-Path $script:realTemp ("mnpdf-ci-" + [Guid]::NewGuid().ToString('n'))
    $appData = Join-Path $state 'appdata'
    $temp = Join-Path $state 'temp'
    $null = New-Item -ItemType Directory -Force -Path $appData
    $null = New-Item -ItemType Directory -Force -Path $temp
    $log = Join-Path $state 'output.log'
    Write-Output "=== $s"
    # a child process, so the isolated environment cannot leak into the runner
    # or into the next suite
    $env:APPDATA = $appData
    $env:TEMP = $temp
    $env:TMP = $temp
    $env:MNPDF_GATE_EXE = $exe
    $env:MNPDF_GATE_STATE = $state
    $env:MNPDF_GATE_SUITE = (Join-Path $repo ($s + '.ps1'))
    # the app only checks that the variable exists, so a foreground suite needs
    # it removed rather than set to 0
    if ($foreground -contains $s) { Remove-Item Env:MNPDF_BACKGROUND -ErrorAction SilentlyContinue }
    else { $env:MNPDF_BACKGROUND = '1' }
    Stop-TestInstances
    # The suite writes its own log. Redirecting its stdout to a file instead
    # would deadlock: Start-Process -Wait also waits for the redirected pipe to
    # close, and the app the suite launches inherits that handle, so the wait
    # never returns.
    $inner = ('& "' + $env:MNPDF_GATE_SUITE + '" *>&1 | Tee-Object -FilePath "' + $log + '"; exit $LASTEXITCODE')
    # Called directly rather than through Start-Process -Wait: that also waits
    # for the child's inherited output handle, and the app the suite launches
    # holds it open long after the suite itself has finished.
    & $psExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $inner
    $suiteExit = $LASTEXITCODE
    $output = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
    $output | Where-Object { $_ -match '^(PASS|FAIL|SKIP|RESULT)' } | ForEach-Object { Write-Output "  $_" }
    Stop-TestInstances
    $bad = @($output | Where-Object { $_ -match '^(FAIL|SKIP:|RESULT: [0-9]+ FAILURE)' })
    if ($suiteExit -ne 0 -or $bad.Count -gt 0 -or
        -not ($output | Where-Object { $_ -match '^PASS ' })) {
      [void]$failed.Add(("$s failed (exit $($suiteExit))"))
      Write-Output "CI: $s FAILED (exit $($suiteExit)); stopping before another suite launches."
      break
    }
    Write-Output "CI: $s OK"
    Remove-Item -LiteralPath $state -Recurse -Force -ErrorAction SilentlyContinue
  }
} finally {
  Stop-TestInstances
  $env:MNPDF_BACKGROUND = '1'
}
Write-Output ''
if ($failed.Count) {
  foreach ($f in $failed) { Write-Output "CI: $f" }
  Write-Output ('CI RESULT: {0} FAILURE(S)' -f $failed.Count)
  exit 1
}
Write-Output 'CI RESULT: ALL SUITES OK'
