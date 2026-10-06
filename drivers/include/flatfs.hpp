/**
 * @file  flatfs.hpp
 * @brief A tiny "flat" filesystem stored on a raw 512-byte-sector block
 *        device (here: the SD driver in sd.hpp).
 *
 * Design (no directories, no FAT — a *flat* table of files):
 *
 *   block 0               : superblock (magic, geometry, free count)
 *   blocks 1 .. 1+B-1     : allocation bitmap for the data region
 *   blocks 1+B .. ds-1    : file table, max_files = table_blocks * 16
 *   blocks ds .. end      : file data (each file is a contiguous run)
 *
 * Each file-table entry (32 bytes) is:
 *     name[20]  u32 start_block  u32 size_bytes  u32 flags (bit0 = valid)
 *
 * Files are contiguous, so a file is located and read by its start block and
 * byte size. Writing a file replaces any existing one (frees its old blocks
 * first); deleting frees its blocks. Free space is a first-fit search over
 * the in-RAM bitmap.
 *
 * The on-disk layout is deliberately simple and is reproduced byte-for-byte
 * by the host tool `sd_test/flatfs_tool.py`, so a Linux host can create a
 * disk image (seed file) that this port mounts, and can verify the files this
 * port writes back onto the image.
 *
 * Little-endian fields throughout (ARM is little-endian).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "sd.hpp"

namespace flatfs
{

inline constexpr std::uint32_t kBlockSize = sd::kSectorSize; // 512
inline constexpr std::uint32_t kSuperBlock = 0;
inline constexpr std::uint32_t kEntryBytes = 32;
inline constexpr std::uint32_t kEntriesPerBlock = kBlockSize / kEntryBytes; // 16
inline constexpr std::uint32_t kTableBlocks = 16; // -> 256 files max
inline constexpr std::uint32_t kNameMax = 19;     // bytes in name[], excl NUL
inline constexpr char kMagic[9] = "FLATFS01";     // 8-byte superblock magic

struct FileEntry
{
  char name[kNameMax + 1]; // NUL-terminated in RAM
  std::uint32_t start_block = 0;
  std::uint32_t size_bytes = 0;
  bool valid = false;
};

/// Visitor called for each file during list(). Return false to stop.
using FileVisitor = bool (*) (const FileEntry& file, void* ctx);

class FlatFs
{
public:
  enum class Result
  {
    ok = 0,
    err_io,        // block device I/O failed
    err_no_fs,     // no valid superblock
    err_format,    // geometry/layout inconsistency
    err_no_space,  // not enough contiguous free blocks
    err_not_found, // file does not exist
    err_exists,    // (unused; writes overwrite)
    err_full,      // file table full
    err_name,      // bad file name
    err_media,     // looks like a DOS MBR/FAT volume (refusing to wipe it)
    err_buf,       // caller buffer too small
  };

  static const char* result_str (Result r) noexcept;

  FlatFs () = default;
  FlatFs (const FlatFs&) = delete;
  FlatFs& operator= (const FlatFs&) = delete;

  /// Wipe the card and lay down a fresh empty filesystem.
  Result format (sd::SdCard& card);

  /// Attach to an existing filesystem (validates the superblock).
  Result mount (sd::SdCard& card);

  /// True after a successful format()/mount().
  [[nodiscard]] bool mounted () const noexcept { return mounted_; }

  /// Replace (or create) `name` with `size` bytes from `data`.
  Result write_file (const char* name, const void* data, std::uint32_t size);

  /// Append `size` bytes to `name`, creating it if absent.
  ///
  /// Unlike write_file(), the existing content is not rewritten: only the
  /// sectors the new bytes actually touch are written, so appending is O(size)
  /// rather than O(file size). The file keeps its contiguous extent and simply
  /// grows into the blocks that follow it, which is why this returns
  /// err_no_space when those blocks are already allocated to something else —
  /// there is no relocation. A log-style file written by repeated append_file()
  /// on a freshly formatted volume always has room to grow.
  Result append_file (const char* name, const void* data, std::uint32_t size);

  /// Read `name` into `buf` (capacity `cap`); actual size via `out_size`.
  Result read_file (const char* name, void* buf, std::uint32_t cap,
                    std::uint32_t* out_size);

  /// Read at most `cap` bytes of `name` starting at byte `offset`.
  ///
  /// The companion of append_file(): it lets a reader follow a file that is
  /// still growing without having to hold the whole thing in RAM. Reading at or
  /// past end-of-file is not an error — it yields `out_len` = 0.
  Result read_file_at (const char* name, std::uint32_t offset, void* buf,
                       std::uint32_t cap, std::uint32_t& out_len);

  /// Size of `name`.
  Result file_size (const char* name, std::uint32_t& size);

  /// Delete `name` (free its blocks + table slot).
  Result remove_file (const char* name);

  /// Total sectors, free data sectors and number of files.
  Result stat (std::uint32_t& total_sectors, std::uint32_t& free_sectors,
               std::uint32_t& file_count);

  /// Enumerate files.
  Result list (FileVisitor visitor, void* ctx);

  /// Force-persist any cached metadata (superblock/table/bitmap).
  Result flush ();

private:
  // ---- geometry ------------------------------------------------------
  struct Geometry
  {
    std::uint32_t total_blocks = 0;
    std::uint32_t data_start = 0;   // first file-data block
    std::uint32_t bitmap_blocks = 0;
    std::uint32_t table_blocks = kTableBlocks;
    std::uint32_t data_blocks = 0;  // blocks available for file data
    std::uint32_t max_files = kTableBlocks * kEntriesPerBlock;
  };

  static Geometry geometry_for (std::uint32_t total_blocks) noexcept;

  // ---- on-disk helpers ----------------------------------------------
  void store_le32 (std::uint8_t* p, std::uint32_t v) const noexcept;
  std::uint32_t load_le32 (const std::uint8_t* p) const noexcept;

  Result read_sb ();
  Result write_sb ();
  Result load_table ();
  Result flush_table ();
  Result load_bitmap ();
  Result flush_bitmap_dirty ();
  Result flush_super ();

  // ---- allocation ----------------------------------------------------
  bool bit_test (std::uint32_t p) const noexcept;
  void bit_set (std::uint32_t p) noexcept;
  void bit_clear (std::uint32_t p) noexcept;
  void mark_dirty (std::uint32_t bitpos) noexcept;
  bool alloc_blocks (std::uint32_t n, std::uint32_t& start_block);
  void free_blocks (std::uint32_t start_block, std::uint32_t n);

  int find_file (const char* name) const noexcept;

  // ---- state ---------------------------------------------------------
  sd::SdCard* card_ = nullptr;
  bool mounted_ = false;
  Geometry geom_;
  std::uint8_t* bitmap_ = nullptr;   // 1 bit per data block
  std::uint8_t* dirty_ = nullptr;    // per-bitmap-sector dirty flags
  std::uint8_t* table_ = nullptr;    // max_files * kEntryBytes
  std::uint32_t free_blocks_ = 0;
  Result last_ = Result::ok;

  bool ensure_buffers ();
  void release_buffers ();
};

} // namespace flatfs
