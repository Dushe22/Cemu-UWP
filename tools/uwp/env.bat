@echo off
rem Shared setup for the UWP scripts: MSVC with the Store runtime, plus cmake and ninja.
rem VCVARS can point to another vcvars64.bat (any VS 2022 install with the C++ and UWP workloads).
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\WindowsPowerShell\v1.0;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
if not defined VCVARS (
  for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
rem "store" links VCRUNTIME140_APP.dll; the desktop runtime closes the app at launch on Xbox
call "%VCVARS%" store >nul || (echo vcvars64.bat not found, set VCVARS & exit /b 1)
cd /d "%~dp0..\.."
rem cmake and ninja from vcpkg's tool downloads when they are not on PATH
for /d %%d in ("dependencies\vcpkg\downloads\tools\ninja-*") do set "PATH=%%~fd;%PATH%"
for /d %%d in ("dependencies\vcpkg\downloads\tools\cmake-*") do for /d %%e in ("%%d\cmake-*") do set "PATH=%%~fe\bin;%PATH%"
exit /b 0
