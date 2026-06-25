#include "d3d11_renderer.h"

// setMappingEditMode
void D3D11Renderer::setMappingEditMode(bool editMode) {
        if (mappingEditMode_ == editMode) return;
        mappingEditMode_ = editMode;
        if (!mappingEditMode_) {
            selectedMappingBindingIndex_ = -1;
            selectedMenuIndex_ = -1;
            selectedMacroIndex_ = -1;
            selectedMacroStepId_ = 0;
            macroStepEditorVisible_ = false;
            macroDrag_ = false;
            macroStepDrag_ = false;
            macroStepRangeDrag_ = false;
            mappingBindingDrag_ = false;
            menuEditDrag_ = MenuEditDrag::None;
            menuEditDragMoved_ = false;
            if (lockEditDrag_ != LockEditDrag::None && GetCapture() == hwnd_) ReleaseCapture();
            lockEditDrag_ = LockEditDrag::None;
            if (mappingCreateMode_ != MappingCreateMode::None) mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = HLW(L"运行模式：映射可触发，UI透明");
        }
        else {
            mappingEditorStatus_ = HLW(L"编辑模式：可创建/编辑映射");
        }
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }
