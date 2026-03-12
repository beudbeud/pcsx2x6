#pragma once

/**
 * @file ACATA.h
 * ATA interface through the NAMCO board of the SYSTEM246/256
 * used by ACATA.IRX, which is used by ACDVDV and ACATAD for using disc readers or hard drivers on system2x6 units
 */

#include "MemoryTypes.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"
#include <map>
#define ACATA_DEVCNT 2 // ammount of ATA devices probed by ACATA.IRX

#define ACATA_PROBEREG_0  0x16020000
#define ACATA_PROBEREG_1  0x16030000
#define ACATA_PROBEREG_2  0x16160000
#define ACATA_PROBEREG_3  0x16010000
#define ACATA_DEVICE_SELECT  0x16060000 // ACATA_DEVICE_SELECT [`ACATA_UNIT0`, `ACATA_UNIT1`]
#define ACATA_PROBEREG_4R 0x16070000 // this is the reg that holds the response to `ACATA_DEVICE_SELECT`, it is set to 0 TWICE before getting read for the response, the response wait loop is `while(++count <= 1999999)`

#define ACATA_16050000    0x16050000 // REF: atapi_packet_send
#define ACATA_16040000    0x16040000

#define ACATA_BASE_PROBEADDR 0x16000000
#define ACATA_RANGE 0x1600

#define ACATA_UNIT0 0x0  // 16 * (unitIndex != 0)
#define ACATA_UNIT1 0x10 // 16 * (unitIndex != 0)
#define ACATA_UNIT_NOT_READY 0x80 // ACATA expects this bit set for a successful probe
#define ACATA_UNIT_READY 0x0 // ACATA uses ACATA_UNIT_NOT_READY bit to check for errors... however, it expects the first probe to be an error. and we only want that for unit 1

#define ACATA_PROBE_BEGIN_NOTICE 0x16020000 // set to 4660

namespace ACATA 
{
    extern std::map<u32, u32> REGS;
    extern int device_probes[ACATA_DEVCNT];
    extern int last_device_probed;
    extern u32 last_read;
    extern u32 last_write;
    u16 read16(u32 addr);
    void write16(u32 addr, u16 val);
}
