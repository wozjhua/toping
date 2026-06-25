#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <cstdint>
#include <string>

#include "mirror_types.h"

struct AdbSetupResult {
    bool ok = false;
    std::wstring status;
};

int32_t ReadBE32(const uint8_t* p);
int64_t ReadBE64(const uint8_t* p);
FrameHeader ParseHeader(const uint8_t* buf);

void SetAdbSerialOverride(const std::string& serial);
std::string GetAdbSerialOverride();
std::string BuildAdbTargetPrefix(const std::string& adbQuoted);

AdbSetupResult RunAdbForward();
AdbSetupResult RunAdbAudioForward();
AdbSetupResult RunAdbCenterRoiVideoForward();

bool RecvAll(SOCKET s, uint8_t* dst, int size);
int SocketPendingBytes(SOCKET s);
int GetSocketOptInt(SOCKET s, int level, int optName);
void ConfigureVideoSocketForLowLatency(SOCKET s);
