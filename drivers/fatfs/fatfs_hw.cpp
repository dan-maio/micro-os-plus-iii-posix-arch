/**
 * @file fatfs_hw.cpp
 * @brief Implementation of the hardware SD-card test helper (see fatfs_hw.hpp).
 *
 * All test files live under "tests/" in the existing FAT32 boot partition.
 */

#include "fatfs_hw.hpp"

#include "ff.h"
#include "diskio.h"
#include "sd.hpp"

extern "C" void fatfs_use_card (void* card);
extern "C" void fatfs_window (std::uint32_t* base, std::uint32_t* blocks);

#include <cstdio>
#include <cstring>

namespace
{
  sd::SdCard* g_card = nullptr;
  bool g_bound = false;
  FATFS g_fs;                 // FatFs work area for the single volume
  bool g_mounted = false;
  bool g_dir_ok = false;
  FRESULT g_last = FR_OK;

  const char*
  fres_str (FRESULT fr)
  {
    switch (fr)
      {
      case FR_OK:
        return "ok";
      case FR_DISK_ERR:
        return "disk error";
      case FR_INT_ERR:
        return "internal error";
      case FR_NOT_READY:
        return "drive not ready";
      case FR_NO_FILE:
        return "no such file";
      case FR_NO_PATH:
        return "no such path";
      case FR_INVALID_NAME:
        return "invalid name";
      case FR_DENIED:
        return "access denied";
      case FR_EXIST:
        return "already exists";
      case FR_INVALID_OBJECT:
        return "invalid object";
      case FR_WRITE_PROTECTED:
        return "write protected";
      case FR_INVALID_DRIVE:
        return "invalid drive";
      case FR_NOT_ENABLED:
        return "volume not mounted";
      case FR_NO_FILESYSTEM:
        return "no FAT filesystem";
      case FR_MKFS_ABORTED:
        return "mkfs aborted";
      case FR_TIMEOUT:
        return "timeout";
      case FR_LOCKED:
        return "locked";
      case FR_NOT_ENOUGH_CORE:
        return "out of memory";
      case FR_TOO_MANY_OPEN_FILES:
        return "too many open files";
      case FR_INVALID_PARAMETER:
        return "invalid parameter";
      default:
        return "unknown";
      }
  }

  bool
  fat_ok (FRESULT fr)
  {
    g_last = fr;
    return fr == FR_OK;
  }
} // namespace

namespace fatfshw
{

void
bind_card (sd::SdCard& card)
{
  g_card = &card;
  g_bound = true;
}

bool
mount_volume (void)
{
  if (g_mounted)
    {
      return true;
    }
  if (!g_bound)
    {
      g_last = FR_NOT_READY;
      return false;
    }
  fatfs_use_card (g_card);

  // Drive 0, "0:" == "" (single volume). f_mount(...,1) forces a remount of
  // whatever filesystem is ALREADY on the card -- it never formats.
  if (!fat_ok (f_mount (&g_fs, "", 1)))
    {
      return false;
    }
  g_mounted = true;
  return true;
}

bool
ensure_tests_dir (void)
{
  if (g_dir_ok)
    {
      return true;
    }
  if (!g_mounted)
    {
      return false;
    }
  // Create the folder only if it is missing; existing contents are kept.
  FRESULT fr = f_mkdir ("tests");
  if (fr == FR_EXIST)
    {
      fr = FR_OK;
    }
  if (!fat_ok (fr))
    {
      return false;
    }
  g_dir_ok = true;
  return true;
}

bool
store_file (const char* leaf, const void* data, std::uint32_t size)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);

  FIL f;
  if (!fat_ok (f_open (&f, path, FA_CREATE_ALWAYS | FA_WRITE)))
    {
      return false;
    }
  UINT written = 0;
  FRESULT fr = f_write (&f, data, size, &written);
  if (fr == FR_OK && written != size)
    {
      fr = FR_DISK_ERR;
    }
  f_close (&f);
  return fat_ok (fr);
}

bool
append_file (const char* leaf, const void* data, std::uint32_t size)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);

  // Create if missing, else append (FA_OPEN_APPEND seeks to EOF on open).
  FIL f;
  if (!fat_ok (f_open (&f, path, FA_OPEN_APPEND | FA_WRITE)))
    {
      return false;
    }
  UINT written = 0;
  FRESULT fr = f_write (&f, data, size, &written);
  if (fr == FR_OK && written != size)
    {
      fr = FR_DISK_ERR;
    }
  f_close (&f);
  return fat_ok (fr);
}

bool
load_file_at (const char* leaf, std::uint32_t offset, void* data,
              std::uint32_t size)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);

  FIL f;
  if (!fat_ok (f_open (&f, path, FA_READ)))
    {
      return false;
    }
  FRESULT fr = f_lseek (&f, offset);
  if (fr == FR_OK && f_tell (&f) != offset)
    {
      fr = FR_DISK_ERR; // seek past EOF
    }
  UINT got = 0;
  if (fr == FR_OK)
    {
      fr = f_read (&f, data, size, &got);
      if (fr == FR_OK && got != size)
        {
          fr = FR_DISK_ERR;
        }
    }
  f_close (&f);
  return fat_ok (fr);
}

bool
load_file (const char* leaf, void* data, std::uint32_t size)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);

  FIL f;
  if (!fat_ok (f_open (&f, path, FA_READ)))
    {
      return false;
    }
  UINT got = 0;
  FRESULT fr = f_read (&f, data, size, &got);
  if (fr == FR_OK && got != size)
    {
      fr = FR_DISK_ERR;
    }
  f_close (&f);
  return fat_ok (fr);
}

bool
file_size (const char* leaf, std::uint32_t& size)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);

  FILINFO fi;
  std::memset (&fi, 0, sizeof (fi));
  if (!fat_ok (f_stat (path, &fi)))
    {
      return false;
    }
  size = static_cast<std::uint32_t> (fi.fsize);
  return true;
}

bool
remove_file (const char* leaf)
{
  if (!g_mounted || !g_dir_ok)
    {
      return false;
    }
  char path[64];
  std::snprintf (path, sizeof (path), "tests/%s", leaf);
  return fat_ok (f_unlink (path));
}

bool
volume_space_kib (std::uint32_t& total_kib, std::uint32_t& free_kib)
{
  if (!g_mounted)
    {
      return false;
    }
  DWORD free_clst = 0;
  FATFS* pfs = nullptr;
  if (!fat_ok (f_getfree ("", &free_clst, &pfs)))
    {
      return false;
    }
  // Sectors first, then >> 1 to KiB. Multiplying out to BYTES overflows 32
  // bits on any volume past 4 GB - a 32 GB card used to report 1.7 GiB.
  const std::uint32_t total_sect
      = static_cast<std::uint32_t> (pfs->n_fatent - 2) * pfs->csize;
  const std::uint32_t free_sect
      = static_cast<std::uint32_t> (free_clst) * pfs->csize;
  total_kib = total_sect >> 1; // 512-byte sectors
  free_kib = free_sect >> 1;
  return true;
}

unsigned
visit_tests_dir (EntryVisitor v, void* ctx)
{
  if (v == nullptr || !g_mounted || !g_dir_ok)
    {
      return 0;
    }
  DIR d;
  if (!fat_ok (f_opendir (&d, "tests")))
    {
      return 0;
    }
  unsigned count = 0;
  FILINFO fi;
  for (;;)
    {
      std::memset (&fi, 0, sizeof (fi));
      if (!fat_ok (f_readdir (&d, &fi)) || fi.fname[0] == '\0')
        {
          break;
        }
      if (fi.fattrib & AM_DIR)
        {
          continue;
        }
      ++count;
      if (!v (fi.fname, static_cast<std::uint32_t> (fi.fsize), ctx))
        {
          break;
        }
    }
  f_closedir (&d);
  return count;
}

unsigned
list_tests_dir (char* buf, std::uint32_t cap)
{
  if (buf == nullptr || cap == 0)
    {
      return 0;
    }
  buf[0] = '\0';
  if (!g_mounted || !g_dir_ok)
    {
      return 0;
    }
  DIR d;
  if (!fat_ok (f_opendir (&d, "tests")))
    {
      return 0;
    }
  std::uint32_t off = 0;
  unsigned count = 0;
  FILINFO fi;
  for (;;)
    {
      std::memset (&fi, 0, sizeof (fi));
      if (!fat_ok (f_readdir (&d, &fi)) || fi.fname[0] == '\0')
        {
          break;
        }
      if (fi.fattrib & AM_DIR)
        {
          continue;
        }
      const int n = std::snprintf (buf + off, cap - off, "%-12s  %8u\n",
                                   fi.fname,
                                   static_cast<unsigned> (fi.fsize));
      if (n < 0 || static_cast<std::uint32_t> (n) >= cap - off)
        {
          break;
        }
      off += static_cast<std::uint32_t> (n);
      ++count;
    }
  f_closedir (&d);
  return count;
}

void
partition_window (std::uint32_t& base_lba, std::uint32_t& blocks)
{
  base_lba = 0;
  blocks = 0;
  if (g_bound)
    {
      // Make sure the MBR scan has run even if mount_volume() was not called
      // yet (or failed before FatFs got involved).
      fatfs_use_card (g_card);
    }
  fatfs_window (&base_lba, &blocks);
}

bool
volume_geometry (const char*& fat_type, std::uint32_t& sec_per_clus,
                 std::uint32_t& clusters)
{
  fat_type = "?";
  sec_per_clus = 0;
  clusters = 0;
  if (!g_mounted)
    {
      return false;
    }
  switch (g_fs.fs_type)
    {
    case FS_FAT12:
      fat_type = "FAT12";
      break;
    case FS_FAT16:
      fat_type = "FAT16";
      break;
    case FS_FAT32:
      fat_type = "FAT32";
      break;
    case FS_EXFAT:
      fat_type = "exFAT";
      break;
    default:
      break;
    }
  sec_per_clus = g_fs.csize;
  clusters = (g_fs.n_fatent >= 2) ? (g_fs.n_fatent - 2) : 0;
  return true;
}

const char*
last_error (void)
{
  return fres_str (g_last);
}

} // namespace fatfshw
