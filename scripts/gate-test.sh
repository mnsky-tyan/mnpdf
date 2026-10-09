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

# MNPDF_VD_DLL is a WINDOWS path (the seat prints C:\... from the Windows side,
# and ~/.local/bin/mnpdf-gate exports the same), because PowerShell is what loads
# it via Test-Path/LoadLibraryW. A bare -f tests the Linux filesystem, where
# 'C:\...' never exists, so translate the Windows form before testing and fall
# back to the raw path for a native Linux path (CI has neither and must not warn
# spuriously - the caller only warns when the variable is set but unusable).
dll_exists() {
  local p="${1:-}" w
  [[ -n "$p" ]] || return 1
  if [[ "$p" == [A-Za-z]:[\\/]* ]]; then
    w=$(wslpath "$p" 2>/dev/null || true)
    [[ -n "$w" && -f "$w" ]]
  else
    [[ -f "$p" ]]
  fi
}

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
# The placement seams. The app reads MNPDF_WINDOW_DESKTOP only to start itself
# minimised and to refuse activation (the placed path starts minimised,
# whatever MNPDF_BACKGROUND says); it has no virtual-desktop code of its own.
# tests/lib.ps1 moves the app's windows to that desktop after launch (and
# follows the popups opened later) using MNPDF_VD_DLL. The windowed wrapper
# exports both vars already; this resolves them
# here so a plain 'gate-test.sh' launch is equally safe - a run the pipeline's
# test agent starts, for instance, with no wrapper involved. A machine without
# the agent seat (CI) resolves neither var and runs exactly as before: the
# suite shells then run foreground as they always have on such a machine.
# Resolved via PATH and a $HOME/.local/bin fallback, because this runs under a
# plain non-login `bash` that does not source ~/.profile, where ~/.local/bin is
# not on PATH. If the seat still cannot resolve, warn LOUDLY: the run then
# places windows on whatever desktop is current (the person's working one), and
# the log must say so rather than looking identical to a clean run. Both lookups
# - PATH, then the absolute $HOME/.local/bin - collapse into one variable that
# is tested once, so no lookup branch can fall through silently: either the seat
# resolves or exactly one loud line goes to stderr.
if [[ -n "${MNPDF_WINDOW_DESKTOP:-}" ]]; then
  # A wrapper may have exported the desktop but not the accessor DLL (the app
  # only reads MNPDF_WINDOW_DESKTOP itself; tests/lib.ps1 needs MNPDF_VD_DLL to
  # do the moving). Claiming placement without it would describe a run that
  # leaves every window on the current desktop, so the DLL is required here too.
  if dll_exists "${MNPDF_VD_DLL:-}"; then
    printf 'GATE: window placement already set by the wrapper (desktop %s)\n' "$MNPDF_WINDOW_DESKTOP"
  else
    printf 'GATE: no placement seat resolved (MNPDF_VD_DLL is unset or missing); windows will appear on the current desktop\n' >&2
  fi
else
  # PATH first, then the absolute fallback: this runs under a plain non-login
  # `bash` that never sources ~/.profile, so ~/.local/bin is not on PATH there
  seat_tool="$(command -v win-desktop-show 2>/dev/null || true)"
  if [[ -z "$seat_tool" && -x "$HOME/.local/bin/win-desktop-show" ]]; then
    seat_tool="$HOME/.local/bin/win-desktop-show"
  fi
  seat_index=''
  seat_dll=''
  if [[ -n "$seat_tool" ]]; then
    seat="$(timeout 20s "$seat_tool" --seat 2>/dev/null || true)"
    candidate="$(printf '%s\n' "$seat" | sed -n '1p')"
    if [[ "$candidate" =~ ^[0-9]+$ ]]; then seat_index="$candidate"; fi
    seat_dll="$(printf '%s\n' "$seat" | sed -n '2p')"
  fi
  if [[ -n "$seat_index" ]] && dll_exists "$seat_dll"; then
    export MNPDF_WINDOW_DESKTOP="$seat_index"
    export MNPDF_VD_DLL="$seat_dll"
    printf 'GATE: windows are placed on desktop %s for this run\n' "$seat_index"
  else
    printf '%s\n' 'GATE: no placement seat resolved (the seat did not report a usable accessor DLL); windows will appear on the current desktop' >&2
  fi
fi
export MNPDF_BACKGROUND=1
export WSLENV="${WSLENV:+${WSLENV}:}MNPDF_GATE_EXE/w:MNPDF_GATE_STATE/w:MNPDF_GATE_SUITE/w:MNPDF_BACKGROUND/w:MNPDF_WINDOW_DESKTOP/w:MNPDF_VD_DLL/w"
# MNPDF_BACKGROUND: the app starts minimized without activating, so gate runs
# never steal focus. Suites inherit it through the test shell's environment.
# MNPDF_WINDOW_DESKTOP + MNPDF_VD_DLL, when the seat resolved above, tell
# tests/lib.ps1 to move each app window to that desktop after launch instead.
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
# LOCKSTEP with scripts/gate-common.ps1 Read-SuiteRoster: this bash parser and
# that PowerShell reader must agree on comments, the trailing-* foreground
# marker and MNPDF_SKIP_SUITES, or the WSL gate and CI would run different
# rosters. The parse lives here because it must work before PowerShell is ever
# invoked; keep the two in step when the rules change.
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
  state_w=$(wslpath -w "$state")
  export MNPDF_GATE_STATE="$state_w"
  export MNPDF_GATE_SUITE="${WINPWD}\\${s}.ps1"
  # the app only checks that the variable exists, so a foreground suite needs it
  # gone, not set to 0. LOCKSTEP: scripts/test-ci.ps1:43-45 makes the same
  # decision for the CI runner from the same roster marker - a change to the
  # rule (the sentinel, the marker) must land in both.
  if [[ "$foreground" == *" $s "* ]]; then unset MNPDF_BACKGROUND; else export MNPDF_BACKGROUND=1; fi
  printf '=== %s\n' "$s"
  rc=0
  ( exec 9>&-; SH timeout --kill-after=5s 900s "$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
      -Command ". '${GATE_COMMON}'; Invoke-SuiteRun" ) || rc=$?
  cleanup_instances
  # the log is written by PowerShell, so its lines end CRLF: the optional CR
  # in the pattern is what keeps the verdict match honest
  # the grep is a second, independent verdict on purpose: it catches the case
  # where the suite shell died before Invoke-SuiteRun could append the line at
  # all. The wording lives in gate-common.ps1 (Get-SuiteVerdict and the verdict
  # Add-Content); a change there must be mirrored in this pattern.
  if (( rc != 0 )) || ! grep -qE $'^SUITE RESULT: OK\r?$' "$state/output.log"; then
    printf 'GATE: %s FAILED (exit %s); stopping before another suite launches.\n' "$s" "$rc" >&2
    exit 1
  fi
  printf 'GATE: %s OK\n' "$s"
done
printf '%s\n' '=== GATE RESULT: ALL SUITES OK'
