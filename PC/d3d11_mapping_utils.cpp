#include "d3d11_renderer.h"

// intListToStringLocal
std::string D3D11Renderer::intListToStringLocal(const std::vector<int>& values) {
        if (values.empty()) return "-";
        std::string out;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out += ",";
            out += std::to_string(values[i]);
        }
        return out;
    }

// intListFromStringLocal
std::vector<int> D3D11Renderer::intListFromStringLocal(const std::string& text) {
        std::vector<int> out;
        if (text.empty() || text == "-") return out;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t next = text.find(',', pos);
            std::string item = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            if (!item.empty()) {
                char* endp = nullptr;
                long v = std::strtol(item.c_str(), &endp, 10);
                if (endp && *endp == '\0') out.push_back(static_cast<int>(v));
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        return out;
    }

// wideToHexLocal
std::string D3D11Renderer::wideToHexLocal(const std::wstring& text) {
        if (text.empty()) return "-";
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size() * 4);
        for (wchar_t wc : text) {
            unsigned int v = static_cast<unsigned int>(wc) & 0xFFFFu;
            out.push_back(hex[(v >> 12) & 0xF]);
            out.push_back(hex[(v >> 8) & 0xF]);
            out.push_back(hex[(v >> 4) & 0xF]);
            out.push_back(hex[v & 0xF]);
        }
        return out;
    }

// hexNibbleLocal
int D3D11Renderer::hexNibbleLocal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

// wideFromHexLocal
std::wstring D3D11Renderer::wideFromHexLocal(const std::string& text) {
        std::wstring out;
        if (text.empty() || text == "-") return out;
        for (size_t i = 0; i + 3 < text.size(); i += 4) {
            int a = hexNibbleLocal(text[i]);
            int b = hexNibbleLocal(text[i + 1]);
            int c = hexNibbleLocal(text[i + 2]);
            int d = hexNibbleLocal(text[i + 3]);
            if (a < 0 || b < 0 || c < 0 || d < 0) break;
            unsigned int v = static_cast<unsigned int>((a << 12) | (b << 8) | (c << 4) | d);
            out.push_back(static_cast<wchar_t>(v));
        }
        return out;
    }

// clampIntLocal
int D3D11Renderer::clampIntLocal(int v, int lo, int hi) {
        return (std::max)(lo, (std::min)(hi, v));
    }

// applyNormalKeyOptionsResult
void D3D11Renderer::applyNormalKeyOptionsResult(size_t index, const PcNormalKeyOptions& opt) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.bindings.size()) return;
        auto& b = profile.bindings[index];
        b.touchMode = opt.touchMode;
        b.specialAction = opt.specialAction;
        b.radiusNorm = PcMappingProfile::ClampButtonRadiusNorm(opt.radiusNorm);
        b.randomRadiusNorm = (std::max)(0, (std::min)(b.radiusNorm, opt.randomRadiusNorm));
        b.randomMoveRadiusNorm = (std::max)(0, (std::min)(b.radiusNorm, opt.randomMoveRadiusNorm));
        b.randomMoveIntervalMs = (std::max)(8, (std::min)(200, opt.randomMoveIntervalMs));
        clampBindingToBounds(b);
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        selectedMappingBindingIndex_ = static_cast<int>(index);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"普通按键选项已更新");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// pxMarginToNorm
int D3D11Renderer::pxMarginToNorm(int marginPx, int sizePx) {
        const int s = (std::max)(1, sizePx);
        if (s <= 1) return 0;
        const int m = (std::max)(0, (std::min)(marginPx, s / 2));
        return static_cast<int>((static_cast<long long>(m) * 1000000LL + (s - 1) / 2) / (s - 1));
    }

// updateSelectedBindingPositionFromPoint
bool D3D11Renderer::updateSelectedBindingPositionFromPoint(int x, int y) {
        if (selectedMappingBindingIndex_ < 0) return false;
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        const size_t index = static_cast<size_t>(selectedMappingBindingIndex_);
        if (index >= profile.bindings.size()) return false;
        auto b = mappingBindingDrag_ ? mappingBindingDragStart_ : profile.bindings[index];
        if (mappingBindingDrag_) {
            const int dx = nx - mappingBindingDragStartXNorm_;
            const int dy = ny - mappingBindingDragStartYNorm_;
            b.xNorm = PcMappingProfile::ClampNorm(mappingBindingDragStart_.xNorm + dx);
            b.yNorm = PcMappingProfile::ClampNorm(mappingBindingDragStart_.yNorm + dy);
        }
        else {
            b.xNorm = nx;
            b.yNorm = ny;
        }
        clampBindingToBounds(b);
        profile.bindings[index] = b;
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"按键坐标：拖动更新中");
        refreshMappingToolbarStatus();
        return true;
    }

