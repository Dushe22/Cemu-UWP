@echo off
rem Checks that D3D12HlslPruner keeps DXIL byte-identical and reports compile times.
rem Usage: run.bat   (desktop toolchain, Windows SDK 10.0.26100 DXC)
setlocal
if defined VCINSTALLDIR goto :have_msvc
set "PATH=C:\Windows\System32;C:\Windows;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
if not defined VCVARS for /f "usebackq delims=" %%i in (`vswhere -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
:have_msvc
set "OUT=%TEMP%\cemu_hlsl_pruner_test"
if not exist "%OUT%" mkdir "%OUT%"
set "SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
cl /nologo /O2 /EHsc /std:c++20 /I "%~dp0..\..\..\src" "%~dp0ptest.cpp" "%~dp0..\..\..\src\Cafe\HW\Latte\Renderer\D3D12\D3D12HlslPruner.cpp" dxcompiler.lib /Fo"%OUT%\\" /Fe"%OUT%\ptest.exe" >nul || exit /b 1
copy /y "%SDKBIN%\dxcompiler.dll" "%OUT%" >nul
copy /y "%SDKBIN%\dxil.dll" "%OUT%" >nul
pushd "%~dp0"
"%OUT%\ptest.exe" tricky.hlsl gs.hlsl levels.hlsl real.hlsl real_macros.hlsl
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%
