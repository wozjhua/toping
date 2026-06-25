#include "d3d11_renderer.h"

// resetMenuDebugDispatchCounters
void D3D11Renderer::resetMenuDebugDispatchCounters() {
        menuDebugRawMessages_ = 0;
        menuDebugMenuEntry_ = 0;
        menuDebugMenuAccepted_ = 0;
        menuDebugWindowActive_ = false;
    }

// resetMenuDebugTotalCounters
void D3D11Renderer::resetMenuDebugTotalCounters() {
        menuDebugTotalActiveMs_ = 0;
        menuDebugTotalRawMessages_ = 0;
        menuDebugTotalRawMouse_ = 0;
        menuDebugTotalApplyCalls_ = 0;
        menuDebugTotalCoalesceFlushes_ = 0;
        menuDebugTotalCoalesceForcedFlushes_ = 0;
        menuDebugTotalRawDx_ = 0;
        menuDebugTotalRawDy_ = 0;
    }

// resetMenuDebugAllCounters
void D3D11Renderer::resetMenuDebugAllCounters() {
        resetMenuDebugDispatchCounters();
        resetMenuDebugTotalCounters();
        menuRuntime_.SnapshotAndResetDebugStats();
    }

// markMenuDebugRawMessage
void D3D11Renderer::markMenuDebugRawMessage() {
        const auto now = std::chrono::steady_clock::now();
        if (!menuDebugWindowActive_) {
            menuDebugWindowActive_ = true;
            menuDebugWindowStart_ = now;
        }
        menuDebugLastInput_ = now;
        ++menuDebugRawMessages_;
    }

// maybeRefreshMenuDebugOverlay
void D3D11Renderer::maybeRefreshMenuDebugOverlay(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!menuDebugWindowActive_ || menuDebugRawMessages_ == 0) return;

        const long long activeMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(menuDebugLastInput_ - menuDebugWindowStart_).count()
            );
        const long long sinceFirstMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - menuDebugWindowStart_).count()
            );
        const long long idleMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - menuDebugLastInput_).count()
            );
        if (!force && sinceFirstMs < kMenuDebugOverlayRefreshMs && idleMs < kMenuDebugIdleFlushMs) return;

        const long long sampleMs = (std::max)(1LL, activeMs);
        const long long wallMs = (std::max)(1LL, sinceFirstMs);
        const uint64_t msgPerSec = static_cast<uint64_t>(
            (menuDebugRawMessages_ * 1000ULL + static_cast<uint64_t>(sampleMs / 2)) / static_cast<uint64_t>(sampleMs)
            );

        menuRuntime_.FlushPendingRawDelta(true);
        const auto st = menuRuntime_.SnapshotAndResetDebugStats();

        menuDebugTotalActiveMs_ += static_cast<uint64_t>(sampleMs);
        menuDebugTotalRawMessages_ += menuDebugRawMessages_;
        menuDebugTotalRawMouse_ += st.rawInputMouse;
        menuDebugTotalApplyCalls_ += st.applyCalls;
        menuDebugTotalCoalesceFlushes_ += st.coalesceFlushes;
        menuDebugTotalCoalesceForcedFlushes_ += st.coalesceForcedFlushes;
        menuDebugTotalRawDx_ += st.rawDx;
        menuDebugTotalRawDy_ += st.rawDy;

        const uint64_t totalMsgPerSec = menuDebugTotalActiveMs_ > 0
            ? static_cast<uint64_t>((menuDebugTotalRawMessages_ * 1000ULL + menuDebugTotalActiveMs_ / 2ULL) / menuDebugTotalActiveMs_)
            : 0ULL;
        const uint64_t savedApply = st.rawInputMouse > st.applyCalls ? (st.rawInputMouse - st.applyCalls) : 0ULL;
        const uint64_t totalSavedApply = menuDebugTotalRawMouse_ > menuDebugTotalApplyCalls_ ? (menuDebugTotalRawMouse_ - menuDebugTotalApplyCalls_) : 0ULL;

        std::wostringstream oss;
        oss << HLW(L"菜单诊断：") << (st.active ? HLW(L"激活") : HLW(L"未激活"))
            << HLW(L" / ") << (st.secondaryFree ? HLW(L"自由鼠标") : HLW(L"轮盘"))
            << HLW(L"\r\n本次采样=") << sampleMs << HLW(L"ms，窗口耗时≈") << wallMs << HLW(L"ms")
            << HLW(L"，窗口消息=") << menuDebugRawMessages_
            << HLW(L"，鼠标包=") << st.rawInputMouse
            << HLW(L"，估算=") << msgPerSec << HLW(L"次/秒");
        oss << HLW(L"\r\n本次位移：横向=") << st.rawDx << HLW(L"，纵向=") << st.rawDy;
        oss << HLW(L"\r\n本次读包：菜单入口=") << menuDebugMenuEntry_
            << HLW(L"，成功接收=") << menuDebugMenuAccepted_;
        oss << HLW(L"\r\n本次合并：周期=2ms，鼠标包=") << st.rawInputMouse
            << HLW(L"，菜单计算=") << st.applyCalls
            << HLW(L"，减少计算=") << savedApply
            << HLW(L"，强制尾包=") << st.coalesceForcedFlushes;
        oss << HLW(L"\r\n累计采样≈") << menuDebugTotalActiveMs_
            << HLW(L"ms，平均=") << totalMsgPerSec << HLW(L"次/秒");
        oss << HLW(L"\r\n累计数量：窗口消息=") << menuDebugTotalRawMessages_
            << HLW(L"，鼠标包=") << menuDebugTotalRawMouse_
            << HLW(L"，总位移=(") << menuDebugTotalRawDx_ << HLW(L",") << menuDebugTotalRawDy_ << HLW(L")");
        oss << HLW(L"\r\n累计合并：菜单计算=") << menuDebugTotalApplyCalls_
            << HLW(L"，减少计算=") << totalSavedApply
            << HLW(L"，强制尾包=") << menuDebugTotalCoalesceForcedFlushes_;
        oss << HLW(L"\r\n光标：(") << st.cursorXNorm << HLW(L",") << st.cursorYNorm << HLW(L")");

        mappingEditorStatus_ = oss.str();
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        resetMenuDebugDispatchCounters();
        lastMenuDebugOverlayUpdate_ = now;
    }

// menuSlotForCategory
int D3D11Renderer::menuSlotForCategory(PcMenuCategory cat) {
        if (cat == PcMenuCategory::MenuRadial) return 10;
        return 11;
    }

// isMenuFreeCursorCategoryLocal
bool D3D11Renderer::isMenuFreeCursorCategoryLocal(PcMenuCategory cat) {
        return cat == PcMenuCategory::MenuItemOperation ||
            cat == PcMenuCategory::MenuBagOperation ||
            cat == PcMenuCategory::MenuHorizontal ||
            cat == PcMenuCategory::MenuVertical;
    }

// clampMenuButtonRadiusNormLocal
int D3D11Renderer::clampMenuButtonRadiusNormLocal(int v) {
        return (std::max)(PC_MENU_BUTTON_RADIUS_MIN_NORM, (std::min)(PC_MENU_BUTTON_RADIUS_MAX_NORM, v));
    }

// menuAnchorPointToNormFromClient
bool D3D11Renderer::menuAnchorPointToNormFromClient(const PcMenuBinding& m, int x, int y, int& outX, int& outY) {
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
        const int r = clampMenuButtonRadiusNormLocal(m.radiusNorm);
        outX = (std::max)(r, (std::min)(1000000 - r, PcMappingProfile::ClampNorm(nx)));
        outY = (std::max)(r, (std::min)(1000000 - r, PcMappingProfile::ClampNorm(ny)));
        return true;
    }

// resetMenuRangeToDefault
void D3D11Renderer::resetMenuRangeToDefault(PcMenuBinding& m) {
        if (m.category == PcMenuCategory::MenuRadial) {
            const int r = clampMenuButtonRadiusNormLocal(m.radiusNorm);
            m.rangeLeftNorm = PcMappingProfile::ClampNorm(m.centerXNorm - r);
            m.rangeTopNorm = PcMappingProfile::ClampNorm(m.centerYNorm - r);
            m.rangeRightNorm = PcMappingProfile::ClampNorm(m.centerXNorm + r);
            m.rangeBottomNorm = PcMappingProfile::ClampNorm(m.centerYNorm + r);
        }
        else {
            m.rangeLeftNorm = 0;
            m.rangeTopNorm = 0;
            m.rangeRightNorm = 1000000;
            m.rangeBottomNorm = 1000000;
        }
    }

// rebindMenuBinding
bool D3D11Renderer::rebindMenuBinding(size_t index) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.menus.size()) return false;
        mappingEditorStatus_ = HLW(L"重绑菜单：请按新的键盘组合");
        updateStatusText(mappingEditorStatus_);
        PcCapturedKeyCombo combo;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || combo.vkCodes.empty() || combo.mouseButtonCode > 0) {
            mappingEditorStatus_ = HLW(L"重绑菜单已取消：暂只支持键盘组合");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        profile.menus[index].comboTriggerCodes = combo.vkCodes;
        profile.menus[index].triggerLabel = combo.label;
        menuRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        selectedMenuIndex_ = static_cast<int>(index);
        mappingEditorStatus_ = HLW(L"菜单已重绑：") + combo.label;
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// deleteMenuBinding
bool D3D11Renderer::deleteMenuBinding(size_t index) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.menus.size()) return false;
        const bool wasFreeMenu = isMenuFreeCursorCategoryLocal(profile.menus[index].category);
        profile.menus.erase(profile.menus.begin() + static_cast<std::ptrdiff_t>(index));
        menuRuntime_.Reset();
        if (wasFreeMenu) {
            lockRuntime_.Reset();
        }
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        selectedMenuIndex_ = -1;
        selectedMappingBindingIndex_ = -1;
        compassSelected_ = false;
        menuEditDrag_ = MenuEditDrag::None;
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"菜单已删除");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// editMenuOptions
bool D3D11Renderer::editMenuOptions(size_t index) {
        const PcMappingProfile& cur = mappingRuntime_.GetProfile();
        if (index >= cur.menus.size()) return false;
        const auto& m = cur.menus[index];
        PcMenuOptions opt;
        opt.category = m.category;
        opt.relativeSpeedX = m.relativeSpeedX;
        opt.relativeSpeedY = m.relativeSpeedY;
        opt.radiusNorm = clampMenuButtonRadiusNormLocal(m.radiusNorm);
        opt.triggerPlacement = m.triggerPlacement;
        opt.itemLandingZone = m.itemLandingZone;
        opt.itemWheelScrollEnabled = m.itemWheelScrollEnabled;
        opt.itemWheelInvert = m.itemWheelInvert;
        opt.itemWheelScrollSpeed = m.itemWheelScrollSpeed;
        opt.itemWheelMaxDistancePx = m.itemWheelMaxDistancePx;
        opt.itemWheelStepPx = m.itemWheelStepPx;
        mappingEditorStatus_ = HLW(L"菜单设置：独立窗口已打开");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        if (!OpenMenuOptionsDialog(inst_, hwnd_, opt, static_cast<WPARAM>(index))) {
            mappingEditorStatus_ = HLW(L"菜单设置打开失败");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
        }
        return true;
    }

// applyMenuOptionsResult
void D3D11Renderer::applyMenuOptionsResult(size_t index, const PcMenuOptions& opt) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.menus.size()) return;
        auto& m = profile.menus[index];
        m.radiusNorm = clampMenuButtonRadiusNormLocal(opt.radiusNorm);
        if (!isMenuFreeCursorCategoryLocal(m.category)) {
            m.relativeSpeedX = (std::max)(0.4, (std::min)(1.2, opt.relativeSpeedX));
            m.relativeSpeedY = (std::max)(0.4, (std::min)(1.2, opt.relativeSpeedY));
        }
        else {
            m.relativeSpeedX = 0.5;
            m.relativeSpeedY = 0.5;
        }
        m.triggerPlacement = opt.triggerPlacement;
        m.itemLandingZone = opt.itemLandingZone;
        m.itemWheelScrollEnabled = opt.itemWheelScrollEnabled;
        m.itemWheelInvert = opt.itemWheelInvert;
        m.itemWheelScrollSpeed = (std::max)(PC_MENU_ITEM_WHEEL_SPEED_MIN, (std::min)(PC_MENU_ITEM_WHEEL_SPEED_MAX, opt.itemWheelScrollSpeed));
        m.itemWheelMaxDistancePx = (std::max)(PC_MENU_ITEM_WHEEL_DISTANCE_MIN, (std::min)(PC_MENU_ITEM_WHEEL_DISTANCE_MAX, opt.itemWheelMaxDistancePx));
        m.itemWheelStepPx = (std::max)(PC_MENU_ITEM_WHEEL_STEP_MIN, (std::min)(PC_MENU_ITEM_WHEEL_STEP_MAX, opt.itemWheelStepPx));
        resetMenuRangeToDefault(m);
        menuRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        selectedMenuIndex_ = static_cast<int>(index);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"菜单设置已更新");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// updateMenuPositionFromPoint
bool D3D11Renderer::updateMenuPositionFromPoint(int x, int y) {
        if (menuEditDrag_ == MenuEditDrag::None || selectedMenuIndex_ < 0) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (selectedMenuIndex_ >= static_cast<int>(profile.menus.size())) return false;
        PcMenuBinding m = menuDragStart_;

        if (menuEditDrag_ == MenuEditDrag::Body) {
            int nx = 0, ny = 0;
            if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
            const int dx = nx - menuDragStartXNorm_;
            const int dy = ny - menuDragStartYNorm_;
            m.centerXNorm = PcMappingProfile::ClampNorm(menuDragStart_.centerXNorm + dx);
            m.centerYNorm = PcMappingProfile::ClampNorm(menuDragStart_.centerYNorm + dy);
            const int r = clampMenuButtonRadiusNormLocal(m.radiusNorm);
            m.centerXNorm = (std::max)(r, (std::min)(1000000 - r, m.centerXNorm));
            m.centerYNorm = (std::max)(r, (std::min)(1000000 - r, m.centerYNorm));
            if (isMenuFreeCursorCategoryLocal(m.category)) {
                m.triggerXNorm = PcMappingProfile::ClampNorm(menuDragStart_.triggerXNorm + dx);
                m.triggerYNorm = PcMappingProfile::ClampNorm(menuDragStart_.triggerYNorm + dy);
                m.freeCursorXNorm = PcMappingProfile::ClampNorm(menuDragStart_.freeCursorXNorm + dx);
                m.freeCursorYNorm = PcMappingProfile::ClampNorm(menuDragStart_.freeCursorYNorm + dy);
                m.closeXNorm = PcMappingProfile::ClampNorm(menuDragStart_.closeXNorm + dx);
                m.closeYNorm = PcMappingProfile::ClampNorm(menuDragStart_.closeYNorm + dy);
            }
            resetMenuRangeToDefault(m);
            mappingEditorStatus_ = HLW(L"菜单位置：拖动更新中");
        }
        else {
            int ax = 0, ay = 0;
            if (!menuAnchorPointToNormFromClient(m, x, y, ax, ay)) return false;
            if (std::abs(ax - menuDragStartXNorm_) > 4000 || std::abs(ay - menuDragStartYNorm_) > 4000) {
                menuEditDragMoved_ = true;
            }
            if (menuEditDrag_ == MenuEditDrag::TriggerAnchor) {
                m.triggerXNorm = ax;
                m.triggerYNorm = ay;
                mappingEditorStatus_ = HLW(L"菜单触发键圆点：拖动更新中");
            }
            else if (menuEditDrag_ == MenuEditDrag::LandingAnchor) {
                m.freeCursorXNorm = ax;
                m.freeCursorYNorm = ay;
                mappingEditorStatus_ = HLW(L"菜单鼠标落点圆点：拖动更新中");
            }
            else if (menuEditDrag_ == MenuEditDrag::CloseAnchor) {
                m.closeXNorm = ax;
                m.closeYNorm = ay;
                mappingEditorStatus_ = HLW(L"菜单关闭圆点：拖动更新中");
            }
        }

        profile.menus[static_cast<size_t>(selectedMenuIndex_)] = m;
        menuRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        refreshMappingToolbarStatus();
        return true;
    }
