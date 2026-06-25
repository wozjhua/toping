# d3d11_native 第一阶段拆分

这个包把原来的 `d3d11_native.cpp` 拆成了头文件 + 多个功能 cpp。

## 文件职责

- `d3d11_renderer.h`：`D3D11Renderer` 类声明、成员变量、配置结构、函数声明。
- `d3d11_native.cpp`：启动参数、ADB 初始选择、`wWinMain` 主入口。
- `d3d11_renderer_lifecycle.cpp`：构造/析构、初始化、主循环、窗口创建、触摸发送基础封装。
- `d3d11_render_core.cpp`：D3D11 设备、纹理上传、HUD、渲染、清理。
- `d3d11_input_dispatch.cpp`：主窗口消息分发。
- `d3d11_settings_window.cpp`：设置窗口，已经吸收你上传的 `settings_window.inl`，后续不需要再 include 这个 inl。
- `d3d11_mapping_callbacks.cpp`：PC 映射 runtime 与 pc_uinput 的回调绑定。
- `d3d11_mapping_toolbar_bridge.cpp`：映射工具栏、状态文本、方案入口。
- `d3d11_mapping_profile.cpp`：映射方案保存/加载、默认方案、宏和运行参数随方案持久化。
- `d3d11_mapping_edit.cpp`：映射创建、选择、拖动、编辑、删除。
- `d3d11_mapping_utils.cpp`：映射序列化、按键选项、坐标换算等辅助函数。
- `d3d11_pc_runtime.cpp`：视角、摇杆、菜单、宏运行时辅助逻辑。
- `d3d11_perf.cpp`：性能日志开关和自动停止。

## 接入方式

1. 先备份当前 `d3d11_native.cpp`。
2. 把这些 `.h/.cpp` 文件加入同一个 VS 工程。
3. 旧的 `d3d11_native.cpp` 替换为这里的新版 `d3d11_native.cpp`。
4. 不再在主文件里 include `d3d11_render_core.inl` / `settings_window.inl`。
5. 若编译器报某个函数重复定义，确认旧 `.inl` 没有仍被 include。

这是第一阶段拆分：先把单文件拆开，尽量不改业务逻辑。第二阶段可以继续把 `d3d11_mapping_edit.cpp` 内的创建逻辑再拆成 key / lock / compass / menu / macro。

## 2026-05-19 修正

- `RuntimeSettings` 默认值已从 `d3d11_renderer.h` 内联初始化移动到 `d3d11_renderer_lifecycle.cpp`，避免头文件直接依赖 `VideoStreamTuning::...` 和 `DEFAULT_AUDIO_VOLUME_PERCENT` 时产生顺序问题。
- 新增 `build_msvc_x64_split_no_rc.bat`。拆分后不能再只编译 `d3d11_native.cpp`，必须把拆出的 `d3d11_renderer_lifecycle.cpp`、`d3d11_render_core.cpp`、`d3d11_settings_window.cpp` 等一起交给 `cl`。


## Stage 2 split

`d3d11_mapping_edit.cpp` and `d3d11_pc_runtime.cpp` have been split into feature-oriented files:

- `d3d11_mapping_mode.cpp`: edit mode enter/exit state.
- `d3d11_mapping_create.cpp`: create-entry functions and click-to-create flow.
- `d3d11_mapping_key.cpp`: normal key binding options, rebind, delete, slot allocation.
- `d3d11_mapping_overlay_edit.cpp`: overlay hit-test/edit dispatch.
- `d3d11_mapping_lock.cpp`: lock-view editing, lock options, lock debug counters.
- `d3d11_mapping_menu.cpp`: radial/item/bag menu editing and menu debug counters.
- `d3d11_mapping_macro.cpp`: normal macro editing helpers.
- `d3d11_mapping_compass.cpp`: WASD compass editing, radii, anchors, sector handles.

Build with `build_msvc_x64_split_no_rc.bat`.

## Stage 3 settings split

`d3d11_settings_window.cpp` has been split into smaller settings modules:

- `d3d11_settings_window.cpp`: settings window entry point, apply/reset, WndProc, show/hide orchestration.
- `d3d11_settings_hotkeys.cpp`: hotkey parsing, hotkey edit subclass proc, timer-based capture.
- `d3d11_settings_controls.cpp`: Win32 control creation, visibility/enabled helpers, layout, fill controls.
- `d3d11_settings_stream.cpp`: HUD/audio labels, FPS/capture/fullscreen controls, runtime setting sync to Android.
- `d3d11_settings_weights.cpp`: split weight defaults, slider readback, weight apply/reset.
- `d3d11_settings_video_roi.cpp`: video preset/quality, JPEG subsampling, ROI controls and apply logic.
- `d3d11_settings_adb_wifi.cpp`: USB/WiFi ADB mode switch, IP detection, endpoint normalization, reconnect.

`debugTextWidth()` has been moved next to the other debug text helpers in `d3d11_render_core.cpp`.

The build script has been updated to compile all new settings cpp files.


## Stage 4 render outer split

从 `d3d11_render_core.cpp` 继续拆出外围渲染逻辑：

- `d3d11_debug_draw.cpp`：F4/ROI/H264 诊断覆盖层的 CPU BGRA 绘制辅助。
- `d3d11_hud_render.cpp`：HUD 纹理创建、文字合成、HUD quad 绘制。
- `d3d11_mapping_overlay_render.cpp`：PC 映射 Overlay 纹理更新与绘制。

本阶段不拆 `createDevice`、swapchain、frame upload、`render()`、Present 主路径。

## Stage 5 device / cleanup split

继续从 `d3d11_render_core.cpp` 拆出边界清楚的设备和清理逻辑：

- `d3d11_device.cpp`：D3D11 device、swapchain、RTV 重建、shader/input-layout、sampler、窗口按首帧尺寸适配。
- `d3d11_cleanup.cpp`：工具栏/runtime 停止、设置窗口资源释放、D3D11/纹理/vertex buffer/swapchain/device 释放。
- `d3d11_render_core.cpp`：保留帧纹理上传、strict sync、consumeLatestFrame、center ROI、updateVertices、render/Present 主路径。

本阶段仍不拆 `render()` 和 `consumeLatestFrame()`，避免一次性触碰主渲染路径。


## Stage 6

进一步从 `d3d11_render_core.cpp` 拆出帧上传与中心 ROI 纹理逻辑：

- `d3d11_frame_upload.cpp`: `ensureFrameTexture`, `uploadFrameTextureDynamic`, strict hybrid sync queue, `consumeLatestFrame`, center ROI texture and vertices, upload mode text.

`d3d11_render_core.cpp` 现在主要保留 frame quad、`render()`、Present 统计与少量 HUD 辅助文本。


## Stage7 render present split

- Added `d3d11_render_present.cpp` for `updateVertices()`, `render()`, and present interval statistics.
- `d3d11_render_core.cpp` now keeps only small shared render/HUD helper functions.
- Build script includes `d3d11_render_present.cpp`.


## Stage8 remove render core shell

- Removed `d3d11_render_core.cpp` from the build.
- Moved HUD-only helpers (`centerVideoDecoderModeText`, `actualCpuCoreCountForHud`) into `d3d11_hud_render.cpp`.
- Moved strict-sync/frame-upload helpers (`absI64`, `strictSyncToleranceNs`) into `d3d11_frame_upload.cpp`.
- After this stage, the historical `d3d11_render_core.cpp` file can be deleted from the project tree.
