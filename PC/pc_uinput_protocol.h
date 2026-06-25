#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct PcUinputPacket {
    char magic[2];      // 'P','U'
    uint8_t version;    // 2
    uint8_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    int32_t e;
    int32_t f;
    int32_t g;
    int32_t h;
};
#pragma pack(pop)

static constexpr char PCU_MAGIC0 = 'P';
static constexpr char PCU_MAGIC1 = 'U';
static constexpr uint8_t PCU_VERSION = 2;

static constexpr int32_t PCU_NORM_MAX = 1000000;

enum PcUinputPacketType : uint8_t {
    PCU_HELLO       = 1,
    // V2 DISPLAYCFG semantics copied from the old Android native path:
    // a=rotation(-1 auto, 0..3 force), b=uiW, c=uiH, d=physW(optional), e=physH(optional), f/g/h reserved.
    PCU_CONFIG      = 2,
    PCU_TOUCH_DOWN  = 3,  // a=slot, b=xNorm, c=yNorm
    PCU_TOUCH_MOVE  = 4,  // a=slot, b=xNorm, c=yNorm
    PCU_TOUCH_UP    = 5,  // a=slot
    PCU_KEY         = 6,  // a=linuxCode, b=down(0/1)
    PCU_WHEEL       = 7,  // a=wheelSteps
    PCU_RESET       = 8,
    PCU_PING        = 9,
    PCU_EXIT        = 10,
};
