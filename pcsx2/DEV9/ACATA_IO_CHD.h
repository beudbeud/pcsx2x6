#pragma once

#include "common/ARCADE.h"
#include "ACATA.h"
#include "ACATA_IO_WRITES.h"
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include <libchdr/chd.h>
}

class ChdImage
{
public:

    ChdImage();
    ~ChdImage();

    ChdImage(const ChdImage&) = delete;
    ChdImage& operator=(const ChdImage&) = delete;

    bool Open(const std::string& path, std::string* why = nullptr); // why: the reason it failed
    void Close();

    bool IsOpen() const;

    ACMEDIATYPE GetType() const;
    u32 GetSectorSize() const;
    u64 GetSectorCount() const;

    bool ReadSector(u64 lba, void* buffer);
    bool ReadSectors(u64 lba, u32 count, void* buffer);

    // ARMSX2: a hard drive's writes, which the CHD cannot take, kept in [path] and read back over it
    // (ChdWrites). After Open().
    void OpenWrites(const std::string& path);
    bool WriteSectors(u64 lba, u32 count, const void* buffer);
    
    static bool IsChdFileName(const std::string& path);

private:
    bool ReadHunk(u32 hunk);
    u32 DetectCdDataOffset();

    chd_file* m_chd = nullptr;

    ACMEDIATYPE m_type = ACMEDIATYPE::ACUNK;

    u32 m_hunkSize = 0;
    u32 m_unitBytes = 0;
    u32 m_frameDataOffset = 0;  // where the 2048-byte payload sits inside a raw CD frame

    u64 m_totalUnits = 0;

    std::vector<u8> m_hunkBuffer;

    u32 m_cachedHunk = UINT32_MAX;

    ChdWrites m_writes;
};

extern ChdImage CHD;
