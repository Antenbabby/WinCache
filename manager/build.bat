@echo off
REM build.bat — Build the user-mode management tool (cachectl.exe)
REM
REM Requires: Visual Studio Build Tools or full VS (for cl.exe)
REM Or: EWDK with MSVC toolchain
REM
REM Run this from an MSVC Developer Command Prompt, or launch
REM the EWDK build environment first.

set SRC=main.c
set OUT=cachectl.exe
set SHARED=..\shared
set CFLAGS=/nologo /W4 /O2 /I%SHARED% /DUNICODE /D_UNICODE
set LIBS=kernel32.lib advapi32.lib user32.lib

echo Building cachectl.exe...
cl %CFLAGS% /Fe:%OUT% %SRC% /link %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo OK — %OUT% built successfully
) else (
    echo FAILED — check errors above
)
