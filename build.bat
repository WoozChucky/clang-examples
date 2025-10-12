@echo off
setlocal enableextensions
REM ======================================================
REM Setup MSVC environment for x64 before building
REM ======================================================

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul

REM ======================================================
REM Build step with clang (Debug by default)
REM Usage: build.bat [release]
REM   Debug:   -g -O0 -Wall -Wextra
REM   Release: -O3 -DNDEBUG -Wall -Wextra
REM ======================================================

set MODE=debug
set TARGET=all

REM Parse arguments: release|debug and dll|exe|all
for %%A in (%*) do (
    if /I "%%~A"=="release" set MODE=release
    if /I "%%~A"=="debug"   set MODE=debug
    if /I "%%~A"=="dll"     set TARGET=dll
    if /I "%%~A"=="exe"     set TARGET=exe
    if /I "%%~A"=="all"     set TARGET=all
)

REM Create a timestamp for unique PDB names (only used in Debug)
for /f %%i in ('powershell -NoProfile -Command "(Get-Date).ToString(\"yyyyMMdd_HHmmss_fff\")"') do set TS=%%i

REM Targets and inputs
set SRC_EXE=src/main.cpp src/renderer.cpp
set OUT_EXE=main.exe
set SRC_DLL=src/game.cpp
set OUT_DLL=game.dll

set LIBS=-luser32.lib -lkernel32.lib -lgdi32.lib -lole32.lib

if /I "%MODE%"=="release" (
    set CXXFLAGS=-std=c++23 -O3 -DNDEBUG -Wall -Wextra
    set LDFLAGS_EXE=Wl,/SUBSYSTEM:WINDOWS
    set LDFLAGS_DLL=-Wl,/IMPLIB:game.lib
) else (
    set CXXFLAGS=-std=c++23 -g -O0 -Wall -Wextra -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces
    REM Use unique PDB filenames to avoid debugger locks
    set LDFLAGS_EXE=-Wl,/DEBUG -Wl,/PDB:"%CD%\main-%TS%.pdb"
    set LDFLAGS_DLL=-Wl,/DEBUG -Wl,/PDB:"%CD%\game-%TS%.pdb" -Wl,/IMPLIB:game.lib
)

REM Decide what to build
set DO_DLL=0
set DO_EXE=0
if /I "%TARGET%"=="dll" set DO_DLL=1
if /I "%TARGET%"=="all" set DO_DLL=1
if /I "%TARGET%"=="exe" set DO_EXE=1
if /I "%TARGET%"=="all" set DO_EXE=1

REM ------------------------------------------------------
REM 1 - Build game shared library DLL
REM ------------------------------------------------------
if "%DO_DLL%"=="1" (
    echo Building %SRC_DLL% -> %OUT_DLL% (%MODE%)
    clang %CXXFLAGS% -DGAME_EXPORTS -shared %SRC_DLL% -o %OUT_DLL% %LIBS% %LDFLAGS_DLL%
    set ERR=%ERRORLEVEL%
    if not "%ERR%"=="0" (
        echo Build failed DLL with error %ERR%.
        endlocal & exit /b %ERR%
    )
)

REM ------------------------------------------------------
REM 2 - Build main executable, skip if it's running
REM ------------------------------------------------------
set "EXE_PATH=%CD%\%OUT_EXE%"
powershell -NoProfile -Command "$p=Get-Process -Name 'main' -ErrorAction SilentlyContinue; if($p){ foreach($proc in $p){ if($proc.Path -and [System.IO.Path]::GetFullPath($proc.Path) -ieq [System.IO.Path]::GetFullPath('%EXE_PATH%')){ exit 100 } } }; exit 0"
set ERR=%ERRORLEVEL%
if "%ERR%"=="100" (
    echo Skipping EXE build because %OUT_EXE% is currently running.
) else (
    if "%DO_EXE%"=="1" (
        echo Building %SRC_EXE% -> %OUT_EXE% (%MODE%)
        clang %CXXFLAGS% %SRC_EXE% -o %OUT_EXE% %LIBS% %LDFLAGS_EXE%
        set ERR=%ERRORLEVEL%
        if not "%ERR%"=="0" (
            echo Build failed EXE with error %ERR%.
            endlocal & exit /b %ERR%
        )
    )
)

echo Build succeeded (%MODE%).
endlocal & exit /b 0