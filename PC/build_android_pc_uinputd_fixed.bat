@echo off
setlocal EnableExtensions

rem Build Android native executable: pc_uinputd
rem Usage:
rem   set NDK_ROOT=D:\SDK\ndk\27.0.12077973
rem   call .\build_android_pc_uinputd_fixed.bat

if "%ANDROID_API%"=="" set ANDROID_API=21
if "%ANDROID_ABI%"=="" set ANDROID_ABI=arm64-v8a

if "%NDK_ROOT%"=="" (
  if exist "D:\SDK\ndk\27.0.12077973" set NDK_ROOT=D:\SDK\ndk\27.0.12077973
)
if "%NDK_ROOT%"=="" (
  if exist "%LOCALAPPDATA%\Android\Sdk\ndk" (
    for /f "delims=" %%D in ('dir /b /ad /o-n "%LOCALAPPDATA%\Android\Sdk\ndk" 2^>nul') do (
      if "%NDK_ROOT%"=="" set NDK_ROOT=%LOCALAPPDATA%\Android\Sdk\ndk\%%D
    )
  )
)

if "%NDK_ROOT%"=="" (
  echo NDK_ROOT is not set.
  echo Example: set NDK_ROOT=D:\SDK\ndk\27.0.12077973
  exit /b 1
)

set TOOLCHAIN=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin

if /i "%ANDROID_ABI%"=="arm64-v8a" (
  set CLANG=%TOOLCHAIN%\aarch64-linux-android%ANDROID_API%-clang++.cmd
) else if /i "%ANDROID_ABI%"=="armeabi-v7a" (
  set CLANG=%TOOLCHAIN%\armv7a-linux-androideabi%ANDROID_API%-clang++.cmd
) else if /i "%ANDROID_ABI%"=="x86_64" (
  set CLANG=%TOOLCHAIN%\x86_64-linux-android%ANDROID_API%-clang++.cmd
) else (
  echo Unsupported ANDROID_ABI=%ANDROID_ABI%
  exit /b 1
)

if not exist "%CLANG%" (
  echo Cannot find clang: %CLANG%
  echo.
  echo Current NDK_ROOT=%NDK_ROOT%
  echo Current ANDROID_API=%ANDROID_API%
  echo Current ANDROID_ABI=%ANDROID_ABI%
  echo.
  echo Available clang wrappers:
  dir /b "%TOOLCHAIN%\*android*-clang++.cmd" 2>nul
  exit /b 1
)

if not exist "pc_uinputd.cpp" (
  echo Missing: pc_uinputd.cpp
  exit /b 1
)
if not exist "pc_uinput_protocol.h" (
  echo Missing: pc_uinput_protocol.h
  exit /b 1
)

echo Using NDK_ROOT=%NDK_ROOT%
echo Using ANDROID_API=%ANDROID_API%
echo Using ANDROID_ABI=%ANDROID_ABI%
echo Using CLANG=%CLANG%

"%CLANG%" pc_uinputd.cpp -std=c++17 -O2 -Wall -Wextra -static-libstdc++ -o pc_uinputd
if errorlevel 1 exit /b 1

echo.
echo Build completed: pc_uinputd
endlocal
