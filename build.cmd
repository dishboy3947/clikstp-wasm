@echo off
rem Builds ClikstpBridge.wasm with the MSFS 2024 SDK's own clang and wasm-ld, then stages it in
rem PackageSources\modules\ for the sim's Project Editor (open ClikstpBridge.xml in Dev Mode, Build All).
rem
rem   build.cmd                 compile and link into obj\modules\, then stage
rem
rem The flags mirror the SDK's Visual Studio settings (WASM\vs\2022\Microsoft.Cpp.MSFS.Common.props/.targets):
rem compile with clang, then link with the SDK's wasm-ld directly, adding its version library and exports.
rem The SDK installer sets MSFS2024_SDK; otherwise C:\MSFS 2024 SDK\ is assumed.

setlocal
set SDK=%MSFS2024_SDK%
if "%SDK%"=="" set SDK=C:\MSFS 2024 SDK\
if not "%SDK:~-1%"=="\" set SDK=%SDK%\

set BIN=%SDK%WASM\llvm\bin\
set SYSROOT=%SDK%WASM\wasi-sysroot
set LIBDIR=%SYSROOT%\lib\wasm32-wasi
set VERSIONS=%SDK%WASM\WasmVersions\MSFS_WasmVersions.a

rem The SDK ships clang-cl.exe (the Visual C++-style front end). The same program behaves like clang++ when given
rem --driver-mode=g++, which is what the compile flags below are written for.
set MODE=
if exist "%BIN%clang++.exe" (
  set CLANG=%BIN%clang++.exe
) else if exist "%BIN%clang.exe" (
  set CLANG=%BIN%clang.exe
  set MODE=--driver-mode=g++
) else if exist "%BIN%clang-cl.exe" (
  set CLANG=%BIN%clang-cl.exe
  set MODE=--driver-mode=g++
) else (
  echo Could not find clang in "%BIN%".
  echo Set MSFS2024_SDK to your MSFS 2024 SDK folder and try again.
  exit /b 1
)
set LINKER=%BIN%wasm-ld.exe
for %%f in ("%LINKER%" "%LIBDIR%\libclang_rt.builtins-wasm32.a" "%VERSIONS%") do (
  if not exist %%f (
    echo Missing %%f. Is the MSFS 2024 SDK fully installed?
    exit /b 1
  )
)

set HERE=%~dp0
set OBJ=%HERE%obj
set OUT=%OBJ%
if not exist "%OUT%\modules" mkdir "%OUT%\modules"

rem Two sources: BridgeCore.cpp (the logic, also built and tested natively by tests\run-tests) and
rem ClikstpBridge.cpp (the MSFS calls). Same flags for both.
set CFLAGS=--target=wasm32-unknown-wasi --sysroot="%SYSROOT%" -c ^
  -std=c++17 -O2 -fstack-size-section -mbulk-memory -fvisibility=hidden ^
  -ffunction-sections -fdata-sections -fno-stack-protector -fno-exceptions -fms-extensions -fwritable-strings ^
  -Werror=return-type -Wno-ignored-attributes -Wno-unused-command-line-argument ^
  -D__wasi__ -D_STRING_H_CPLUSPLUS_98_CONFORMANCE_ -D_WCHAR_H_CPLUSPLUS_98_CONFORMANCE_ -D_LIBCPP_HAS_NO_THREADS -D_WINDLL -D_MBCS ^
  -I"%SDK%WASM\include" -I"%SDK%SimConnect SDK\include"

for %%s in (BridgeCore ClikstpBridge) do (
  echo Compiling %%s.cpp with %CLANG% %MODE%
  "%CLANG%" %MODE% %CFLAGS% "%HERE%src\%%s.cpp" -o "%OBJ%\%%s.o"
  if errorlevel 1 (
    echo Compile failed.
    exit /b 1
  )
)

echo Linking with %LINKER%
"%LINKER%" "%OBJ%\BridgeCore.o" "%OBJ%\ClikstpBridge.o" -o "%OUT%\modules\ClikstpBridge.wasm" ^
  --no-entry --stack-guard-page --allow-undefined --export-dynamic --export-table ^
  --export malloc --export free --export __wasm_call_ctors ^
  --export mallinfo --export mchunkit_begin --export mchunkit_next --export get_pages_state --export mark_decommit_pages ^
  -L "%LIBDIR%" -lc++ -lc++abi -lc "%LIBDIR%\libclang_rt.builtins-wasm32.a" -lc "%VERSIONS%" ^
  --export GetSimConnectVersion --export GetExtensionVersion --export GetExtensionVersionBuffer ^
  --strip-debug --gc-sections -O3
if errorlevel 1 (
  echo Link failed.
  exit /b 1
)

rem Stage the module for the SDK package project (ClikstpBridge.xml): the supported way to package it. Open the
rem project in Dev Mode's Project Editor and Build All; the sim builds Packages\clikstp-bridge and loads it.
if not exist "%HERE%PackageSources\modules" mkdir "%HERE%PackageSources\modules"
copy /y "%OUT%\modules\ClikstpBridge.wasm" "%HERE%PackageSources\modules\ClikstpBridge.wasm" >nul
echo Staged PackageSources\modules\ClikstpBridge.wasm. Now Build All in the sim's Project Editor (README: Package).
endlocal
