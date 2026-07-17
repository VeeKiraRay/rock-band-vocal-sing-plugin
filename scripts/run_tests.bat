@echo off
setlocal

rem scripts\run_tests.bat — configure, build, and run RBVocalSingTests in one
rem command for fast local feedback. Plain batch (not PowerShell) so it runs with
rem a double-click or `scripts\run_tests.bat` with no -ExecutionPolicy prompt.

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%"

cmake -B build -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 goto :fail

cmake --build build --config Debug --target RBVocalSingTests
if errorlevel 1 goto :fail

"build\RBVocalSingTests_artefacts\Debug\RBVocalSingTests.exe"
set "TEST_EXIT=%ERRORLEVEL%"
popd
exit /b %TEST_EXIT%

:fail
popd
exit /b 1
