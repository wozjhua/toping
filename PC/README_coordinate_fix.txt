坐标修复说明：
1. PcUinputMirrorController 新增 SetMirrorFrameViewport()。
2. d3d11_render_present.cpp 每帧把 D3D 实际 frameVp 同步给 pcUinput_。
3. 鼠标窗口坐标先按 D3D frameVp 转成投屏帧坐标，再按 Android 真实屏幕比例计算图像内部黑边。
4. 默认 Android 真实屏幕为 Y700 横屏 3040x1904。
5. 其他设备可在运行前设置：set HUILANG_ANDROID_SCREEN=宽x高，例如 set HUILANG_ANDROID_SCREEN=3040x1904
6. pc_input_mirror_window.cpp 现在只保留兼容空文件，实际输入逻辑在 pc_uinput_mirror_controller.cpp。
