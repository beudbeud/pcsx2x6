#include "ACATA.h"
#include "common/Console.h"

#define CLRB(V, n) V &= ~(n) //CLeaR Bit
#define U32FU16(hi, lo) (((uint32_t)(hi) << 16) | (uint16_t)(lo)) // u32 from two u16

uint16_t ATA_R_IDENTIFY_PACKET_DEVICE[256] {
    0x8500,
    0,0,0,0,0,0,0,0,0,
    /* serial [10]*/
    0x2020,0x2020,0x2020,0x2020,0x2020,
    /* firmware [15]*/
    0x312E,0x3030,0x2020,0x2020,
    /* model [19]*/
    0x4E41,0x4D43,0x4F20,0x4456,//[22
    0x442D,0x524F,0x4D20,0x4452,//[26
    0x4956,0x4520,0x2020,0x2020,//[30
    0x2020,0x2020,0x2020,0x2020,//[34
    0x2020,0x2020,0x2020,0x2020,//[38
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //[50
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,// [61
    0x0007, //Word 62: Single-word DMA
    0x0007, //Word 63: Multiword DMA
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,//[80]
    0,0,0,0,0,0,0,//[87]
    0x0000,//Word 88 — Ultra DMA
};

int ACATA::device_probes[ACATA_DEVCNT] = {0,0};
int ACATA::last_device_probed = 0;
u32 ACATA::last_read = 0;
u32 ACATA::last_write = 0;
u32 atacmd_responding = -1;
u32 atacmd_response_traverse = 0;

u16 ACATA::read16(u32 addr) {
    last_read = addr;
    switch (addr)
    {
    case ACATA_BASE_PROBEADDR:
        return ACATA::cmd_handleR(addr);
    break;
    case ACATA_PROBEREG_0:
    case ACATA_PROBEREG_1:
    case ACATA_PROBEREG_2: ACATA::REGS[ACATA_R_STATUS] = 0; break;
    case ACATA_PROBEREG_3:
    case ACATA_DEVICE_SELECT:
    break;
    case ACATA_R_STATUS:
        if (ACATA::last_write == ACATA_R_STATUS && ACATA::last_device_probed == ACATA_UNIT0) {
            // reading R_STATUS after writing zero to it? this is the ACATA probe. we have to respond BUSY at least once for the driver to keep going
            // FIXME
            if (ACATA::REGS[ACATA_R_STATUS] & ATA_STAT_BUSY)
                CLRB(ACATA::REGS[ACATA_R_STATUS], ATA_STAT_BUSY);
            else
                ACATA::REGS[ACATA_R_STATUS] |= ATA_STAT_BUSY;
        }
    break;
    
    default:
        break;
    }
    return ACATA::REGS[addr];
}

#define MMIO_RESPOND(V, O, N) if (V == O) V = N


void ACATA::write16(u32 addr, u16 val) {
    last_write = addr;
    u16 V = val;
    switch (addr) {
    case ACATA_PROBEREG_0: V = 52; break;
    case ACATA_PROBEREG_1:
    case ACATA_PROBEREG_3:
    case ACATA_PROBEREG_2:
        // value 2: ata_probe()
    break;
    case ACATA_R_STATUS:
        ACATA::rstat_write_handle(val);
        break;
    case ACATA_DEVICE_SELECT:
        ACATA::last_device_probed = ((V & ACATA_UNIT1) != 0); // ACATA: 0x0: unit 0 | 0x10: unit 1
        break;
    
    default:
        break;
    }
    ACATA::REGS[addr] = V;
}

u32 ACATA::cmd_handled;
u32 ACATA::cmd_handledc;

u16 ACATA::cmd_handleW(u32 addr, u16 val) {
    switch (ACATA::cmd_handled)
    {
    case ATA_C_PACKET:
        Console.Error("ACATA:ATA_C_PACKET:W %04X", val);
        break;
    
    default:
        Console.Error("ACATA: writing from %X while no pending CMD", ACATA_BASE_PROBEADDR);
        break;
    }
}

u16 ACATA::cmd_handleR(u32 addr) {
    switch (ACATA::cmd_handled)
    {
    case -1: break;
    case ATA_C_IDENTIFY_PACKET_DEVICE: //ATA_C_IDENTIFY_PACKET_DEVICE
        if (ACATA::cmd_handledc < 256) {
            //Console.Warning("ATA_C_IDENTIFY_PACKET_DEVICE[%d]: %04X", ACATA::cmd_handledc, ATA_R_IDENTIFY_PACKET_DEVICE[ACATA::cmd_handledc]);
            return ATA_R_IDENTIFY_PACKET_DEVICE[ACATA::cmd_handledc++];
        } else ACATA::cmd_handled = -1;
        break;
    case ATA_C_SET_FEATURES:
    break;
    case ATA_C_PACKET:
    break;
    
    default:
        Console.Error("ACATA: reading from %X while no pending CMD", ACATA_BASE_PROBEADDR);
    }
    return ACATA::REGS[ACATA_BASE_PROBEADDR];
}

void ACATA::rstat_write_handle(u16 val) {
    switch (val)
    {
    case 0:
        // the only situation when we will recieve a 0 (twice) to this reg, is during ACATA init.
        // here, the ata busy flag must be present at least once, on the following checks to this addr
        break;
    case ATA_C_IDENTIFY_PACKET_DEVICE:
        ACATA::cmd_handled = val;
        ACATA::cmd_handledc = 0;
    break;
    case ATA_C_PACKET:
        ACATA::cmd_handled = val;
        Console.Warning("ATA_C_PACKET:", U32FU16(ACATA::REGS[ACATA_16050000], ACATA::REGS[ACATA_16040000]));
        ACATA::REGS[ACATA_R_STATUS] |= ATA_STAT_DRQ;
    break;
    
    
    default:
        Console.Error("unhandled ATACMD %X", val);
        break;
    }
}

std::map<u32, u32> ACATA::REGS = {
    {ACATA_BASE_PROBEADDR, 0x8500},
    {ACATA_PROBEREG_0,     0},
    {ACATA_PROBEREG_1,     0},
    {ACATA_PROBEREG_2,     0},
    {ACATA_PROBEREG_3,     0},
    {ACATA_DEVICE_SELECT,  0},
    {ACATA_R_STATUS,       0},
    // atapi_packet_send
    {0x16050000,           0},// 0;
    {0x16040000,           0},// 64;
    //{ACATA_DEVICE_SELECT,           0},// flag & 0x10;
    {0x16160000,           0},// (flag & 2) ^ 2;
    {0x16010000,           0},// flag & 1;
};

