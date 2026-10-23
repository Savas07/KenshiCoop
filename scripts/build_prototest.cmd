@echo off
REM Build dist\prototest.exe - the asserting unit layer for the wire protocol,
REM content hash and interpolation buffer (src\prototest). Uses the same v100
REM (VC++ 2010) x64 compiler as the plugin so the packed-struct layout under
REM test is EXACTLY the layout the shipped DLL compiles (that is the point:
REM prototest locks the wire contract of this toolchain).
REM
REM No game/KenshiLib/ENet dependencies - just the CRT.
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

REM Toolchain: the standard install paths, else the portable copy beside the
REM repo (..\kc_tc\vs10 and ..\kc_tc\sdk71, what this PC has), and
REM KENSHICOOP_VS10 / KENSHICOOP_SDK71 override both.
set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
if not exist "%VS10%\VC\bin\amd64\cl.exe" set "VS10=%REPO%\..\kc_tc\vs10"
if defined KENSHICOOP_VS10 set "VS10=%KENSHICOOP_VS10%"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"
if not exist "%SDK%\Include\windows.h" set "SDK=%REPO%\..\kc_tc\sdk71"
if defined KENSHICOOP_SDK71 set "SDK=%KENSHICOOP_SDK71%"
set "VC=%VS10%\VC"
if not exist "%VC%\bin\amd64\cl.exe" (
    echo prototest build FAILED: no v100 x64 cl.exe under "%VS10%" - set KENSHICOOP_VS10
    exit /b 1
)

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\prototest" mkdir "%REPO%\build\prototest"

echo === Building prototest.exe (Release^|x64, v100) ===
REM KENSHICOOP_PROTOTEST keeps SaveXfer.cpp CRT-only (its NetLink/engine-coupled
REM sender + quiescence watch are #ifdef'd out) so the real save-transfer RECEIVER
REM (onSaveBegin/onSaveFile/onSaveDone -> stage/verify/commit) can be exercised
REM end-to-end here without pulling in ENet/KenshiLib.
cl.exe /nologo /O2 /EHsc /W3 /D KENSHICOOP_PROTOTEST ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\prototest.exe" ^
    "%REPO%\src\prototest\main.cpp" ^
    "%REPO%\src\plugin\sync\Interp.cpp" ^
    "%REPO%\src\plugin\sync\SaveXfer.cpp"
if errorlevel 1 (
    echo prototest build FAILED
    exit /b 1
)
echo prototest built: %REPO%\dist\prototest.exe
exit /b 0
