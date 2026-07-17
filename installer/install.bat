@echo off
REM ============================================================================
REM install.bat — Install the WinCache filter driver
REM
REM Steps:
REM 1. Copy cacheflt.sys to %%SystemRoot%%\system32\drivers
REM 2. Create the driver service (boot-start)
REM 3. Start the driver
REM 4. (Optional) Create the cache partition if needed
REM
REM Run as Administrator!
REM ============================================================================

setlocal enabledelayedexpansion

echo.
echo ================================================
echo  WinCache Driver Installer
echo ================================================
echo.

REM --- Check for admin privileges ---
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and select "Run as Administrator".
    pause
    exit /b 1
)

REM --- Check that cacheflt.sys exists ---
set DRIVER_SRC=%~dp0..\driver\cacheflt.sys
set DRIVER_DST=%SystemRoot%\system32\drivers\cacheflt.sys

if not exist "%DRIVER_SRC%" (
    echo ERROR: cacheflt.sys not found at %DRIVER_SRC%
    echo Build the driver first using the EWDK / WDK.
    pause
    exit /b 1
)

REM --- Copy driver ---
echo [1/4] Copying driver to %DRIVER_DST%...
copy /Y "%DRIVER_SRC%" "%DRIVER_DST%"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to copy driver file.
    pause
    exit /b 1
)
echo        OK.

REM --- Create service ---
echo [2/4] Creating driver service...
sc create cacheflt type= kernel start= boot binPath= "%DRIVER_DST%" group= "PNP Filter" error= normal
if %ERRORLEVEL% NEQ 0 (
    echo        Service may already exist — attempting to start it.
)

REM --- Configure as Disk class lower filter ---
echo [3/4] Registering as Disk class lower filter...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E967-E325-11CE-BFC1-08002BE10318}" /v LowerFilters /t REG_MULTI_SZ /d cacheflt /f
echo        OK.

REM --- Start driver ---
echo [4/4] Starting driver...
sc start cacheflt
if %ERRORLEVEL% NEQ 0 (
    echo        Driver may need a reboot to start — please reboot.
)

echo.
echo ================================================
echo  Installation complete!
echo.
echo  Next steps:
echo    1. Initialize the cache SSD partition:
echo       cachectl init-cache \Device\HarddiskX\PartitionY 256
echo       (creates a 256GB cache on the specified partition)
echo    2. Attach the cache to a source HDD:
echo       cachectl attach \Device\HarddiskA\DR0 \Device\HarddiskX\PartitionY
echo    3. Check status:
echo       cachectl status
echo       cachectl stats
echo ================================================
pause
