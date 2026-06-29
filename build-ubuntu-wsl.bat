@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-ubuntu-wsl.ps1" %*
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
    echo.
    echo Ubuntu WSL2 build failed with exit code %RESULT%.
    pause
)
exit /b %RESULT%
