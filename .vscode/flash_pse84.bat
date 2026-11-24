@echo off
setlocal EnableExtensions

pushd "%~dp0.."
set "OPENOCD_EXE=D:\RT-ThreadStudio\repo\Extract\Debugger_Support_Packages\Infineon\OpenOCD-Infineon\2.0.0\bin\openocd.exe"
set "CFG_FILE=.vscode/openocd_pse84.cfg"
set "QSPI_CFG_DIR=libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/config/GeneratedSource"
set "HEX_FILE=Debug/rtthread.hex"
if not "%~1"=="" set "HEX_FILE=%~1"
set "HEX_FILE_TCL=%HEX_FILE:\=/%"

if not exist "%OPENOCD_EXE%" (
    echo [ERROR] Infineon OpenOCD not found at: "%OPENOCD_EXE%"
    popd
    exit /b 1
)

if not exist "%HEX_FILE%" (
    echo [ERROR] Firmware image "%HEX_FILE%" not found in "%CD%".
    echo         Please build the project first ^(Ctrl+Shift+B or Status Bar "Build"^).
    popd
    exit /b 1
)

if not exist "%QSPI_CFG_DIR%/qspi_config.cfg" (
    echo [ERROR] QSPI configuration not found at: "%QSPI_CFG_DIR%/qspi_config.cfg"
    popd
    exit /b 1
)

echo ============================================================================
echo [Flash] Target : Infineon PSoC Edge E84 ^(PSE846GPS2DBZC4A - Cortex-M55^)
echo [Flash] Probe  : KitProg3 ^(SWD^) via Infineon OpenOCD 2.0.0
echo [Flash] Image  : %HEX_FILE_TCL%
echo ============================================================================

"%OPENOCD_EXE%" -s "%QSPI_CFG_DIR%" -f "%CFG_FILE%" -c "targets cat1d.cm33; flash write_image erase {%HEX_FILE_TCL%}; verify_image {%HEX_FILE_TCL%}; reset run; shutdown"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
    echo.
    echo [ERROR] Flashing failed ^(exit code %RC%^). Troubleshooting:
    echo   1. Check that the Edgi-Talk KitProg3 USB port is connected to this PC.
    echo   2. Close any active debug session ^(OpenOCD / RT-Thread Studio^) holding KitProg3.
    echo   3. Ensure CM33 Secure + CM33 Non-Secure boot firmware is already programmed on the board.
    popd
    exit /b %RC%
)

echo.
echo [OK] Flash programming, verification and reset-to-run completed.
popd
exit /b 0
