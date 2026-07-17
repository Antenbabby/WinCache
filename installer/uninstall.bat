@echo off
REM ============================================================================
REM uninstall.bat — Uninstall the WinCache filter driver
REM
REM Run as Administrator!
REM ============================================================================

echo.
echo ================================================
echo  WinCache Driver Uninstaller
echo ================================================
echo.

REM --- Check for admin privileges ---
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: This script must be run as Administrator.
    pause
    exit /b 1
)

REM --- Stop driver ---
echo [1/3] Stopping driver...
sc stop cacheflt
echo        OK.

REM --- Remove Disk class LowerFilters entry ---
echo [2/3] Removing LowerFilters registration...
reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E967-E325-11CE-BFC1-08002BE10318}" /v LowerFilters /f
echo        OK.

REM --- Delete service ---
echo [3/3] Deleting driver service...
sc delete cacheflt
echo        OK.

REM --- Remove driver file ---
del /F "%SystemRoot%\system32\drivers\cacheflt.sys" 2>nul

echo.
echo Uninstallation complete. Reboot recommended.
pause
