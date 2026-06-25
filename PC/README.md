# D3D11 GPU Enhance Patch

改动目标：PC 端 D3D11 显示链路增加一个低延迟单 pass 放大锐化 Pixel Shader。

替换/合并以下文件：
- d3d11_renderer.h
- d3d11_device.cpp
- d3d11_cleanup.cpp
- d3d11_render_present.cpp

默认参数：
- enableGpuEnhance_ = true
- gpuEnhanceSharpness_ = 0.42f

如果画面出现白边、压缩块被放大、文字边缘发脏，把 d3d11_renderer.h 里的
gpuEnhanceSharpness_ 调低到 0.28f ~ 0.35f。

如果想关闭增强，把 enableGpuEnhance_ 改成 false。

注意：
- 这不是 DLSS，而是低延迟空间锐化/增强方案。
- HUD、映射层、ROI 仍走原来的 ps_，不会被一起锐化。
- 我这里无法在 Linux 沙箱里编译 Windows D3D11 项目，请在 Visual Studio/Windows SDK 环境编译验证。
