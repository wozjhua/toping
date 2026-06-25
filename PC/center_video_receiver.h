#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <memory>
#include <d3d11.h>

#include "mirror_types.h"

class CenterRoiVideoDisplayReceiver {
public:
    explicit CenterRoiVideoDisplayReceiver(SharedState& state, ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dCtx = nullptr);
    ~CenterRoiVideoDisplayReceiver();

    void start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
