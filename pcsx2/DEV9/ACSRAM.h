#pragma once

#include "MemoryTypes.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"
#include <string>

#define ACSRAM_ADDR_BASE 0xB2500000
#define ACSRAM_ADDR_BASE_IOP_POV 0x12500000
#define ACSRAM_MAX_SIZE _32kb

namespace ACSRAM
{
    // emulation methods
    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    u32 Read32(u32 addr);

    void Write32(u32 addr, u32 val);
    void Write16(u32 addr, u16 val);
    void Write8(u32 addr, u8 val);

    // data
    u8 SRAMBUF[];

    //storage
    void ReadFile(std::string path);
    void WriteFile(std::string path);
    void Clear(u8 fillerbyte = 0x0);
    std::string GetSRAMPath(void);
}
