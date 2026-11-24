@echo off
setlocal
set "ENV_VENV_SCRIPTS=D:\RT-ThreadStudio\platform\env_released\env-new\.venv\Scripts"
if exist "%ENV_VENV_SCRIPTS%\scons.exe" (
    set "PATH=%ENV_VENV_SCRIPTS%;%PATH%"
    "%ENV_VENV_SCRIPTS%\scons.exe" %*
) else (
    scons %*
)
exit /b %ERRORLEVEL%
