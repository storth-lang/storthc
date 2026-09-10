@echo off
setlocal enabledelayedexpansion

set "BUILD_DIR=build"
set "OBJ_DIR=%BUILD_DIR%\obj"

set "VERSION_MAJOR=0"
set "VERSION_MINOR=1"
set "VERSION_PATCH=0"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

set "GIT_HASH=unknown"
for /f "delims=" %%h in ('git rev-parse --short HEAD 2^>nul') do set "GIT_HASH=%%h"
git diff --quiet 2>nul
if errorlevel 1 set "GIT_HASH=%GIT_HASH%-dirty"

set "SRC_FILES="
for /f "delims=" %%i in ('dir "src\*.c" /s /b') do (
    set "filename=%%~ni"
    if "!filename:_linux=!"=="!filename!" (
        set "SRC_FILES=!SRC_FILES! "%%i""
    )
)

echo Compiling project (%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH% %GIT_HASH%)...
cl /nologo /std:c11 /F 8388608 /I"src" /D"WIN32_LEAN_AND_MEAN" ^
   /DSTORTHC_VERSION_MAJOR=%VERSION_MAJOR% ^
   /DSTORTHC_VERSION_MINOR=%VERSION_MINOR% ^
   /DSTORTHC_VERSION_PATCH=%VERSION_PATCH% ^
   /DSTORTHC_GIT_HASH=\"%GIT_HASH%\" ^
   /Fo"%OBJ_DIR%/" /Fe:"%BUILD_DIR%/storthc.exe" !SRC_FILES!

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build successful! Created %BUILD_DIR%\storthc.exe
