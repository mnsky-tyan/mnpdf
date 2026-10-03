# Crash-path restore for a forged app.txt. Init-PrefForge (tests/lib.ps1)
# spawns this child, which outlives the suite shell by any means - timeout,
# tree kill, hard crash - and is the only thing that restores the user's
# preferences on those paths.
#
# All policy lives in lib.ps1's Restore-PrefState; this file only waits for the
# parent to die and hands the paths over as arguments, so an apostrophe in a
# profile path cannot break the restore the way interpolating them into
# generated code once did.
#
# Invoked positionally (powershell.exe -File does not bind named parameters):
#   ParentPid LibPath AppPref Marker Backup Missing [Sentinel]
param(
  [Parameter(Mandatory=$true)][int]$ParentPid,
  [Parameter(Mandatory=$true)][string]$LibPath,
  [Parameter(Mandatory=$true)][string]$AppPref,
  [Parameter(Mandatory=$true)][string]$Marker,
  [Parameter(Mandatory=$true)][string]$Backup,
  [Parameter(Mandatory=$true)][string]$Missing,
  [string]$Sentinel = 'test-forged=1'
)
$ErrorActionPreference = 'Stop'
. $LibPath

while (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
Start-Sleep -Milliseconds 400

Restore-PrefState -AppPref $AppPref -Marker $Marker -Backup $Backup -Missing $Missing -Sentinel $Sentinel
# the scratch files' job ends once the prefs are back; drop them so a later
# run cannot mistake stale state for its own (a tree-killed watchdog that never
# ran leaves them in place, which is exactly the leftover-forge signal)
Remove-Item -LiteralPath $Backup, $Missing -Force -ErrorAction SilentlyContinue
