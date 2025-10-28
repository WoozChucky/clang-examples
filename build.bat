@echo off
setlocal enableextensions
REM ======================================================
REM Setup MSVC environment for x64 before building
REM ======================================================

REM call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul
REM call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

REM ======================================================
REM Build ONLY the game shared library (Debug by default)
REM Output DLL must be: build\debug\bin\Debug\game.dll
REM Usage: build.bat [release|debug]
REM   Debug:   -g -O0 -Wall -Wextra (+ unique PDB)
REM   Release: -O3 -DNDEBUG -Wall -Wextra
REM ======================================================

set MODE=debug
for %%A in (%*) do (
    if /I "%%~A"=="release" set MODE=release
    if /I "%%~A"=="debug"   set MODE=debug
)

REM Create a timestamp for unique PDB names (only used in Debug)
for /f %%i in ('powershell -NoProfile -Command "(Get-Date).ToString(\"yyyyMMdd_HHmmss_fff\")"') do set TS=%%i

REM Inputs/outputs
set SRC_DLL=src\game\src\game.cpp
set OUT_DIR=build\debug-clang\bin\Debug
set OUT_DLL=%OUT_DIR%\game_temp.dll

REM Dependencies and defs (match CMake target_compile_definitions for game)
set INC=-Ithird_party\glm -Isrc\common\include -Isrc\game\include
set DEFINES=-DGAME_EXPORTS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RIGHT_HANDED -DGLM_ENABLE_EXPERIMENTAL
set LIBS=-luser32.lib -lkernel32.lib -lgdi32.lib -lole32.lib
set SYSTEM_LIBS_LOCATION_UM="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64"
set SYSTEM_LIBS_LOCATION_UCRT="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64"
set WINSDK_LIB_PATH="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
set MSVC_LIBS_LOCATION="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

if /I "%MODE%"=="release" (
    set CXXFLAGS=-std=c++23 -O3 -DNDEBUG -Wall -Wextra
    set LDFLAGS_DLL=-Wl,/IMPLIB:"%OUT_DIR%\game.lib"
) else (
    set CXXFLAGS=-std=c++23 -g -O0 -Wall -Wextra -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces
    REM Use unique PDB filenames to avoid debugger locks
    set LDFLAGS_DLL=-Wl,/DEBUG -Wl,/PDB:"%OUT_DIR%\game-%TS%.pdb" -Wl,/IMPLIB:"%OUT_DIR%\game.lib"
)

REM ------------------------------------------------------
REM Build game shared library DLL
REM ------------------------------------------------------

echo Building %SRC_DLL% -> %OUT_DLL% (%MODE%)
clang %CXXFLAGS% %DEFINES% %INC% -shared "%SRC_DLL%" -o "%OUT_DLL%" -L%SYSTEM_LIBS_LOCATION_UM% -L%SYSTEM_LIBS_LOCATION_UCRT% -L%MSVC_LIBS_LOCATION% %LIBS% %LDFLAGS_DLL%
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" (
    echo Build failed DLL with error %ERR%.
    endlocal & exit /b %ERR%
)

REM Rename temp DLL to final name
move /Y "%OUT_DLL%" "%OUT_DIR%\game.dll" >nul
if not "%ERRORLEVEL%"=="0" (
    echo Failed to rename DLL.
    endlocal & exit /b 1
)

echo Build succeeded (%MODE%).
endlocal & exit /b 0