@echo off
setlocal EnableExtensions
title Gemini-chan Cute Female Live TTS Bridge (zh-CN-XiaoyiNeural)
cd /d "%~dp0"

set "PY_EXE="
for %%P in (
    "%LOCALAPPDATA%\Programs\Python\Python311\python.exe"
    "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
    "python"
) do (
    if not defined PY_EXE (
        %%~P -c "import edge_tts" >nul 2>&1
        if not errorlevel 1 set "PY_EXE=%%~P"
    )
)

if not defined PY_EXE (
    echo [ERROR] No Python environment with edge-tts found. Run: pip install -U edge-tts
    pause
    exit /b 1
)

"%PY_EXE%" -u tools\gemini_tts_bridge.py --voice zh-CN-XiaoyiNeural --pitch +6Hz --rate +4%% --port 8890
pause
