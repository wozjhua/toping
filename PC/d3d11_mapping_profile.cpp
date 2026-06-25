#include "d3d11_renderer.h"

// mappingExeDirW
std::wstring D3D11Renderer::mappingExeDirW() {
        wchar_t path[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return L".";
        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (slash) *slash = L'\0';
        return std::wstring(path);
    }

// mappingProfilePathForSlot
std::wstring D3D11Renderer::mappingProfilePathForSlot(int slot) {
        slot = (std::max)(1, (std::min)(3, slot));
        std::wstring dir = mappingExeDirW();
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L"\\";
        dir += HLW(L"huilang_pc_mapping_");
        dir += std::to_wstring(slot);
        dir += HLW(L".hlm");
        return dir;
    }

// legacyDefaultMappingProfilePath
std::wstring D3D11Renderer::legacyDefaultMappingProfilePath() {
        std::wstring dir = mappingExeDirW();
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L"\\";
        dir += HLW(L"huilang_pc_mapping.hlm");
        return dir;
    }

// defaultMappingProfilePath
std::wstring D3D11Renderer::defaultMappingProfilePath() {
        return mappingProfilePathForSlot(1);
    }

// appendMacrosToMappingFile
bool D3D11Renderer::appendMacrosToMappingFile(const std::wstring& path) const {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || !fp) return false;
        std::fprintf(fp, "# HLMACRO1\n");
        for (const auto& macro : pcMacros_) {
            const std::string combo = intListToStringLocal(macro.comboTriggerCodes);
            const std::string labelHex = wideToHexLocal(macro.triggerLabel);
            std::fprintf(
                fp,
                "macro %s %d %d %d %d %d %d %d %d %d %s %s\n",
                macro.id.c_str(),
                macro.enabled ? 1 : 0,
                macro.mouseButtonCode,
                static_cast<int>(macro.triggerCondition),
                macro.startStepId,
                macro.slot,
                PcMappingProfile::ClampNorm(macro.xNorm),
                PcMappingProfile::ClampNorm(macro.yNorm),
                PcMappingProfile::ClampButtonRadiusNorm(macro.radiusNorm),
                macro.consumeEvent ? 1 : 0,
                combo.c_str(),
                labelHex.c_str()
            );
            for (const auto& step : macro.steps) {
                std::fprintf(
                    fp,
                    "macrostep %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    macro.id.c_str(),
                    step.id,
                    step.order,
                    static_cast<int>(step.actionType),
                    step.slot,
                    PcMappingProfile::ClampNorm(step.xNorm),
                    PcMappingProfile::ClampNorm(step.yNorm),
                    PcMappingProfile::ClampNorm(step.endXNorm),
                    PcMappingProfile::ClampNorm(step.endYNorm),
                    (std::max)(1, (std::min)(5000, step.durationMs)),
                    (std::max)(0, (std::min)(5000, step.delayAfterMs)),
                    (std::max)(1, (std::min)(32, step.repeatCount)),
                    (std::max)(0, (std::min)(200000, step.randomRadiusNorm)),
                    step.nextStepId,
                    step.linuxKeyCode,
                    step.wheelSteps
                );
            }
        }
        std::fclose(fp);
        return true;
    }

// loadMacrosFromMappingFile
bool D3D11Renderer::loadMacrosFromMappingFile(const std::wstring& path, std::vector<PcMacroRuntime::Binding>& outMacros) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
        outMacros.clear();
        char line[2048];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            std::istringstream iss(s);
            std::string tag;
            iss >> tag;
            if (tag == "macro") {
                PcMacroRuntime::Binding m;
                int enabled = 1;
                int condition = static_cast<int>(PcMacroRuntime::TriggerCondition::Press);
                int consume = 1;
                std::string combo;
                std::string labelHex;
                iss >> m.id >> enabled >> m.mouseButtonCode >> condition >> m.startStepId >> m.slot >> m.xNorm >> m.yNorm >> m.radiusNorm >> consume >> combo >> labelHex;
                if (!iss.fail() && !m.id.empty()) {
                    m.enabled = enabled != 0;
                    if (condition < 0 || condition > 1) condition = 0;
                    m.triggerCondition = static_cast<PcMacroRuntime::TriggerCondition>(condition);
                    m.slot = (std::max)(1, (std::min)(15, m.slot));
                    m.xNorm = PcMappingProfile::ClampNorm(m.xNorm);
                    m.yNorm = PcMappingProfile::ClampNorm(m.yNorm);
                    m.radiusNorm = PcMappingProfile::ClampButtonRadiusNorm(m.radiusNorm);
                    m.consumeEvent = consume != 0;
                    m.comboTriggerCodes = intListFromStringLocal(combo);
                    m.triggerLabel = wideFromHexLocal(labelHex);
                    outMacros.push_back(std::move(m));
                }
            }
            else if (tag == "macrostep") {
                std::string macroId;
                PcMacroRuntime::Step step;
                int action = static_cast<int>(PcMacroRuntime::StepActionType::Tap);
                iss >> macroId >> step.id >> step.order >> action >> step.slot >> step.xNorm >> step.yNorm >> step.endXNorm >> step.endYNorm >> step.durationMs >> step.delayAfterMs >> step.repeatCount >> step.randomRadiusNorm >> step.nextStepId >> step.linuxKeyCode >> step.wheelSteps;
                if (iss.fail() || macroId.empty()) continue;
                if (action < 0 || action > 4) action = 0;
                step.actionType = static_cast<PcMacroRuntime::StepActionType>(action);
                step.slot = (std::max)(1, (std::min)(15, step.slot));
                step.xNorm = PcMappingProfile::ClampNorm(step.xNorm);
                step.yNorm = PcMappingProfile::ClampNorm(step.yNorm);
                step.endXNorm = PcMappingProfile::ClampNorm(step.endXNorm);
                step.endYNorm = PcMappingProfile::ClampNorm(step.endYNorm);
                step.durationMs = (std::max)(1, (std::min)(5000, step.durationMs));
                step.delayAfterMs = (std::max)(0, (std::min)(5000, step.delayAfterMs));
                step.repeatCount = (std::max)(1, (std::min)(32, step.repeatCount));
                step.randomRadiusNorm = (std::max)(0, (std::min)(200000, step.randomRadiusNorm));
                for (auto& macro : outMacros) {
                    if (macro.id == macroId) {
                        macro.steps.push_back(step);
                        break;
                    }
                }
            }
        }
        std::fclose(fp);
        for (auto& macro : outMacros) {
            normalizeMacroSteps(macro);
        }
        return true;
    }

// appendRuntimeSettingsToMappingFile
bool D3D11Renderer::appendRuntimeSettingsToMappingFile(const std::wstring& path) {
        // 保存前先把当前 F2 面板上的临时值读回 settings_。
        // 如果 F2 没打开，这些函数不会改变当前 settings_。
        if (settingsHwnd_) {
            applySettingsFromControls();
        }

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || !fp) return false;
        std::fprintf(fp, "# HLSETTINGS2\n");
        std::fprintf(
            fp,
            "settings2 %u %u %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            static_cast<unsigned>(settings_.hudKey),
            static_cast<unsigned>(settings_.fullscreenKey1),
            static_cast<unsigned>(settings_.fullscreenKey2),
            static_cast<unsigned>(settings_.settingsKey),
            static_cast<unsigned>(settings_.splitDebugKey),
            static_cast<unsigned>(settings_.exitKey),
            settings_.hudRefreshMs,
            settings_.audioVolumePercent,
            settings_.jpegSubsamplingMode,
            settings_.jpegQuality,
            settings_.keepFrameRate ? 1 : 0,
            settings_.targetFps,
            settings_.fullscreenPreset,
            settings_.fullscreenSplitParts,
            settings_.cropSize,
            settings_.cropSplitParts,
            settings_.captureMode,
            settings_.videoPreset,
            settings_.videoWidth,
            settings_.videoHeight,
            settings_.videoBitrateMbps,
            settings_.videoFps,
            settings_.videoQualityMode,
            settings_.centerRoiUseVideo ? 1 : 0,
            settings_.roiRegion,
            settings_.roiEdgeQualityReduction,
            settings_.roiEdgeScalePercent,
            settings_.roiWidthPercent,
            settings_.roiHeightPercent,
            settings_.roiTopLowPercent,
            settings_.roiJpegCenterWidthPercent,
            settings_.roiCenterCpuWeightPercent,
            settings_.strictHybridSync ? 1 : 0,
            settings_.compactHudMode ? 1 : 0,
            settings_.debugHudMode ? 1 : 0,
            splitWeightCustomized_ ? 1 : 0
        );
        std::fprintf(fp, "centeronly %d\n", settings_.jpegCenterOnly ? 1 : 0);
        std::fprintf(fp, "bigcoreweight %d\n", settings_.roiBigCoreWeightPercent);
        std::fprintf(fp, "stretchframe %d\n", settings_.stretchFrame ? 1 : 0);
        std::fprintf(fp, "splitweights %d", MAX_RUNTIME_SPLIT_PARTS);
        for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
            std::fprintf(fp, " %d", settings_.splitWeightPercent[i]);
        }
        std::fprintf(fp, "\n");
        std::fprintf(fp, "wifi %s\n", wideToHexLocal(wifiAdbEndpoint_).c_str());
        std::fclose(fp);
        return true;
    }

// loadRuntimeSettingsFromMappingFile
bool D3D11Renderer::loadRuntimeSettingsFromMappingFile(const std::wstring& path) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
        bool found = false;
        bool loadedWeights = false;
        char line[4096];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            std::istringstream iss(s);
            std::string tag;
            iss >> tag;
            if (tag == "settings2") {
                unsigned hudKey = settings_.hudKey;
                unsigned full1 = settings_.fullscreenKey1;
                unsigned full2 = settings_.fullscreenKey2;
                unsigned settingsKey = settings_.settingsKey;
                unsigned splitKey = settings_.splitDebugKey;
                unsigned exitKey = settings_.exitKey;
                int keep = settings_.keepFrameRate ? 1 : 0;
                int centerVideo = settings_.centerRoiUseVideo ? 1 : 0;
                int strictSync = settings_.strictHybridSync ? 1 : 0;
                int compact = settings_.compactHudMode ? 1 : 0;
                int debug = settings_.debugHudMode ? 1 : 0;
                int weightCustom = splitWeightCustomized_ ? 1 : 0;
                iss >> hudKey >> full1 >> full2 >> settingsKey >> splitKey >> exitKey
                    >> settings_.hudRefreshMs
                    >> settings_.audioVolumePercent
                    >> settings_.jpegSubsamplingMode
                    >> settings_.jpegQuality
                    >> keep
                    >> settings_.targetFps
                    >> settings_.fullscreenPreset
                    >> settings_.fullscreenSplitParts
                    >> settings_.cropSize
                    >> settings_.cropSplitParts
                    >> settings_.captureMode
                    >> settings_.videoPreset
                    >> settings_.videoWidth
                    >> settings_.videoHeight
                    >> settings_.videoBitrateMbps
                    >> settings_.videoFps
                    >> settings_.videoQualityMode
                    >> centerVideo
                    >> settings_.roiRegion
                    >> settings_.roiEdgeQualityReduction
                    >> settings_.roiEdgeScalePercent
                    >> settings_.roiWidthPercent
                    >> settings_.roiHeightPercent
                    >> settings_.roiTopLowPercent
                    >> settings_.roiJpegCenterWidthPercent
                    >> settings_.roiCenterCpuWeightPercent
                    >> strictSync
                    >> compact
                    >> debug
                    >> weightCustom;
                if (!iss.fail()) {
                    settings_.hudKey = static_cast<UINT>(hudKey);
                    settings_.fullscreenKey1 = static_cast<UINT>(full1);
                    settings_.fullscreenKey2 = static_cast<UINT>(full2);
                    settings_.settingsKey = static_cast<UINT>(settingsKey);
                    settings_.splitDebugKey = static_cast<UINT>(splitKey);
                    settings_.exitKey = static_cast<UINT>(exitKey);
                    settings_.keepFrameRate = keep != 0;
                    settings_.centerRoiUseVideo = centerVideo != 0;
                    settings_.strictHybridSync = strictSync != 0;
                    settings_.compactHudMode = compact != 0;
                    settings_.debugHudMode = debug != 0;
                    splitWeightCustomized_ = weightCustom != 0;
                    found = true;
                }
            }
            else if (tag == "centeronly") {
                int centerOnly = settings_.jpegCenterOnly ? 1 : 0;
                iss >> centerOnly;
                if (!iss.fail()) settings_.jpegCenterOnly = centerOnly != 0;
            }
            else if (tag == "bigcoreweight") {
                int bigWeight = settings_.roiBigCoreWeightPercent;
                iss >> bigWeight;
                if (!iss.fail()) settings_.roiBigCoreWeightPercent = VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(bigWeight);
            }
            else if (tag == "stretchframe") {
                int stretchFrame = settings_.stretchFrame ? 1 : 0;
                iss >> stretchFrame;
                if (!iss.fail()) settings_.stretchFrame = stretchFrame != 0;
            }
            else if (tag == "splitweights") {
                int n = 0;
                iss >> n;
                if (!iss.fail() && n > 0) {
                    for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
                        int v = 100;
                        if (i < n) iss >> v;
                        settings_.splitWeightPercent[i] = clampIntLocal(v, 25, 400);
                    }
                    splitWeightInitializedCount_ = clampIntLocal(n, 0, MAX_RUNTIME_SPLIT_PARTS);
                    loadedWeights = true;
                }
            }
            else if (tag == "wifi") {
                std::string hex;
                iss >> hex;
                if (!hex.empty()) wifiAdbEndpoint_ = wideFromHexLocal(hex);
            }
        }
        std::fclose(fp);
        if (!found) return false;

        settings_.hudRefreshMs = clampIntLocal(settings_.hudRefreshMs, 100, 1000);
        settings_.audioVolumePercent = clampIntLocal(settings_.audioVolumePercent, 0, 100);
        settings_.jpegSubsamplingMode = settings_.jpegSubsamplingMode == 444 ? 444 : 420;
        settings_.jpegQuality = clampIntLocal(settings_.jpegQuality, 60, 100);
        settings_.targetFps = clampIntLocal(settings_.targetFps, 30, 240);
        settings_.fullscreenPreset = clampIntLocal(settings_.fullscreenPreset, 1, 5);
        settings_.fullscreenSplitParts = clampIntLocal(settings_.fullscreenSplitParts, 2, MAX_RUNTIME_SPLIT_PARTS);
        settings_.cropSize = clampIntLocal(settings_.cropSize, 320, 1080);
        settings_.cropSize = (settings_.cropSize / 20) * 20;
        settings_.cropSplitParts = clampIntLocal(settings_.cropSplitParts, 1, 4);
        settings_.captureMode = clampIntLocal(settings_.captureMode, 0, 1);
        settings_.videoBitrateMbps = VideoStreamTuning::NormalizeVideoBitrateMbps(settings_.videoBitrateMbps);
        settings_.videoFps = VideoStreamTuning::NormalizeVideoFps(settings_.videoFps);
        settings_.videoQualityMode = VideoStreamTuning::NormalizeVideoQualityMode(settings_.videoQualityMode);
        settings_.roiRegion = VideoStreamTuning::NormalizeRoiRegion(settings_.roiRegion);
        settings_.roiEdgeQualityReduction = VideoStreamTuning::NormalizeRoiEdgeQualityReduction(settings_.roiEdgeQualityReduction);
        settings_.roiEdgeScalePercent = VideoStreamTuning::NormalizeRoiEdgeScalePercent(settings_.roiEdgeScalePercent);
        settings_.roiWidthPercent = VideoStreamTuning::NormalizeRoiPercent(settings_.roiWidthPercent);
        settings_.roiHeightPercent = VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent);
        settings_.roiTopLowPercent = VideoStreamTuning::NormalizeRoiTopLowPercent(settings_.roiTopLowPercent);
        settings_.roiJpegCenterWidthPercent = VideoStreamTuning::NormalizeRoiJpegCenterWidthPercent(settings_.roiJpegCenterWidthPercent);
        settings_.roiCenterCpuWeightPercent = VideoStreamTuning::NormalizeRoiCenterCpuWeightPercent(settings_.roiCenterCpuWeightPercent);
        settings_.roiBigCoreWeightPercent = VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(settings_.roiBigCoreWeightPercent);
        if (settings_.jpegCenterOnly && settings_.centerRoiUseVideo && settings_.roiRegion == ROI_REGION_CENTER) {
            settings_.strictHybridSync = false;
        }
        if (!loadedWeights) splitWeightInitializedCount_ = 0;
        stretch_ = settings_.stretchFrame;

        g_audioVolumePercent.store(settings_.audioVolumePercent, std::memory_order_release);
        state_.centerRoiStrictSync.store(settings_.centerRoiUseVideo && settings_.strictHybridSync, std::memory_order_release);
        if (!settings_.centerRoiUseVideo) clearCenterRoiOverlay();
        if (settingsHwnd_) {
            fillSettingsControls();
            updateSettingsCompactLayout();
        }
        pendingRuntimeSettingsSync_ = true;
        lastRuntimeSyncAttempt_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        hudDirty_ = true;
        titleDirty_ = true;
        dirtyWindow_ = true;
        return true;
    }

// applyLoadedMappings
void D3D11Renderer::applyLoadedMappings(PcMappingProfile profile, std::vector<PcMacroRuntime::Binding> macros, const std::wstring& path) {
        mappingRuntime_.SetProfile(profile);
        pcMacros_ = std::move(macros);
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        selectedMacroIndex_ = -1;
        selectedMacroStepId_ = 0;
        macroStepEditorVisible_ = false;
        compassSelected_ = false;
        macroRuntime_.SetBindings(pcMacros_);
        SetPcMappingOverlayMacroSource(&pcMacros_, selectedMacroIndex_, selectedMacroStepId_, macroStepEditorVisible_);
        mappingRuntime_.SetEnabled(!mappingRuntime_.GetProfile().Empty() || !pcMacros_.empty());
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        std::wstring msg = HLW(L"映射已载入：");
        msg += path;
        updateStatusText(msg);
        refreshMappingToolbarStatus();
    }

// loadMappingProfileSlot
bool D3D11Renderer::loadMappingProfileSlot(int slot, bool silentIfMissing) {
        std::wstring path = mappingProfilePathForSlot(slot);
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (slot == 1) {
                // 兼容上一版单文件保存名。
                std::wstring legacy = legacyDefaultMappingProfilePath();
                DWORD legacyAttr = GetFileAttributesW(legacy.c_str());
                if (legacyAttr != INVALID_FILE_ATTRIBUTES && !(legacyAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                    path = legacy;
                    attr = legacyAttr;
                }
            }
        }
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!silentIfMissing) {
                std::wstring msg = HLW(L"映射槽位");
                msg += std::to_wstring((std::max)(1, (std::min)(3, slot)));
                msg += HLW(L"：没有保存文件");
                updateStatusText(msg);
                refreshMappingToolbarStatus();
            }
            return false;
        }

        PcMappingProfile profile;
        std::string err;
        if (!profile.LoadFromFile(path, &err)) {
            std::wstring msg = HLW(L"映射载入失败：");
            msg += path;
            updateStatusText(msg);
            refreshMappingToolbarStatus();
            return false;
        }
        std::vector<PcMacroRuntime::Binding> macros;
        loadMacrosFromMappingFile(path, macros);
        applyLoadedMappings(std::move(profile), std::move(macros), path);
        const bool loadedSettings = loadRuntimeSettingsFromMappingFile(path);
        if (loadedSettings) {
            std::wstring msg = HLW(L"映射+F2参数已载入：槽位");
            msg += std::to_wstring((std::max)(1, (std::min)(3, slot)));
            updateStatusText(msg);
        }
        return true;
    }

// loadDefaultMappingProfileIfPresent
void D3D11Renderer::loadDefaultMappingProfileIfPresent() {
        loadMappingProfileSlot(1, true);
    }

// saveMappingProfileSlot
void D3D11Renderer::saveMappingProfileSlot(int slot) {
        slot = (std::max)(1, (std::min)(3, slot));
        const std::wstring path = mappingProfilePathForSlot(slot);
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        profile.name = "slot" + std::to_string(slot);
        bool ok = profile.SaveToFile(path);
        if (ok) ok = appendMacrosToMappingFile(path);
        if (ok) ok = appendRuntimeSettingsToMappingFile(path);
        if (ok) {
            std::wstring msg = HLW(L"槽位");
            msg += std::to_wstring(slot);
            msg += HLW(L"：映射+F2参数已保存");
            updateStatusText(msg);
        }
        else {
            std::wstring msg = HLW(L"槽位");
            msg += std::to_wstring(slot);
            msg += HLW(L"：保存失败 ");
            msg += path;
            updateStatusText(msg);
        }
        refreshMappingToolbarStatus();
    }

// saveDefaultMappingProfile
void D3D11Renderer::saveDefaultMappingProfile() {
        saveMappingProfileSlot(1);
    }

