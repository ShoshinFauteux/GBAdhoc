<#
stop_relay.ps1 -- stop exactly the processes start_relay.ps1 started (by the PIDs it
recorded in %LOCALAPPDATA%\GBAdhocRelay\), never anything by name.
#>
param([string]$Distro = "Ubuntu")
$state = Join-Path $env:LOCALAPPDATA "GBAdhocRelay"
$f = Join-Path $state "winfwd.pid"
if (Test-Path $f) {
  $p = [int](Get-Content $f)
  if (Get-Process -Id $p -ErrorAction SilentlyContinue) { Stop-Process -Id $p -Force; "stopped winfwd $p" }
  Remove-Item $f
}
$f = Join-Path $state "relay.pid"
if (Test-Path $f) {
  $p = (Get-Content $f).Trim()
  if ($p -match '^\d+$') { & wsl -d $Distro -- kill $p; "stopped relay (WSL PID $p)" }
  Remove-Item $f
}
$f = Join-Path $state "wsl.pid"
if (Test-Path $f) {
  $p = [int](Get-Content $f)
  Start-Sleep -Seconds 1
  if (Get-Process -Id $p -ErrorAction SilentlyContinue) { Stop-Process -Id $p -Force; "stopped wsl.exe $p" }
  Remove-Item $f
}
