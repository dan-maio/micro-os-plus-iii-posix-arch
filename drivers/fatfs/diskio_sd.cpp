/**
 * @file diskio_sd.cpp
 * @brief FatFs low-level disk glue for the Raspberry Pi Zero 2W SD driver.
 *
 * Bridges the C `disk_*` API (diskio.h) to this port's C++ `sd::SdCard`
 * (Arasan EMMC/SDHCI). The microSD on a real Pi is the *boot* card, so it has
 * an MBR whose first partition is the FAT32 boot partition holding
 * config.txt / kernel7.img (aarch32) or kernel8.img (aarch64) etc. The
 * firmware tests therefore:
 *
 *   - NEVER format / wipe the card (FF_USE_MKFS is 0 in ffconf.h);
 *   - mount that EXISTING FAT32 partition read/write;
 *   - put all test files under a "tests" folder so boot files are untouched.
 *
 * To make FatFs see the FAT boot sector as volume LBA 0, the glue scans the
 * MBR once (on the first disk access), records the first FAT partition's
 * start LBA + size, and every disk_read()/disk_write() is offset by that
 * start. If the card has no MBR (whole card is one FAT volume /
 * "superfloppy"), offset 0 is used.
 *
 * Two ways to bring the card up:
 *   - let the glue own it:  disk_initialize() lazily creates + inits a card;
 *   - hand an already-init'ed card over:  fatfs_use_card(&my_card) then mount.
 */

#include "ff.h"
#include "diskio.h"

#include "sd.hpp"

namespace
{
  sd::SdCard* g_card = nullptr;
  bool g_inited = false;

  // FAT volume window inside the physical card.
  std::uint32_t g_base_lba = 0;   // physical LBA of the FAT volume
  std::uint32_t g_vol_blocks = 0; // size of that volume, in 512-byte sectors

  bool is_fat_type (std::uint8_t t)
  {
    switch (t)
      {
      case 0x01: case 0x04: case 0x06: case 0x0e:  // FAT12/FAT16
      case 0x0b: case 0x0c: case 0x1b: case 0x1c:  // FAT32
      case 0x2b: case 0x2c:
        return true;
      default:
        return false;
      }
  }
} // namespace

// Scan the MBR (sector 0) for the first FAT partition and set g_base_lba /
// g_vol_blocks. Returns false only on I/O failure; when no FAT partition is
// found the whole card is used (offset 0).
static bool locate_partition ()
{
  std::uint8_t mbr[512];
  if (!g_card->read_sectors (0, 1, mbr))
    {
      return false;
    }
  if (mbr[510] != 0x55 || mbr[511] != 0xAA)
    {
      // No MBR signature: treat the whole card as one volume.
      g_base_lba = 0;
      g_vol_blocks = g_card->sector_count ();
      return true;
    }
  for (int i = 0; i < 4; ++i)
    {
      std::uint8_t* e = mbr + 446 + i * 16;
      if (e[4] != 0 && is_fat_type (e[4]))
        {
          std::uint32_t start = static_cast<std::uint32_t> (e[8])
                                | (static_cast<std::uint32_t> (e[9]) << 8)
                                | (static_cast<std::uint32_t> (e[10]) << 16)
                                | (static_cast<std::uint32_t> (e[11]) << 24);
          std::uint32_t size = static_cast<std::uint32_t> (e[12])
                               | (static_cast<std::uint32_t> (e[13]) << 8)
                               | (static_cast<std::uint32_t> (e[14]) << 16)
                               | (static_cast<std::uint32_t> (e[15]) << 24);
          g_base_lba = start;
          g_vol_blocks = size;
          return true;
        }
    }
  // MBR present but no FAT partition: fall back to the whole card.
  g_base_lba = 0;
  g_vol_blocks = g_card->sector_count ();
  return true;
}

extern "C" void fatfs_use_card (void* card)
{
  g_card = static_cast<sd::SdCard*> (card);
  if (g_card != nullptr && g_card->ready ())
    {
      // Card already up (test called init() itself): just locate the volume.
      locate_partition ();
      g_inited = true;
    }
}

extern "C" void* fatfs_card ()
{
  return static_cast<void*> (g_card);
}

/* Report the FAT window located in the MBR (see locate_partition()). Set as
 * soon as the card is bound, i.e. BEFORE FatFs is asked to mount, so it stays
 * meaningful when the mount itself fails. */
extern "C" void fatfs_window (std::uint32_t* base, std::uint32_t* blocks)
{
  if (base != nullptr)
    {
      *base = g_base_lba;
    }
  if (blocks != nullptr)
    {
      *blocks = g_vol_blocks;
    }
}

extern "C" DSTATUS disk_status (BYTE pdrv)
{
  if (pdrv != 0)
    {
      return STA_NOINIT;
    }
  return g_inited ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize (BYTE pdrv)
{
  if (pdrv != 0)
    {
      return STA_NOINIT;
    }
  if (!g_inited)
    {
      if (g_card == nullptr)
        {
          g_card = new sd::SdCard;
        }
      if (!g_card->init ())
        {
          return STA_NOINIT | STA_NODISK;
        }
      if (!locate_partition ())
        {
          return STA_NOINIT;
        }
      g_inited = true;
    }
  return g_inited ? 0 : STA_NOINIT;
}

extern "C" DRESULT disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
  if (pdrv != 0)
    {
      return RES_PARERR;
    }
  if (!g_inited)
    {
      return RES_NOTRDY;
    }
  if (sector + count > g_vol_blocks)
    {
      return RES_PARERR;
    }
  return g_card->read_sectors (g_base_lba + static_cast<std::uint32_t> (sector),
                               count, buff)
             ? RES_OK
             : RES_ERROR;
}

extern "C" DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector,
                               UINT count)
{
  if (pdrv != 0)
    {
      return RES_PARERR;
    }
  if (!g_inited)
    {
      return RES_NOTRDY;
    }
  if (sector + count > g_vol_blocks)
    {
      return RES_PARERR;
    }
  return g_card->write_sectors (
             g_base_lba + static_cast<std::uint32_t> (sector), count, buff)
             ? RES_OK
             : RES_ERROR;
}

extern "C" DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
  if (pdrv != 0)
    {
      return RES_PARERR;
    }
  switch (cmd)
    {
    case CTRL_SYNC: // writes are synchronous (busy-waited)
      return RES_OK;
    case GET_SECTOR_COUNT:
      *static_cast<LBA_t*> (buff) = g_vol_blocks;
      return RES_OK;
    case GET_SECTOR_SIZE:
      *static_cast<WORD*> (buff) = 512;
      return RES_OK;
    case GET_BLOCK_SIZE:
      *static_cast<DWORD*> (buff) = 1; // erase-block size unknown
      return RES_OK;
    default:
      return RES_PARERR;
    }
}
