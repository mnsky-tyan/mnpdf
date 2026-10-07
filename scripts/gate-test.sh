#!/usr/bin/env bash
# Run the Windows suites from WSL, with isolated preferences and bounded waits.
# This is the WSL entry point; scripts/test-ci.ps1 is the same thing on plain
# Windows, which is what .github/workflows/ci.yml runs. The roster, the build,
# the per-suite isolation and the verdict all live in scripts/gate-common.ps1
# and scripts/suites.txt, shared with that runner; what is left here is the
# WSL-specific glue: the drive-letter guard, the checkout lock, evidence
# retention, orphan reaping, and the refusal to close an app that is already up.
set -euo pipefail
cd "$(dirname "$0")/.."

# 9 is never a target of redirection inside this script, so the lock survives
# every command's cleanup
SH() { "$@" 9>&-; }

PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
WINPWD=$(wslpath -w "$PWD")
case "$WINPWD" in
  [A-Za-z]:*) ;;
  *)
    printf '%s\n' 'GATE: refusing a WSL/network-path launch. Put this checkout on a local Windows drive.' >&2
    exit 1
    ;;
esac

mkdir -p build
# bound the evidence this runner keeps: drop isolated states older than a week
find build -maxdepth 1 -name 'gate-run.*' -mtime +7 -exec rm -rf {} + 2>/dev/null || true
exec 9>build/gate-test.lock
flock -n 9 || { printf '%s\n' 'GATE: another runner owns this checkout.' >&2; exit 1; }

export MNPDF_GATE_EXE="${WINPWD}\\build\\mnpdf.exe"
export MNPDF_GATE_STATE='' MNPDF_GATE_SUITE=''
export MNPDF_BACKGROUND=1
export WSLENV="${WSLENV:+${WSLENV}:}MNPDF_GATE_EXE/w:MNPDF_GATE_STATE/w:MNPDF_GATE_SUITE/w:MNPDF_BACKGROUND/w:MNPDF_FORCE_BACKGROUND/w:MNPDF_WINDOW_DESKTOP/w:MNPDF_VD_DLL/w"
# MNPDF_BACKGROUND: the app starts minimized without activating, so gate runs
# never steal focus. Suites inherit it through the test shell's environment.
GATE_COMMON="${WINPWD}\\scripts\\gate-common.ps1"

cleanup_instances() {
  [[ -n "$MNPDF_GATE_STATE" ]] || return 0
  SH timeout --kill-after=5s 60s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -Command ". '${GATE_COMMON}'; Stop-RunnerTree"
}
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

# reap any runner shell or app an older interrupted run left behind
for d in build/gate-run.*; do
  [[ -d "$d" ]] || continue
  for s in "$d"/*/runner.json; do
    [[ -f "$s" ]] || continue
    MNPDF_GATE_STATE=$(wslpath -w "${s%/runner.json}") cleanup_instances || true
  done
done

# Do not close an existing app to make a test runnable, or accept its SKIP as
# a pass: the suites must have the machine to themselves.
if ! SH timeout --kill-after=5s 20s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -Command ". '${GATE_COMMON}'; Assert-NoAppRunning"; then
  printf '%s\n' 'GATE: an mnpdf instance is already running; leaving it untouched.' >&2
  exit 1
fi

printf '%s\n' '=== build'
if ! SH timeout --kill-after=5s 600s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -Command ". '${GATE_COMMON}'; Invoke-Build -RepoRoot '${WINPWD}' -ExePath '${MNPDF_GATE_EXE}'"; then
  printf '%s\n' 'GATE: BUILD FAILED' >&2
  exit 1
fi

[[ -f scripts/suites.txt ]] || { printf '%s\n' 'GATE: scripts/suites.txt is missing.' >&2; exit 1; }
# MNPDF_SKIP_SUITES: comma-separated suite names dropped from this run's roster.
# Trimmed the way Read-SuiteRoster trims them for the CI runner, so both entry
# points run the same list.
skip=' '
if [[ -n "${MNPDF_SKIP_SUITES:-}" ]]; then
  for k in ${MNPDF_SKIP_SUITES//,/ }; do skip="$skip$k "; done
fi
suites=()
foreground=' '
while IFS= read -r raw || [[ -n "$raw" ]]; do
  line=$(printf '%s' "${raw%%#*}" | tr -d ' \t\r')
  [[ -z "$line" ]] && continue
  fg=0
  if [[ "$line" == *'*' ]]; then
    line="${line%\*}"
    fg=1
  fi
  [[ "${skip,,}" == *" ${line,,} "* ]] && continue
  if (( fg )); then foreground="$foreground$line "; fi
  suites+=("$line")
done < scripts/suites.txt
((${#suites[@]})) || { printf '%s\n' 'GATE: no suites in scripts/suites.txt.' >&2; exit 1; }

run_dir=$(mktemp -d "$PWD/build/gate-run.XXXXXX")
printf 'GATE: logs and isolated preferences: %s\n' "$run_dir"

for s in "${suites[@]}"; do
  state="$run_dir/$s"
  mkdir -p "$state/appdata" "$state/temp"
  export MNPDF_GATE_STATE=$(wslpath -w "$state")
  export MNPDF_GATE_SUITE="${WINPWD}\\${s}.ps1"
  # the app only checks that the variable exists, so a foreground suite needs it
  # gone, not set to 0
  if [[ "$foreground" == *" $s "* ]]; then unset MNPDF_BACKGROUND; else export MNPDF_BACKGROUND=1; fi
  printf '=== %s\n' "$s"
  rc=0
  ( exec 9>&-; SH timeout --kill-after=5s 900s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
      -Command ". '${GATE_COMMON}'; Invoke-SuiteRun" ) || rc=$?
  cleanup_instances
  # the log is written by PowerShell, so its lines end CRLF: the optional CR
  # in the pattern is what keeps the verdict match honest
  if (( rc != 0 )) || ! grep -qE $'^SUITE RESULT: OK\r?$' "$state/output.log"; then
    printf 'GATE: %s FAILED (exit %s); stopping before another suite launches.\n' "$s" "$rc" >&2
    exit 1
  fi
  printf 'GATE: %s OK\n' "$s"
done
printf '%s\n' '=== GATE RESULT: ALL SUITES OK'
