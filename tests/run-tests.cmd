@echo off
rem Builds and runs the bridge's desktop tests with AddressSanitizer, using Visual Studio's compiler.
rem Run it from a "Developer Command Prompt for VS 2022" (Start menu), so cl.exe is on the path:
rem   wasm\ClikstpBridge\tests\run-tests.cmd
rem Fuzzing needs clang; on Windows the easiest route is WSL: wsl sh tests/run-tests.sh fuzz
setlocal
set HERE=%~dp0
set ROOT=%HERE%..
set OUT=%ROOT%\obj\tests
if not exist "%OUT%" mkdir "%OUT%"

where cl >nul 2>nul
if errorlevel 1 (
  echo cl.exe isn't on the path. Open a "Developer Command Prompt for VS 2022" and run this again.
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /Zi /fsanitize=address /Fo"%OUT%\\" /Fd"%OUT%\\" ^
  "%HERE%BridgeTests.cpp" "%ROOT%\src\BridgeCore.cpp" /Fe"%OUT%\bridge-tests.exe"
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
"%OUT%\bridge-tests.exe"
