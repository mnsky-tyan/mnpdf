#!/usr/bin/env bash
# Run the Windows suites on a local drive, with isolated preferences and bounded
# waits. commands.test supplies a baseline; it does not replace the Test agent.
set -euo pipefail
cd "$(dirname "$0")/.."

SH() { "$@" 9>&-; }

PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
CMD=/mnt/c/Windows/System32/cmd.exe
WINPWD=$(wslpath -w "$PWD")
if [[ ! "$WINPWD" =~ ^[A-Za-z]: ]]; then
  printf '%s\n' 'GATE: refusing a WSL/network-path launch. Configure this checkout in global worktree_roots on a local Windows drive.' >&2
  exit 1
fi

mkdir -p build
# bound the evidence this runner keeps: drop isolated states older than a week
find build -maxdepth 1 -name 'gate-run.*' -mtime +7 -exec rm -rf {} + 2>/dev/null || true
exec 9>build/gate-test.lock
flock -n 9 || { printf '%s\n' 'GATE: another runner owns this checkout.' >&2; exit 1; }
export MNPDF_GATE_EXE="${WINPWD}\\build\\mnpdf.exe"
export MNPDF_GATE_STATE='' MNPDF_GATE_SUITE=''
export MNPDF_BACKGROUND=1
# MNPDF_BACKGROUND: the app starts minimized without activating, so gate runs
# never steal focus. Suites inherit it through the test shell's environment.
export WSLENV="${WSLENV:+${WSLENV}:}MNPDF_GATE_EXE/w:MNPDF_GATE_STATE/w:MNPDF_GATE_SUITE/w:MNPDF_BACKGROUND"

cleanup_instances() {
  [[ -n "$MNPDF_GATE_STATE" ]] || return 0
  SH timeout --kill-after=5s 30s "$PS" -NoProfile -NonInteractive -Command '
    $ErrorActionPreference = "Stop"
    $record = Join-Path $env:MNPDF_GATE_STATE "runner.json"
    if (-not (Test-Path -LiteralPath $record)) { exit 0 }
    $owned = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
    $runner = Get-Process -Id $owned.Id -ErrorAction SilentlyContinue
    if ($runner -and $runner.ProcessName -eq "powershell" -and
        $runner.StartTime.ToUniversalTime().Ticks -eq $owned.Started) {
      try {
        $runner.Kill()
        if (-not $runner.WaitForExit(5000)) { throw "Test shell did not exit" }
      } catch { if (-not $runner.HasExited) { throw } }
    }
    # Parent identity AND exact executable path, never every build/mnpdf.exe.
    $children = Get-CimInstance Win32_Process -Filter "Name = '\''mnpdf.exe'\''" |
      Where-Object { $_.ParentProcessId -eq $owned.Id -and
        $_.ExecutablePath -eq $env:MNPDF_GATE_EXE -and
        $_.CreationDate.ToUniversalTime().Ticks -ge $owned.Started }
    foreach ($child in $children) {
      $app = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue
      if (-not $app -or $app.Path -ne $env:MNPDF_GATE_EXE) { continue }
      try {
        [void]$app.CloseMainWindow()
        if (-not $app.WaitForExit(3000)) {
          $app.Kill()
          if (-not $app.WaitForExit(3000)) { throw "Test app did not exit" }
        }
      } catch { if (-not $app.HasExited) { throw } }
    }
    exit 0
  '
}
reap_orphans() {
  local d s ws
  for d in build/gate-run.*; do
    [[ -d "$d" ]] || continue
    for s in "$d"/*; do
      [[ -f "$s/runner.json" ]] || continue
      ws=$(wslpath -w "$s")
      ( export MNPDF_GATE_STATE="$ws"; cleanup_instances ) || true
    done
  done
}
reap_orphans

# Do not close an existing app to make a test runnable, or accept its SKIP as PASS.
SH timeout --kill-after=5s 20s "$PS" -NoProfile -NonInteractive -Command '
  $ErrorActionPreference = "Stop"
  if (@(Get-Process mnpdf -ErrorAction SilentlyContinue).Count) {
    Write-Output "GATE: an mnpdf instance is already running; leaving it untouched."
    exit 1
  }
  exit 0
'

finish() {
  local rc=$?
  trap - EXIT
  if ! cleanup_instances; then
    printf '%s\n' 'GATE: cleanup failed; inspect the recorded test process before retrying.' >&2
    rc=1
  fi
  exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf '%s\n' '=== build'
if ! SH timeout --kill-after=5s 600s "$CMD" /c cd /d "$WINPWD" '&&' build.bat; then
  printf '%s\n' 'GATE: BUILD FAILED' >&2
  exit 1
fi
[[ -f build/mnpdf.exe ]] || { printf '%s\n' 'GATE: build/mnpdf.exe missing' >&2; exit 1; }

run_dir=$(mktemp -d "$PWD/build/gate-run.XXXXXX")
printf 'GATE: logs and isolated preferences: %s\n' "$run_dir"
for s in select-msg-test test-features test-suite2 test-continuous test-release; do
  state="$run_dir/$s"
  mkdir -p "$state/appdata" "$state/temp"
  export MNPDF_GATE_STATE="$(wslpath -w "$state")"
  export MNPDF_GATE_SUITE="${WINPWD}\\${s}.ps1"
  printf '=== %s\n' "$s"
  rc=0
  # Invoke the suite directly, not Start-Process -Wait (which waits for all
  # descendants, including apps that the suite intentionally leaves open).
  ( exec 9>&-; SH timeout --kill-after=5s 900s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command '
    $ErrorActionPreference = "Stop"
    $env:APPDATA = Join-Path $env:MNPDF_GATE_STATE "appdata"
    $env:TEMP = Join-Path $env:MNPDF_GATE_STATE "temp"
    $env:TMP = $env:TEMP
    $self = Get-Process -Id $PID
    @{ Id = $PID; Started = $self.StartTime.ToUniversalTime().Ticks } |
      ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $env:MNPDF_GATE_STATE "runner.json")
    $global:LASTEXITCODE = 0
    & $env:MNPDF_GATE_SUITE
    if (-not $?) { exit 1 }
    exit $LASTEXITCODE
  ' 2>&1 | tee "$state/output.log" ) 2>/dev/null || rc=$?
  cleanup_instances
  if (( rc != 0 )) || grep -Eq '^(FAIL|SKIP:|RESULT: [0-9]+ FAILURE)' "$state/output.log" ||
      ! grep -q '^PASS ' "$state/output.log"; then
    printf 'GATE: %s FAILED (exit %s); stopping before another suite launches.\n' "$s" "$rc" >&2
    exit 1
  fi
  printf 'GATE: %s OK\n' "$s"
done
printf '%s\n' '=== GATE RESULT: ALL SUITES OK'
