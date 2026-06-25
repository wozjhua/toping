#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <memory>

#include "mirror_types.h"

inline constexpr int DEFAULT_AUDIO_VOLUME_PERCENT = 30;
extern std::atomic<int> g_audioVolumePercent;

class AudioReceiver {
public:
    explicit AudioReceiver(SharedState& state);
    ~AudioReceiver();

    void start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
