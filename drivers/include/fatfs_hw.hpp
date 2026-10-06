/**
 * @file  fatfs_hw.hpp
 * @brief FatFs-based SD file helper for the Raspberry Pi Zero 2W hardware
 *        SD-card tests.
 *
 * Used ONLY by the hardware build (make HW=1). Unlike the QEMU builds, which
 * attach a raw disk image and use the custom `flatfs` on top of the raw
 * sectors, on real hardware the microSD is the BOOT card: it carries an MBR +
 * FAT32 partition with config.txt / kernel8.img (or kernel7.img) / ... at its root. The tests
 * must therefore:
 *
 *   - mount the EXISTING FAT32 partition (never format / wipe it);
 *   - keep every test file under a "tests" folder so the boot files at the
 *     FAT root are never touched.
 *
 * Every file the tests write/read lives at "tests/<leaf>" inside the boot
 * partition. Names must be short (8.3) because FF_USE_LFN is 0.
 *
 * All SD I/O happens from one core / one task at a time (the tests do their
 * card work on core 0 before the solver cores are released, or on the single
 * core in sd_test), so no re-entrancy is needed.
 */
#pragma once

#include <cstdint>

namespace sd
{
  class SdCard;
}

namespace fatfshw
{
  /// Register an already-initialised card (see diskio_sd.cpp). Idempotent.
  void bind_card (sd::SdCard& card);

  /// Bind the current card if not done yet and mount the existing FAT volume.
  /// Returns false (and prints why on the UART) on failure.
  bool mount_volume (void);

  /// Create the "tests" folder if it does not exist yet. No-op if present.
  bool ensure_tests_dir (void);

  /// Write `size` bytes from `data` to "tests/<leaf>" (created/overwritten).
  bool store_file (const char* leaf, const void* data, std::uint32_t size);

  /// Append `size` bytes to `tests/leaf` (creates it if missing).
  bool append_file (const char* leaf, const void* data, std::uint32_t size);

  /// Read exactly `size` bytes of "tests/<leaf>" into `data`.
  bool load_file (const char* leaf, void* data, std::uint32_t size);

  /// Read `size` bytes of `/tests/<leaf>` starting at byte `offset`.
  /// Companion of append_file(): lets a reader follow a growing log file
  /// without holding all of it in RAM.
  bool load_file_at (const char* leaf, std::uint32_t offset, void* data,
                     std::uint32_t size);

  /// Current size of "tests/<leaf>"; false if missing.
  bool file_size (const char* leaf, std::uint32_t& size);

  /// Delete "tests/<leaf>".
  bool remove_file (const char* leaf);

  /// Total / free space of the mounted FAT volume, in KiB. KiB rather than
  /// bytes because a byte count overflows 32 bits on any card past 4 GB: a
  /// 32 GB volume reported 1825904 KiB instead of 31186032 KiB before this.
  bool volume_space_kib (std::uint32_t& total_kib, std::uint32_t& free_kib);

  /// Append "name  <bytes>\n" lines of every entry in tests/ to `buf`
  /// (NUL-terminated, at most `cap` bytes). Returns number of entries.
  unsigned list_tests_dir (char* buf, std::uint32_t cap);

  /// Callback for visit_tests_dir(). Return false to stop the walk early.
  using EntryVisitor = bool (*) (const char* name, std::uint32_t size,
                                 void* ctx);

  /// Enumerate every regular file in tests/, one call per entry. The
  /// companion of list_tests_dir() for callers that want the entries
  /// themselves rather than the formatted text. Returns entries visited.
  unsigned visit_tests_dir (EntryVisitor v, void* ctx);

  /// Geometry of the FAT window the glue found in the MBR, in 512-byte
  /// sectors. Valid after the first mount attempt, successful or not — the
  /// scan happens before FatFs is involved, so this is the key diagnostic
  /// when a mount fails on an unfamiliar card.
  void partition_window (std::uint32_t& base_lba, std::uint32_t& blocks);

  /// FAT sub-type ("FAT12"/"FAT16"/"FAT32"/"exFAT"/"?"), sectors per cluster
  /// and cluster count of the mounted volume. Only valid once mounted.
  bool volume_geometry (const char*& fat_type, std::uint32_t& sec_per_clus,
                        std::uint32_t& clusters);

  /// Human readable reason of the last failed operation.
  const char* last_error (void);
} // namespace fatfshw
