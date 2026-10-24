@echo off
rem builds bin\uwp\CemuUwp.exe; extra arguments go to cmake --build (e.g. --target <object>)
call "%~dp0env.bat" || exit /b 1
cmake --build out\uwp %*
