#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pc_mapping_profile.h"
#include "pc_macro_runtime.h"

struct PcMappingOverlayFrame {
    int width = 0;
    int height = 0;
    bool visible = false;
    std::vector<uint8_t> bgra;
};

struct PcMappingOverlayOptions {
    bool editMode = false;
    int selectedBindingIndex = -1;
    int selectedMenuIndex = -1;
    bool selectedCompass = false;
    const std::vector<PcMacroRuntime::Binding>* macros = nullptr;
    int selectedMacroIndex = -1;
    int selectedMacroStepId = 0;
    bool macroStepEditorVisible = false;
    float collapsedAlpha = 0.30f;
};


void SetPcMappingOverlayMacroSource(
    const std::vector<PcMacroRuntime::Binding>* macros,
    int selectedMacroIndex = -1,
    int selectedMacroStepId = 0,
    bool macroStepEditorVisible = false
);

struct PcMappingOverlayHit {
    enum class Kind {
        None,
        BindingChip,
        BindingOptions,
        BindingRebind,
        BindingDelete,
        LockModeChip,
        LockOptions,
        LockRebind,
        CompassBody,
        CompassOptions,
        CompassDelete,
        CompassButtonRebind,
        CompassMotionMode,
        CompassRadiusReverse,
        CompassSectorHandle,
        CompassDiagAnchor,
        MenuBody,
        MenuTriggerAnchor,
        MenuLandingAnchor,
        MenuCloseAnchor,
        MenuRebind,
        MenuDelete,
        MenuOptions,
        MacroBody,
        MacroBind,
        MacroDelete,
        MacroTriggerCondition,
        MacroAddStep,
        MacroStepBody,
        MacroStepConfig,
        MacroStepSwipeEndHandle,
        MacroStepRangeHandle,
        MacroStepEditorClose,
        MacroStepEditorDelete,
        MacroStepEditorAction,
        MacroStepEditorDurationMinus,
        MacroStepEditorDurationPlus,
        MacroStepEditorDelayMinus,
        MacroStepEditorDelayPlus,
        MacroStepEditorRepeatMinus,
        MacroStepEditorRepeatPlus,
        MacroStepEditorRandomMinus,
        MacroStepEditorRandomPlus,
        MacroStepEditorAddAfter,
    } kind = Kind::None;
    size_t bindingIndex = 0;
    int lockMode = 0;
    int compassButton = -1; // 0=上 1=左 2=下 3=右 4=中心
    int compassMotionMode = 0;
    int compassDiagSlot = -1; // 0=LU 1=RU 2=LD 3=RD
    int compassSectorGroup = -1; // 0=WASD正向扇区 1=斜向扇区
    int compassSectorIndex = -1; // 正向:0=W 1=A 2=S 3=D；斜向:0=LU 1=RU 2=LD 3=RD
    size_t menuIndex = 0;
    size_t macroIndex = 0;
    int macroStepId = 0;
};

bool BuildPcMappingOverlayFrame(
    int width,
    int height,
    const PcMappingProfile& profile,
    const PcMappingOverlayOptions& options,
    PcMappingOverlayFrame& outFrame
);

PcMappingOverlayHit HitTestPcMappingOverlay(
    int width,
    int height,
    const PcMappingProfile& profile,
    const PcMappingOverlayOptions& options,
    int x,
    int y
);
