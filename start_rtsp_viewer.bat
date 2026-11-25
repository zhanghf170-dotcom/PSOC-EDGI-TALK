@echo off
setlocal
where ffplay.exe >nul 2>&1
if errorlevel 1 (
    echo FFplay was not found on PATH. Install FFmpeg with FFplay and try again.
    pause
    exit /b 1
)
set "EDGI_RTSP_IPV4="
set /p "EDGI_RTSP_IPV4=Board IPv4 address: "
powershell.exe -NoProfile -Command "$ip = $null; if (-not [ipaddress]::TryParse($env:EDGI_RTSP_IPV4, [ref]$ip) -or $ip.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { Write-Host 'Enter the board IPv4 address displayed on its Wi-Fi screen.'; exit 1 }; & ffplay.exe -hide_banner -loglevel warning -rtsp_transport tcp -timeout 20000000 -window_title 'PSOC-EDGI-TALK RTSP' ('rtsp://{0}:8554/camera' -f $ip); exit $LASTEXITCODE"
if errorlevel 1 (
    pause
    exit /b 1
)
exit /b 0
