$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolchainBin = if ($env:RTT_EXEC_PATH) {
    $env:RTT_EXEC_PATH
} else {
    "D:\RT-ThreadStudio\repo\Extract\ToolChain_Support_Packages\ARM\GNU_Tools_for_ARM_Embedded_Processors\13.3\bin"
}
$SizeExe = Join-Path $ToolchainBin "arm-none-eabi-size.exe"
$EdgeTools = Join-Path $Root "tools\edgeprotecttools\bin\edgeprotecttools.exe"

Write-Host "=== [1/3] Building Cortex-M55 Application ===" -ForegroundColor Cyan
Push-Location $Root
try {
    & (Join-Path $Root "scons.bat") -j16
    if ($LASTEXITCODE -ne 0) { throw "CM55 SCons build failed with exit code $LASTEXITCODE" }
    & $SizeExe -A (Join-Path $Root "rt-thread.elf")
} finally {
    Pop-Location
}

Write-Host "`n=== [2/3] Building Cortex-M33 Companion Firmware ===" -ForegroundColor Cyan
$Cm33Dir = Join-Path $Root "cm33_companion"
Push-Location $Cm33Dir
try {
    & (Join-Path $Cm33Dir "scons.bat") -j16
    if ($LASTEXITCODE -ne 0) { throw "CM33 SCons build failed with exit code $LASTEXITCODE" }
    & $SizeExe -A (Join-Path $Cm33Dir "rt-thread.elf")

    if (Test-Path $EdgeTools) {
        Write-Host "`n=== [3/3] Packaging Debug/cm33_combined.hex ===" -ForegroundColor Cyan
        & $EdgeTools run-config -i (Join-Path $Cm33Dir "config\boot_dualcore_scons.json")
        if ($LASTEXITCODE -ne 0) { throw "edgeprotecttools failed with exit code $LASTEXITCODE" }
    }
} finally {
    Pop-Location
}

$Cm55Hex = Join-Path $Root "Debug\rtthread.hex"
$Cm33Combined = Join-Path $Root "Debug\cm33_combined.hex"

Write-Host "`n=== Dual-Core Build Summary ===" -ForegroundColor Green
if (Test-Path $Cm55Hex) {
    $sz55 = (Get-Item $Cm55Hex).Length
    Write-Host ("  CM55 App Hex:      {0} ({1:N0} B)" -f $Cm55Hex, $sz55)
}
if (Test-Path $Cm33Combined) {
    $sz33 = (Get-Item $Cm33Combined).Length
    Write-Host ("  CM33 Combined Hex: {0} ({1:N0} B)" -f $Cm33Combined, $sz33)
}
Write-Host "  Flashing Guide:"
Write-Host "    - Dual-Core Mode: Flash Debug\cm33_combined.hex AND Debug\rtthread.hex"
Write-Host "    - M55 Solo Mode:  Flash Debug\rtthread.hex only (graceful M55 SOLO fallback)"
