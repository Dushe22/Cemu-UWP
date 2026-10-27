@echo off
rem Isolated tests that need no full build: record ring (one and two stages), pipeline prewarm policy, HLSL pruner.
rem Uses the MSVC environment if it is already set (CI), otherwise the local build tools.
setlocal
if defined VCINSTALLDIR goto :have_msvc
set "PATH=C:\Windows\System32;C:\Windows;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
if not defined VCVARS for /f "usebackq delims=" %%i in (`vswhere -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
:have_msvc
set "SRC=%~dp0..\..\src"
set "OUT=%TEMP%\cemu_tests"
if not exist "%OUT%" mkdir "%OUT%"
set FAILED=0
call :run record_ring\ring_test.cpp ring_test
call :run record_ring\two_stage_test.cpp two_stage_test
call :run prewarm_policy\policy_test.cpp policy_test
call "%~dp0hlsl_pruner\run.bat" >nul
if errorlevel 1 (echo FAIL hlsl_pruner & set FAILED=1) else echo ok   hlsl_pruner
exit /b %FAILED%

:run
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /I "%SRC%" "%~dp0%1" /Fo"%OUT%\\" /Fe"%OUT%\%2.exe" >"%OUT%\%2.log" 2>&1
if errorlevel 1 (echo FAIL %2: does not compile, see %OUT%\%2.log & set FAILED=1 & exit /b 0)
"%OUT%\%2.exe" >"%OUT%\%2.out" 2>&1
if errorlevel 1 (echo FAIL %2 & type "%OUT%\%2.out" & set FAILED=1) else echo ok   %2
exit /b 0
