# stage_cards.ps1 -- put the harness onto both memory sticks, safely.
#
# WHY THIS EXISTS (and why it is ASCII-only: PowerShell 5.1 reads .ps1 as ANSI
# without a BOM, so a stray em-dash becomes mojibake and breaks the parse): a parked console re-arms USB on a timer, so its card
# appears and disappears every few tens of seconds.  Copying a 16 MB ROM onto
# a volume that does that loses the write when sceUsbDeactivate fires mid-copy
# -- and Windows' cache then serves the truncated file back, so a read-back
# check says everything is fine.  That happened twice on this project and cost
# two rebuild cycles.
#
# So this script REFUSES to write until it has watched both volumes stay
# continuously present for longer than one park window, and it verifies by
# hash after an explicit flush.  The console must be in MANUAL USB Connection
# from the XMB (where nothing is cycling it), not parked in the handoff loop.

param(
  [string]$HostDrive = "D:",
  [string]$JoinDrive = "E:",
  [int]$StableSeconds = 40,     # must exceed the console's park window (30 s)
  [switch]$SkipStabilityCheck   # escape hatch; you are on your own
)

$ErrorActionPreference = "Stop"
$repo   = "c:\Users\DrSto\OneDrive\Desktop\GBA PSP Wirless Trading (Claude)\wt-morning"
$fix    = "$repo\testdata\fixtures"
$sp     = "C:\Users\DrSto\AppData\Local\Temp\claude\c--Users-DrSto-OneDrive-Desktop-GBA-PSP-Wirless-Trading--Claude-\86ab8597-7548-489f-a178-10e8745decd2\scratchpad"
$assets = "$sp\rigassets"

$cfg = @{
  $HostDrive = @{ role="host"; sav="emerald_parkedB.sav"; script="emerald_trade_host.inputs"; nick="HOSTPSP" }
  $JoinDrive = @{ role="join"; sav="emerald_parkedA.sav"; script="emerald_trade_join.inputs"; nick="JOINPSP" }
}

function Test-Stable {
  param([string[]]$Drives, [int]$Seconds)
  Write-Host "Watching $($Drives -join ', ') for $Seconds s of uninterrupted presence..."
  $end = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $end) {
    foreach ($d in $Drives) {
      if (-not (Test-Path "$d\PSP")) {
        Write-Host "  $d VANISHED -- it is cycling. Not writing." -ForegroundColor Red
        return $false
      }
    }
    Start-Sleep -Milliseconds 500
  }
  Write-Host "  both volumes stable." -ForegroundColor Green
  return $true
}

if (-not $SkipStabilityCheck) {
  if (-not (Test-Stable -Drives @($HostDrive,$JoinDrive) -Seconds $StableSeconds)) {
    Write-Host ""
    Write-Host "The consoles are parked in the handoff loop, which re-arms USB on a timer."
    Write-Host "Hold START+SELECT for ~1.5 s on each to leave the loop, then put them into"
    Write-Host "USB Connection from the XMB, then run this again."
    exit 2
  }
}

$fail = $false
foreach ($d in @($HostDrive, $JoinDrive)) {
  $c = $cfg[$d]
  $g = "$d\PSP\GAME\gpsp-harness"
  Write-Host ""
  Write-Host "=== $d ($($c.role)) ==="
  New-Item -ItemType Directory -Force -Path "$g\roms","$g\log","$g\handoff" | Out-Null

  # source -> destination, and every one is hash-verified after the flush
  $files = @(
    @{ src="$sp\EBOOT-harness.pbp";      dst="$g\EBOOT.PBP" },
    @{ src="$assets\gba_bios.bin";       dst="$g\gba_bios.bin" },
    @{ src="$assets\roms\emerald.gba";   dst="$g\roms\emerald.gba" },
    @{ src="$fix\$($c.sav)";             dst="$g\roms\emerald.sav" },
    @{ src="$fix\$($c.script)";          dst="$g\$($c.script)" }
  )
  foreach ($f in $files) {
    if (-not (Test-Path $f.src)) { Write-Host "  MISSING SOURCE $($f.src)" -ForegroundColor Red; $fail=$true; continue }
    Copy-Item $f.src $f.dst -Force
  }
  $ini = "script = $($c.script)`r`n$($c.role) = 1`r`nnick = $($c.nick)`r`n" +
         "handoff = 1`r`nhandoff_window_s = 30`r`nhandoff_park_s = 0`r`nhandoff_max_runs = 0`r`n" +
         "core_phase = 2`r`ngu_defer = 1`r`nnet_session_fps = 40.00`r`n"
  [System.IO.File]::WriteAllText("$g\.gpsp-harness.ini", $ini, (New-Object System.Text.UTF8Encoding($false)))

  # Flush Windows' cache to the device BEFORE verifying, else the read-back is
  # served from RAM and a truncated file on the card still looks perfect.
  try { Write-VolumeCache -DriveLetter $d[0] -ErrorAction Stop } catch { Write-Host "  flush failed: $_" -ForegroundColor Yellow }

  foreach ($f in $files) {
    if (-not (Test-Path $f.dst)) { Write-Host "  MISSING  $(Split-Path $f.dst -Leaf)" -ForegroundColor Red; $fail=$true; continue }
    $a = (Get-FileHash $f.src -Algorithm MD5).Hash
    $b = (Get-FileHash $f.dst -Algorithm MD5).Hash
    if ($a -eq $b) { Write-Host ("  OK   {0,-24} {1,10} bytes" -f (Split-Path $f.dst -Leaf), (Get-Item $f.dst).Length) }
    else { Write-Host "  HASH MISMATCH $(Split-Path $f.dst -Leaf) -- write was truncated" -ForegroundColor Red; $fail=$true }
  }
  if (Test-Path "$g\.gpsp-harness.ini") { Write-Host "  OK   .gpsp-harness.ini" } else { Write-Host "  MISSING .gpsp-harness.ini" -ForegroundColor Red; $fail=$true }
}

Write-Host ""
if ($fail) { Write-Host "STAGING FAILED -- do not launch." -ForegroundColor Red; exit 1 }
Write-Host "STAGING VERIFIED on both cards. Safe to launch 'PSP AGB HARNESS'." -ForegroundColor Green
exit 0
