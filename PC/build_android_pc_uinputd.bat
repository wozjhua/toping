@echo off
setlocal

if "%NDK_ROOT%"=="" (
  echo Please set NDK_ROOT to your Android NDK path, for example:
  echo   set NDK_ROOT=D:\Android\Sdk\ndk\26.3.11579264
  exit /b 1
)

set TOOLCHAIN=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin
set CXX=%TOOLCHAIN%\aarch64-linux-android29-clang++.cmd

if not exist "%CXX%" (
  echo Cannot find clang: %CXX%
  exit /b 1
)

"%CXX%" -std=c++17 -O2 -fPIE -pie -static-libstdc++ pc_uinputd.cpp -o pc_uinputd
if errorlevel 1 exit /b 1

echo Built pc_uinputd for arm64-v8a.
endlocal
