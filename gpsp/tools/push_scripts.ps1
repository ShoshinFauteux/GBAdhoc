# push_scripts.ps1 -- update the autopilot scripts on both cards, WITHOUT
# destroying anything the collector has not consumed yet.
#
# WHY: the ad-hoc push I was doing deleted RESULT.TXT, STATE.TXT and
# frontend.log before writing.  A parked console re-advertises RESULT.TXT to
# say "I am here"; deleting it starved hw_loop.py, which polls for exactly that
# file, so the collector sat idle while two finished runs went unread -- and
# the frontend.log that would have named the failing step was deleted with it.
# Losing a hardware run to a housekeeping delete is not acceptable.
#
# Rule: this script writes scripts and config ONLY.  Log collection and
# handoff-state cleanup belong to hw_loop.py, which does them in the right
# order and only after it has the data.
param(
  [string]$HostDrive = "D:",
  [string]$JoinDrive = "E:"
)
$ErrorActionPreference = "Stop"
$repo = "c:\Users\DrSto\OneDrive\Desktop\GBA PSP Wirless Trading (Claude)\wt-morning"
$cfg = @{
  $HostDrive = "emerald_tradecenter_host.inputs"
  $JoinDrive = "emerald_tradecenter_join.inputs"
}
$fail = $false
foreach ($d in @($HostDrive, $JoinDrive)) {
  $g = "$d\PSP\GAME\gpsp-harness"
  if (-not (Test-Path $g)) { Write-Host "$d not mounted" -ForegroundColor Yellow; $fail = $true; continue }
  $s = $cfg[$d]
  Copy-Item "$repo\testdata\fixtures\$s" "$g\$s" -Force
  try { Write-VolumeCache -DriveLetter $d[0] -ErrorAction Stop } catch {}
  $a = (Get-FileHash "$repo\testdata\fixtures\$s" -Algorithm MD5).Hash
  $b = (Get-FileHash "$g\$s" -Algorithm MD5).Hash
  if ($a -eq $b) { Write-Host ("  {0} {1} OK" -f $d, $s) }
  else { Write-Host ("  {0} {1} HASH MISMATCH" -f $d, $s) -ForegroundColor Red; $fail = $true }
  # Report what is waiting, do NOT delete it.
  $r = "$g\handoff\RESULT.TXT"
  if (Test-Path $r) {
    $st = ((Get-Content $r) -join ' ')
    Write-Host ("       unconsumed RESULT.TXT present: {0}" -f $st) -ForegroundColor Cyan
  }
}
if ($fail) { exit 1 }
Write-Host "scripts updated; handoff state and logs left intact for the collector" -ForegroundColor Green
exit 0
