#include "d3d11_renderer.h"

// togglePerfLogRecording
void D3D11Renderer::togglePerfLogRecording() {
       /* bool started = false;
        bool saved = false;
        std::wstring path = g_perfLog.toggle(started, saved);
        if (started) {
            updateStatusText(HLW(L"日志记录中：按数字0停止，最多10秒"));
        }
        else if (saved) {
            const size_t rows = g_perfLog.lastSavedDataRows();
            std::wstring msg = HLW(L"日志已写入：");
            msg += path.empty() ? HLW(L"当前目录") : path;
            msg += HLW(L" | 数据行 ");
            msg += std::to_wstring(static_cast<unsigned long long>(rows));
            updateStatusText(msg);
        }*/
    }

// checkPerfLogAutoStop
void D3D11Renderer::checkPerfLogAutoStop() {
      /*  std::wstring path;
        if (g_perfLog.stopIfExpired(path)) {
            const size_t rows = g_perfLog.lastSavedDataRows();
            std::wstring msg = HLW(L"日志自动停止(10秒)，已写入：");
            msg += path.empty() ? HLW(L"当前目录") : path;
            msg += HLW(L" | 数据行 ");
            msg += std::to_wstring(static_cast<unsigned long long>(rows));
            updateStatusText(msg);
        }*/
    }

