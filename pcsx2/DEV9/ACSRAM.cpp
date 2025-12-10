#include "ACSRAM.h"
#include "IopMem.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "ps2/BiosTools.h"

/// TODO: ACSRAM should be saved like PS2 NVRAM!!
/// TODO: ACSRAM should be per-game. keep it per-emulator for the duration of the early development stage

u8 ACSRAM::SRAMBUF[NamcoMemSize::ACSRAM];


void ACSRAM::ReadFile(std::string path) {
}

void ACSRAM::WriteFile(std::string path) {
}

void ACSRAM::Clear(u8 fillerbyte) {
    std::memset(ACSRAM::SRAMBUF, fillerbyte, sizeof(ACSRAM::SRAMBUF));
}

std::string ACSRAM::GetSRAMPath(void) {
	return Path::ReplaceExtension(BiosPath, "sram");
}

u8 ACSRAM::Read8(u32 addr) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        return ACSRAM::SRAMBUF[T];
    }
    return 0;
}

u16 ACSRAM::Read16(u32 addr) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        u16* A = (u16*)&ACSRAM::SRAMBUF[T];
        return *A;
    }
    return 0;
}

u32 ACSRAM::Read32(u32 addr) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        u32* A = (u32*)&ACSRAM::SRAMBUF[T];
        return *A;
    }
    return 0;
}


void ACSRAM::Write8(u32 addr, u8 val) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM::SRAMBUF[T] = val;
    }
}

void ACSRAM::Write16(u32 addr, u16 val) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        u16* A = (u16*)&ACSRAM::SRAMBUF[T];
        *A = val;
    }
}

 void ACSRAM::Write32(u32 addr, u32 val) {
    u32 T = addr - ACSRAM_ADDR_BASE_IOP_POV;
    if (T < ACSRAM_MAX_SIZE) {
        u32* A = (u32*)&ACSRAM::SRAMBUF[T];
        *A = val;
    }
}