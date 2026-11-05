<#
start_relay.ps1 -- run the GBAdhoc online relay on this PC (docs/ONLINE-TUNNEL.md 6).

  gbadhoc-relay  in WSL (Ubuntu), as the normal (unprivileged) WSL user, in
                 forwarded mode on UDP 27430 inside WSL, status on 127.0.0.1:27429
  winfwd.py      on Windows, bound to the LAN address only (default 10.0.0.40),
                 UDP 27420-27421: what the router's port forward points at

Both run hidden in the background.  Their PIDs go to %LOCALAPPDATA%\GBAdhocRelay\
(relay.pid is the Linux PID inside WSL, winfwd.pid / wsl.pid the Windows ones);
stop_relay.ps1 stops exactly those PIDs.  Logs: relay.log (in WSL, ~/gbrelay/),
winfwd.log here.

  powershell -ExecutionPolicy Bypass -File start_relay.ps1 [-Lan 10.0.0.40]
#>
param([string]$Lan = "10.0.0.40", [string]$Distro = "Ubuntu")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$state = Join-Path $env:LOCALAPPDATA "GBAdhocRelay"
New-Item -ItemType Directory -Force $state | Out-Null

if (Test-Path (Join-Path $state "winfwd.pid")) {
  $old = Get-Content (Join-Path $state "winfwd.pid")
  if (Get-Process -Id $old -ErrorAction SilentlyContinue) { throw "already running (winfwd PID $old); run stop_relay.ps1 first" }
}

# the source tree as WSL sees it
$src = (& wsl -d $Distro -- wslpath -a ($here -replace '\\','/')).Trim()
# build into ~/gbrelay (not /tmp: survives WSL restarts)
& wsl -d $Distro -- bash -lc "mkdir -p ~/gbrelay && bash '$src/build.sh' ~/gbrelay/bin >/dev/null && printf '%s\n' '#!/bin/sh' 'exec ~/gbrelay/bin/gbadhoc-relay --bind 0.0.0.0 --fwd 27430 --status 27429 --pidfile ~/gbrelay/relay.pid >> ~/gbrelay/relay.log 2>&1' > ~/gbrelay/run.sh && chmod +x ~/gbrelay/run.sh && echo built"
if ($LASTEXITCODE -ne 0) { throw "relay build failed" }

# the relay: a hidden wsl.exe that stays attached (WSL stops a distro with no client)
$wsl = Start-Process -FilePath "wsl.exe" -WindowStyle Hidden -PassThru `
  -ArgumentList "-d $Distro -- bash -l ~/gbrelay/run.sh"
Set-Content -Path (Join-Path $state "wsl.pid") -Value $wsl.Id
Start-Sleep -Seconds 2
$relayPid = (& wsl -d $Distro -- bash -lc "cat ~/gbrelay/relay.pid").Trim()
Set-Content -Path (Join-Path $state "relay.pid") -Value $relayPid

# the WSL address the forwarder sends to (changes when WSL restarts)
$wslIp = ((& wsl -d $Distro -- hostname -I).Trim() -split '\s+')[0]
$py = (Get-Command python).Source
$fwd = Start-Process -FilePath $py -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $state "winfwd.log") -RedirectStandardError (Join-Path $state "winfwd.err") `
  -ArgumentList @("-u", (Join-Path $here "winfwd.py"), "--lan", $Lan, "--wsl", $wslIp)
Set-Content -Path (Join-Path $state "winfwd.pid") -Value $fwd.Id

Write-Output "relay up: LAN $Lan`:27420-27421 -> WSL $wslIp`:27430"
Write-Output "PIDs: winfwd $($fwd.Id), wsl.exe $($wsl.Id), relay (in WSL) $relayPid  [$state]"
Write-Output "status: python status_relay.py"
