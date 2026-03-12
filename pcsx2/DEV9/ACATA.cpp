#include "ACATA.h"
#include "common/Console.h"

int ACATA::device_probes[ACATA_DEVCNT] = {0,0};
int ACATA::last_device_probed = 0;
u32 ACATA::last_read = 0;
u32 ACATA::last_write = 0;

u16 ACATA::read16(u32 addr) {
    last_read = addr;
    switch (addr)
    {
    case ACATA_PROBEREG_0:
    case ACATA_PROBEREG_1:
    case ACATA_PROBEREG_2:
    case ACATA_PROBEREG_3:
    case ACATA_DEVICE_SELECT:
    break;
    case ACATA_PROBEREG_4R: // probe result... for available device, first probe must be `ACATA_UNIT_NOT_READY`
        if (last_device_probed == ACATA_UNIT0 && ACATA::device_probes[ACATA_UNIT0] == ACATA_UNIT_NOT_READY) {
            ACATA::REGS[addr] = ACATA::device_probes[ACATA_UNIT0] = ACATA_UNIT_READY;
            return ACATA_UNIT_NOT_READY;
        }
    break;
    
    default:
        break;
    }
    Console.Error("%-16s %08X:  %08X", __FUNCTION__, addr, ACATA::REGS[addr]);
    return ACATA::REGS[addr];
}

#define MMIO_RESPOND(V, O, N) if (V == O) V = N

void ACATA::write16(u32 addr, u16 val) {
    last_write = addr;
    u16 V = val;
    Console.Error("%-16s %08X = %08X", __FUNCTION__, addr, V);
    switch (addr) {
    case ACATA_PROBEREG_0: V = 52; break;
    case ACATA_PROBEREG_1:
    case ACATA_PROBEREG_2:
        // value 2: ata_probe()
    case ACATA_PROBEREG_3:
    case ACATA_PROBEREG_4R:
        break;
    case ACATA_DEVICE_SELECT:
        ACATA::last_device_probed = ((V & ACATA_UNIT1) != 0); // ACATA: 0x0: unit 0 | 0x10: unit 1
        if (ACATA::last_device_probed == ACATA_UNIT0) ACATA::device_probes[ACATA_UNIT0] = ACATA_UNIT_NOT_READY; // we only do this for unit 0 bc we want unit1 to fail
        break;
    
    default:
        break;
    }
    ACATA::REGS[addr] = V;
}

std::map<u32, u32> ACATA::REGS = {
    {ACATA_BASE_PROBEADDR, 0xFFFFFFFF},
    {ACATA_PROBEREG_0,     0},
    {ACATA_PROBEREG_1,     0},
    {ACATA_PROBEREG_2,     0},
    {ACATA_PROBEREG_3,     0},
    {ACATA_DEVICE_SELECT,  0},
    {ACATA_PROBEREG_4R,    0},
    // atapi_packet_send
    {0x16050000,           0},// 0;
    {0x16040000,           0},// 64;
    //{ACATA_DEVICE_SELECT,           0},// flag & 0x10;
    {0x16160000,           0},// (flag & 2) ^ 2;
    {0x16010000,           0},// flag & 1;
};