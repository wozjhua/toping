PC uinputd v1 integration notes
===============================

Goal:
  Android does not run the old mapping App / Shizuku input runtime anymore.
  PC pushes and starts pc_uinputd, then sends final touch actions.

Files:
  Android daemon:
    pc_uinputd.cpp
    pc_uinput_protocol.h
    build_android_pc_uinputd.bat
    CMakeLists.txt

  PC side:
    pc_uinput_protocol.h
    pc_uinput_client.h/.cpp
    pc_uinput_mirror_controller.h/.cpp
    build_msvc_x64_onefile_no_rc_add_pc_uinput.bat

Build Android daemon:
  set NDK_ROOT=D:\Android\Sdk\ndk\<your_ndk_version>
  call build_android_pc_uinputd.bat

Put the output file next to the PC EXE:
  pc_uinputd
  d3d11_native_receiver_onefile.exe
  adb.exe

Minimal d3d11_native.cpp integration:

1) Add include:
    #include "pc_uinput_mirror_controller.h"

2) Add member near PcInputBridge or replace it for the v2 path:
    PcUinputMirrorController pcUinput_;

3) In the mirror window message handler, forward messages:
    {
        LRESULT r = 0;
        if (pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, &r)) return r;
    }

4) Start automatically after the main window is created or after screen mirror connection is ready:
    PcUinputMirrorController::Config cfg;
    cfg.client.daemonLocalPath = "pc_uinputd";
    cfg.client.adbPort = 18889;
    cfg.client.socketName = "huilang_pc_uinput";
    cfg.client.rotation = 0; // test 0 first; if touch is rotated, try 1/2/3
    pcUinput_.Start(hwnd_, cfg);

   For production, run this on a background thread so adb push/start does not block the UI.

5) Stop on cleanup:
    pcUinput_.Stop();

Current MVP behavior:
  WM_LBUTTONDOWN -> TOUCH_DOWN slot 0
  WM_MOUSEMOVE while down -> TOUCH_MOVE slot 0
  WM_LBUTTONUP -> TOUCH_UP slot 0
  WM_MOUSEWHEEL -> WHEEL
  Keyboard down/up -> simple Linux key events

Device compatibility:
  pc_uinputd scans /dev/input/event0..127 and selects the best DIRECT touch device.
  It copies the raw ABS range into the virtual uinput device. This is the important part for different Android devices.

Debug on Android:
  adb shell cat /data/local/tmp/pc_uinputd.log
  adb shell ps -A | grep pc_uinputd
