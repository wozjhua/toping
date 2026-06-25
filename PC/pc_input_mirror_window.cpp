#include "pc_uinput_mirror_controller.h"

// 这个工程当前实际使用 PcUinputMirrorController 处理投屏窗口输入。
// 坐标换算已经移动到 pc_uinput_mirror_controller.cpp：
//   - D3D viewport 外层黑边：由 d3d11_render_present.cpp 每帧同步 SetMirrorFrameViewport()
//   - Android 录屏内部黑边：由 Android 真实屏幕比例和当前投屏帧比例计算
// 保留这个空文件只是为了兼容旧工程里仍然引用 pc_input_mirror_window.cpp 的构建配置。
