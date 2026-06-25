@echo off
setlocal

if "%LIBJPEG_TURBO_DIR%"=="" (
  echo Please set LIBJPEG_TURBO_DIR first.
  exit /b 1
)
if "%FFMPEG_DIR%"=="" (
  echo Please set FFMPEG_DIR first.
  exit /b 1
)

if not exist "d3d11_renderer.h" (
  echo Missing: d3d11_renderer.h
  exit /b 1
)

if not exist "d3d11_native.cpp" (
  echo Missing: d3d11_native.cpp
  exit /b 1
)

if not exist "pc_uinputd" (
  echo Warning: pc_uinputd not found next to EXE. Build it first or copy it here.
)
if "%PC_LOCK_ENABLE_STATS%"=="" set PC_LOCK_ENABLE_STATS=0
echo PC_LOCK_ENABLE_STATS=%PC_LOCK_ENABLE_STATS%

cl /nologo /utf-8 /EHsc /std:c++17 /O2 /DPC_LOCK_ENABLE_STATS=%PC_LOCK_ENABLE_STATS% /DUNICODE /DWIN32_LEAN_AND_MEAN /DHUILANG_ENABLE_FFMPEG_D3D11VA=1 ^
  /I "%LIBJPEG_TURBO_DIR%\include" ^
  /I "%FFMPEG_DIR%\include" ^
  /Fe:d3d11_native_receiver_split.exe ^
  d3d11_native.cpp ^
  d3d11_renderer_lifecycle.cpp ^
  d3d11_device.cpp ^
  d3d11_render_present.cpp ^
  d3d11_frame_upload.cpp ^
  d3d11_cleanup.cpp ^
  d3d11_debug_draw.cpp ^
  d3d11_hud_render.cpp ^
  d3d11_mapping_overlay_render.cpp ^
  d3d11_input_dispatch.cpp ^
  d3d11_settings_window.cpp ^
  d3d11_settings_hotkeys.cpp ^
  d3d11_settings_controls.cpp ^
  d3d11_settings_stream.cpp ^
  d3d11_settings_weights.cpp ^
  d3d11_settings_video_roi.cpp ^
  d3d11_settings_adb_wifi.cpp ^
  d3d11_mapping_callbacks.cpp ^
  d3d11_mapping_toolbar_bridge.cpp ^
  d3d11_mapping_profile.cpp ^
  d3d11_mapping_mode.cpp ^
  d3d11_mapping_create.cpp ^
  d3d11_mapping_key.cpp ^
  d3d11_mapping_overlay_edit.cpp ^
  d3d11_mapping_lock.cpp ^
  d3d11_mapping_menu.cpp ^
  d3d11_mapping_macro.cpp ^
  d3d11_mapping_compass.cpp ^
  d3d11_mapping_utils.cpp ^
  d3d11_perf.cpp ^
  hl_common.cpp ^
  perf_logger.cpp ^
  hud_overlay.cpp ^
  mirror_net.cpp ^
  mirror_receiver.cpp ^
  audio_receiver.cpp ^
  center_video_receiver.cpp ^
  pc_mapping_profile.cpp ^
  pc_mapping_runtime.cpp ^
  pc_lock_runtime.cpp ^
  pc_compass_runtime.cpp ^
  pc_menu_runtime.cpp ^
  pc_macro_runtime.cpp ^
  pc_ui_theme.cpp ^
  pc_bind_dialog.cpp ^
  pc_mapping_overlay.cpp ^
  pc_normal_key_options_dialog.cpp ^
  pc_lock_options_dialog.cpp ^
  pc_compass_options_dialog.cpp ^
  pc_menu_create_dialog.cpp ^
  pc_menu_options_dialog.cpp ^
  pc_mapping_toolbar.cpp ^
  pc_uinput_client.cpp ^
  pc_uinput_mirror_controller.cpp ^
  /link /LIBPATH:"%LIBJPEG_TURBO_DIR%\lib" /LIBPATH:"%FFMPEG_DIR%\lib" ^
    turbojpeg-static.lib jpeg-static.lib ^
    ws2_32.lib winmm.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib ole32.lib ^
    mf.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib ^
    avcodec.lib avutil.lib swscale.lib

if errorlevel 1 exit /b 1

if exist "append_self_bundle.ps1" (
  powershell -NoProfile -ExecutionPolicy Bypass -File ".\append_self_bundle.ps1" -ExePath "d3d11_native_receiver_split.exe"
  if errorlevel 1 exit /b 1
)

echo.
echo Split build completed.
endlocal
