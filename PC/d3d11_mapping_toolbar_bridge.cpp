#include "d3d11_renderer.h"

// refreshMappingToolbarStatus
void D3D11Renderer::refreshMappingToolbarStatus() {
        dirtyWindow_ = true;
        hudDirty_ = true;
        titleDirty_ = true;
    }

// mappingToolbarStatusText
std::wstring D3D11Renderer::mappingToolbarStatusText() const {
        if (!mappingEditorStatus_.empty()) return mappingEditorStatus_;
        std::wstring s = mappingRuntime_.StatusText();
        if (lockRuntime_.HasLockConfig()) {
            s += HLW(L" | ");
            s += lockRuntime_.StatusText();
        }
        if (!mappingRuntime_.GetProfile().compasses.empty()) {
            s += HLW(L" | ");
            s += compassRuntime_.StatusText();
        }
        if (!mappingRuntime_.GetProfile().menus.empty()) {
            s += HLW(L" | ");
            s += menuRuntime_.StatusText();
        }
        if (!pcMacros_.empty()) {
            s += HLW(L" | ");
            s += macroRuntime_.StatusText();
        }
        return s;
    }

// mappingToolbarCount
int D3D11Renderer::mappingToolbarCount() const {
        const auto& p = mappingRuntime_.GetProfile();
        return static_cast<int>(p.bindings.size() + p.locks.size() + p.compasses.size() + p.menus.size() + pcMacros_.size());
    }

// toggleMappingOverlayHidden
void D3D11Renderer::toggleMappingOverlayHidden() {
        mappingOverlayHidden_ = !mappingOverlayHidden_;
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = mappingOverlayHidden_ ? HLW(L"映射UI已隐藏：映射仍可触发") : HLW(L"映射UI已显示");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// showMappingProfiles
void D3D11Renderer::showMappingProfiles() {
        mappingEditorStatus_ = HLW(L"映射：配置窗口下一步实现");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// startMappingToolbar
void D3D11Renderer::startMappingToolbar() {
        PcMappingToolbarPanel::Callbacks cb;
        cb.beginCreateKey = [this] { beginCreateMappingKey(); };
        cb.beginCreateCompass = [this] { beginCreateMappingCompass(); };
        cb.beginCreateLock = [this] { beginCreateMappingLock(); };
        cb.beginCreateMenuRadial = [this] { beginCreateMappingMenu(PcMenuCategory::MenuRadial); };
        cb.beginCreateMenuItem = [this] { beginCreateMappingMenu(PcMenuCategory::MenuItemOperation); };
        cb.beginCreateMenuBag = [this] { beginCreateMappingMenu(PcMenuCategory::MenuBagOperation); };
        cb.beginCreateMacro = [this] { beginCreateMappingMacro(); };
        cb.showProfiles = [this] { showMappingProfiles(); };
        cb.toggleMappingUi = [this] { toggleMappingOverlayHidden(); };
        cb.saveMappings = [this](int slot) { saveMappingProfileSlot(slot); };
        cb.loadMappings = [this](int slot) { loadMappingProfileSlot(slot, false); };
        cb.setEditMode = [this](bool editMode) { setMappingEditMode(editMode); };
        cb.isEditMode = [this] { return mappingEditMode_; };
        cb.isMappingUiHidden = [this] { return mappingOverlayHidden_; };
        cb.statusText = [this] { return mappingToolbarStatusText(); };
        cb.mappingCount = [this] { return mappingToolbarCount(); };
        mappingToolbar_.Start(inst_, hwnd_, std::move(cb));
    }

