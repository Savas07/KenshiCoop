@echo off
REM Build tools\devkit\replay\bin\kcrecdump.exe + kcreplay.exe (.kcrec packet
REM recording dump / replay; see README.md). Same v100 (VC++ 2010) x64
REM toolchain as the plugin. Override the toolchain with the VS10 / SDK
REM environment variables, e.g.
REM   set VS10=C:\Users\savas\Documents\kc_tc\vs10
REM   set SDK=C:\Users\savas\Documents\kc_tc\sdk71
setlocal

set "HERE=%~dp0"
pushd "%HERE%..\..\.." >nul
set "REPO=%CD%"
popd >nul

if not defined VS10 set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
if not defined SDK  set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"
set "VC=%VS10%\VC"
if not exist "%VC%\bin\amd64\cl.exe" (
    echo build.cmd: no x64 cl.exe under "%VC%\bin\amd64" - set VS10 to the Visual Studio 10.0 root
    exit /b 1
)

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

set "SRC=%REPO%\tools\devkit\replay"
set "BIN=%SRC%\bin"
set "OBJ=%BIN%\obj"
set "ENET=%REPO%\third_party\enet\enet"
if not exist "%OBJ%\enet" mkdir "%OBJ%\enet"

set CFLAGS=/nologo /O2 /EHsc /W3 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /I"%REPO%\src\netproto" /I"%ENET%\include"

echo === enet (static objects) ===
cl.exe %CFLAGS% /c /Fo"%OBJ%\enet\\" ^
    "%ENET%\callbacks.c" "%ENET%\compress.c" "%ENET%\host.c" "%ENET%\list.c" ^
    "%ENET%\packet.c" "%ENET%\peer.c" "%ENET%\protocol.c" "%ENET%\win32.c"
if errorlevel 1 goto fail

echo === kcrecdump.exe ===
cl.exe %CFLAGS% /Fo"%OBJ%\\" /Fe"%BIN%\kcrecdump.exe" "%SRC%\kcrecdump.cpp"
if errorlevel 1 goto fail

echo === kcreplay.exe ===
cl.exe %CFLAGS% /Fo"%OBJ%\\" /Fe"%BIN%\kcreplay.exe" "%SRC%\kcreplay.cpp" ^
    "%OBJ%\enet\*.obj" ws2_32.lib winmm.lib
if errorlevel 1 goto fail

echo built: %BIN%\kcrecdump.exe %BIN%\kcreplay.exe
exit /b 0

:fail
echo devkit replay build FAILED
exit /b 1
