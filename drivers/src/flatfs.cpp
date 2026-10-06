/*
 * flatfs.cpp — tiny flat filesystem over a raw 512-byte sector device.
 *
 * Layout (see flatfs.hpp):
 *   block 0            : superblock
 *   blocks 1..B        : allocation bitmap (1 bit / data block)
 *   blocks 1+B..ds-1   : file table (16 entries per block)
 *   blocks ds..end     : file data
 *
 * The whole metadata set is small enough to be cached in RAM (the bitmap is
 * the only large object: ~768 KiB on a 3 GiB card). Modifications mark the
 * touched bitmap sectors dirty and are flushed back through the SD driver, so
 * each write_file()/remove_file() leaves the image consistent.
 */
#include <cstring>
#include <new>

#include "flatfs.hpp"

namespace flatfs
{

namespace
{
// Superblock field offsets (within a 512-byte sector).
constexpr std::uint32_t SB_MAGIC = 0;
constexpr std::uint32_t SB_VERSION = 8;
constexpr std::uint32_t SB_BLOCK_SIZE = 12;
constexpr std::uint32_t SB_TOTAL = 16;
constexpr std::uint32_t SB_DATA_START = 20;
constexpr std::uint32_t SB_BITMAP_BLOCKS = 24;
constexpr std::uint32_t SB_TABLE_BLOCKS = 28;
constexpr std::uint32_t SB_MAX_FILES = 32;
constexpr std::uint32_t SB_FREE = 36;

constexpr std::uint32_t kLayoutVersion = 1;

// Entry field offsets (32-byte entries).
constexpr std::uint32_t E_NAME = 0;
constexpr std::uint32_t E_START = 20;
constexpr std::uint32_t E_SIZE = 24;
constexpr std::uint32_t E_FLAGS = 28;

constexpr std::uint32_t kFlagValid = 1u;

constexpr std::uint32_t kBitsPerSector = 512u * 8u; // 4096

// 8 on-disk magic bytes "FLATFS01".
constexpr char kMagicStr[9] = "FLATFS01";
} // namespace

const char*
FlatFs::result_str (Result r) noexcept
{
  switch (r)
    {
    case Result::ok:         return "ok";
    case Result::err_io:     return "I/O error";
    case Result::err_no_fs:  return "not a flatfs volume";
    case Result::err_format: return "layout error";
    case Result::err_no_space: return "no free space";
    case Result::err_not_found: return "file not found";
    case Result::err_exists: return "file exists";
    case Result::err_full:   return "file table full";
    case Result::err_name:   return "bad file name";
    case Result::err_media:  return "refusing to format: media looks like a DOS MBR/FAT boot volume (real SD boot card?)";
    case Result::err_buf:    return "buffer too small";
    }
  return "?";
}

void
FlatFs::store_le32 (std::uint8_t* p, std::uint32_t v) const noexcept
{
  p[0] = static_cast<std::uint8_t> (v);
  p[1] = static_cast<std::uint8_t> (v >> 8);
  p[2] = static_cast<std::uint8_t> (v >> 16);
  p[3] = static_cast<std::uint8_t> (v >> 24);
}

std::uint32_t
FlatFs::load_le32 (const std::uint8_t* p) const noexcept
{
  return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
         | (static_cast<std::uint32_t> (p[2]) << 16)
         | (static_cast<std::uint32_t> (p[3]) << 24);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
FlatFs::Geometry
FlatFs::geometry_for (std::uint32_t total_blocks) noexcept
{
  Geometry g;
  g.total_blocks = total_blocks;
  g.table_blocks = kTableBlocks;
  g.max_files = g.table_blocks * kEntriesPerBlock;

  // Smallest B such that the bitmap (B*4096 bits) covers the data area that
  // remains after the bitmap itself:  data_blocks <= B*4096.
  std::uint32_t b = 1;
  while (static_cast<std::uint64_t> (b) * kBitsPerSector
         < static_cast<std::uint64_t> (total_blocks) - 1u - g.table_blocks - b)
    {
      ++b;
    }
  g.bitmap_blocks = b;
  g.data_start = 1u + b + g.table_blocks;
  if (g.data_start < total_blocks)
    {
      g.data_blocks = total_blocks - g.data_start;
    }
  else
    {
      g.data_blocks = 0;
    }
  return g;
}

// ---------------------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------------------
void
FlatFs::release_buffers ()
{
  delete[] bitmap_;
  delete[] dirty_;
  delete[] table_;
  bitmap_ = nullptr;
  dirty_ = nullptr;
  table_ = nullptr;
}

bool
FlatFs::ensure_buffers ()
{
  if (bitmap_ == nullptr)
    {
      bitmap_ = new std::uint8_t[static_cast<std::size_t> (geom_.bitmap_blocks)
                                 * kBlockSize];
    }
  if (dirty_ == nullptr)
    {
      dirty_ = new std::uint8_t[geom_.bitmap_blocks];
      std::memset (dirty_, 0, geom_.bitmap_blocks);
    }
  if (table_ == nullptr)
    {
      table_ = new std::uint8_t[static_cast<std::size_t> (geom_.table_blocks)
                                * kBlockSize];
    }
  return bitmap_ && dirty_ && table_;
}

// ---------------------------------------------------------------------------
// Superblock
// ---------------------------------------------------------------------------
FlatFs::Result
FlatFs::write_sb ()
{
  std::uint8_t sb[kBlockSize];
  std::memset (sb, 0, sizeof (sb));
  std::memcpy (sb + SB_MAGIC, kMagicStr, 8);
  store_le32 (sb + SB_VERSION, kLayoutVersion);
  store_le32 (sb + SB_BLOCK_SIZE, kBlockSize);
  store_le32 (sb + SB_TOTAL, geom_.total_blocks);
  store_le32 (sb + SB_DATA_START, geom_.data_start);
  store_le32 (sb + SB_BITMAP_BLOCKS, geom_.bitmap_blocks);
  store_le32 (sb + SB_TABLE_BLOCKS, geom_.table_blocks);
  store_le32 (sb + SB_MAX_FILES, geom_.max_files);
  store_le32 (sb + SB_FREE, free_blocks_);

  if (!card_->write_sectors (kSuperBlock, 1, sb))
    {
      return Result::err_io;
    }
  return Result::ok;
}

FlatFs::Result
FlatFs::read_sb ()
{
  std::uint8_t sb[kBlockSize];
  if (!card_->read_sectors (kSuperBlock, 1, sb))
    {
      return Result::err_io;
    }
  if (std::memcmp (sb + SB_MAGIC, kMagicStr, 8) != 0)
    {
      return Result::err_no_fs;
    }
  if (load_le32 (sb + SB_VERSION) != kLayoutVersion)
    {
      return Result::err_no_fs;
    }
  if (load_le32 (sb + SB_BLOCK_SIZE) != kBlockSize)
    {
      return Result::err_no_fs;
    }

  geom_.total_blocks = load_le32 (sb + SB_TOTAL);
  geom_.data_start = load_le32 (sb + SB_DATA_START);
  geom_.bitmap_blocks = load_le32 (sb + SB_BITMAP_BLOCKS);
  geom_.table_blocks = load_le32 (sb + SB_TABLE_BLOCKS);
  geom_.max_files = load_le32 (sb + SB_MAX_FILES);
  geom_.data_blocks = geom_.total_blocks - geom_.data_start;
  free_blocks_ = load_le32 (sb + SB_FREE);

  if (geom_.total_blocks != card_->sector_count ())
    {
      return Result::err_format;
    }
  if (geom_.data_start != 1u + geom_.bitmap_blocks + geom_.table_blocks)
    {
      return Result::err_format;
    }
  if (geom_.max_files != geom_.table_blocks * kEntriesPerBlock)
    {
      return Result::err_format;
    }
  return Result::ok;
}

// ---------------------------------------------------------------------------
// Bitmap
// ---------------------------------------------------------------------------
bool
FlatFs::bit_test (std::uint32_t p) const noexcept
{
  return (bitmap_[p >> 3] & (1u << (p & 7u))) != 0;
}

void
FlatFs::bit_set (std::uint32_t p) noexcept
{
  bitmap_[p >> 3] = static_cast<std::uint8_t> (
      bitmap_[p >> 3] | (1u << (p & 7u)));
  mark_dirty (p);
}

void
FlatFs::bit_clear (std::uint32_t p) noexcept
{
  bitmap_[p >> 3] = static_cast<std::uint8_t> (
      bitmap_[p >> 3] & static_cast<std::uint8_t> (~(1u << (p & 7u))));
  mark_dirty (p);
}

void
FlatFs::mark_dirty (std::uint32_t bitpos) noexcept
{
  dirty_[bitpos >> 12] = 1; // 4096 bits per sector
}

FlatFs::Result
FlatFs::load_bitmap ()
{
  if (!card_->read_sectors (1u, geom_.bitmap_blocks, bitmap_))
    {
      return Result::err_io;
    }
  // Recompute the free count from the bitmap (do not trust the superblock).
  std::uint32_t free_blocks = 0;
  for (std::uint32_t p = 0; p < geom_.data_blocks; ++p)
    {
      if (!bit_test (p))
        {
          ++free_blocks;
        }
    }
  free_blocks_ = free_blocks;
  return Result::ok;
}

FlatFs::Result
FlatFs::flush_bitmap_dirty ()
{
  for (std::uint32_t s = 0; s < geom_.bitmap_blocks; ++s)
    {
      if (dirty_[s])
        {
          if (!card_->write_sectors (1u + s, 1u,
                                     bitmap_ + static_cast<std::size_t> (s)
                                         * kBlockSize))
            {
              return Result::err_io;
            }
          dirty_[s] = 0;
        }
    }
  return Result::ok;
}

// ---------------------------------------------------------------------------
// File table
// ---------------------------------------------------------------------------
FlatFs::Result
FlatFs::load_table ()
{
  if (!card_->read_sectors (1u + geom_.bitmap_blocks, geom_.table_blocks,
                            table_))
    {
      return Result::err_io;
    }
  return Result::ok;
}

FlatFs::Result
FlatFs::flush_table ()
{
  if (!card_->write_sectors (1u + geom_.bitmap_blocks, geom_.table_blocks,
                             table_))
    {
      return Result::err_io;
    }
  return Result::ok;
}

int
FlatFs::find_file (const char* name) const noexcept
{
  for (std::uint32_t e = 0; e < geom_.max_files; ++e)
    {
      const std::uint8_t* p = table_ + static_cast<std::size_t> (e) * kEntryBytes;
      if (load_le32 (p + E_FLAGS) & kFlagValid)
        {
          if (std::strncmp (reinterpret_cast<const char*> (p + E_NAME), name,
                            kNameMax + 1)
              == 0)
            {
              return static_cast<int> (e);
            }
        }
    }
  return -1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
FlatFs::Result
FlatFs::format (sd::SdCard& card)
{
  card_ = &card;
  release_buffers ();
  geom_ = geometry_for (card_->sector_count ());
  if (geom_.data_blocks == 0)
    {
      return Result::err_format;
    }

  // Safety: refuse to format media that already looks like a DOS MBR / FAT
  // boot volume (e.g. a real Pi boot SD card) - flatfs would wipe the
  // partition table and boot files. Blank/raw disk images (0x55AA clear)
  // and previously-formatted flatfs volumes are fine.
  {
    std::uint8_t sec0[512];
    if (card_->read_sectors (0, 1, sec0) && sec0[510] == 0x55u
        && sec0[511] == 0xAAu)
      {
        last_ = Result::err_media;
        return last_;
      }
  }
  if (!ensure_buffers ())
    {
      return Result::err_io;
    }

  // Empty metadata: no allocated blocks, empty table.
  std::memset (bitmap_, 0,
               static_cast<std::size_t> (geom_.bitmap_blocks) * kBlockSize);
  std::memset (dirty_, 0, geom_.bitmap_blocks);
  std::memset (table_, 0,
               static_cast<std::size_t> (geom_.table_blocks) * kBlockSize);
  free_blocks_ = geom_.data_blocks;

  // Write the empty bitmap and table regions.
  if (!card_->write_sectors (1u, geom_.bitmap_blocks, bitmap_))
    {
      return Result::err_io;
    }
  if (!card_->write_sectors (1u + geom_.bitmap_blocks, geom_.table_blocks,
                             table_))
    {
      return Result::err_io;
    }
  if (write_sb () != Result::ok)
    {
      return Result::err_io;
    }
  mounted_ = true;
  last_ = Result::ok;
  return Result::ok;
}

FlatFs::Result
FlatFs::mount (sd::SdCard& card)
{
  card_ = &card;
  release_buffers ();
  if (read_sb () != Result::ok)
    {
      last_ = Result::err_no_fs;
      return last_;
    }
  if (!ensure_buffers ())
    {
      return Result::err_io;
    }
  if (load_bitmap () != Result::ok)
    {
      return Result::err_io;
    }
  if (load_table () != Result::ok)
    {
      return Result::err_io;
    }
  mounted_ = true;
  last_ = Result::ok;
  return Result::ok;
}

bool
FlatFs::alloc_blocks (std::uint32_t n, std::uint32_t& start_block)
{
  std::uint32_t run = 0;
  std::uint32_t run_start = 0;
  for (std::uint32_t p = 0; p < geom_.data_blocks; ++p)
    {
      if (bit_test (p))
        {
          run = 0;
          run_start = p + 1u;
        }
      else
        {
          if (run == 0)
            {
              run_start = p;
            }
          ++run;
          if (run == n)
            {
              for (std::uint32_t q = run_start; q <= p; ++q)
                {
                  bit_set (q);
                }
              free_blocks_ -= n;
              start_block = geom_.data_start + run_start;
              return true;
            }
        }
    }
  return false;
}

void
FlatFs::free_blocks (std::uint32_t start_block, std::uint32_t n)
{
  if (n == 0)
    {
      return;
    }
  const std::uint32_t first = start_block - geom_.data_start;
  for (std::uint32_t q = 0; q < n; ++q)
    {
      bit_clear (first + q);
    }
  free_blocks_ += n;
}

FlatFs::Result
FlatFs::write_file (const char* name, const void* data, std::uint32_t size)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  const std::uint32_t len = static_cast<std::uint32_t> (std::strlen (name));
  if (len == 0 || len > kNameMax)
    {
      return Result::err_name;
    }

  int idx = find_file (name);
  if (idx >= 0)
    {
      const std::uint8_t* p
          = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
      const std::uint32_t old_start = load_le32 (p + E_START);
      const std::uint32_t old_size = load_le32 (p + E_SIZE);
      free_blocks (old_start, (old_size + kBlockSize - 1u) / kBlockSize);
    }
  else
    {
      // First free slot.
      for (std::uint32_t e = 0; e < geom_.max_files; ++e)
        {
          if (!(load_le32 (table_ + static_cast<std::size_t> (e) * kEntryBytes
                           + E_FLAGS)
                & kFlagValid))
            {
              idx = static_cast<int> (e);
              break;
            }
        }
      if (idx < 0)
        {
          return Result::err_full;
        }
    }

  const std::uint32_t sectors = (size + kBlockSize - 1u) / kBlockSize;
  std::uint32_t start = 0;
  if (sectors > 0)
    {
      if (!alloc_blocks (sectors, start))
        {
          // Failed: leave the slot empty rather than half-written.
          if (idx >= 0)
            {
              std::uint8_t* q
                  = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
              std::memset (q, 0, kEntryBytes);
            }
          return Result::err_no_space;
        }

      const std::uint8_t* src = static_cast<const std::uint8_t*> (data);
      const std::uint32_t full = size / kBlockSize;
      if (full > 0
          && !card_->write_sectors (start, full, src))
        {
          return Result::err_io;
        }
      const std::uint32_t rem = size % kBlockSize;
      if (rem > 0)
        {
          std::uint8_t sector[kBlockSize];
          std::memset (sector, 0, sizeof (sector));
          std::memcpy (sector, src + static_cast<std::size_t> (full) * kBlockSize,
                       rem);
          if (!card_->write_sectors (start + full, 1, sector))
            {
              return Result::err_io;
            }
        }
    }

  // Fill the table entry.
  std::uint8_t* q = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  std::memset (q, 0, kEntryBytes);
  std::memcpy (q + E_NAME, name, len);
  store_le32 (q + E_START, start);
  store_le32 (q + E_SIZE, size);
  store_le32 (q + E_FLAGS, kFlagValid);

  last_ = flush ();
  return last_;
}

FlatFs::Result
FlatFs::append_file (const char* name, const void* data, std::uint32_t size)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  if (size == 0)
    {
      return Result::ok;
    }

  const int idx = find_file (name);
  if (idx < 0)
    {
      // Nothing to append to yet — the first append is just a write.
      return write_file (name, data, size);
    }

  std::uint8_t* q = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  const std::uint32_t start = load_le32 (q + E_START);
  const std::uint32_t old_size = load_le32 (q + E_SIZE);
  if (old_size == 0)
    {
      // An empty file has no extent (write_file() stores start = 0), so there
      // is nothing to grow in place: allocate one, exactly as a write does.
      return write_file (name, data, size);
    }
  const std::uint32_t new_size = old_size + size;
  if (new_size < old_size)
    {
      return Result::err_no_space; // 32-bit size overflow
    }

  const std::uint32_t old_sectors = (old_size + kBlockSize - 1u) / kBlockSize;
  const std::uint32_t new_sectors = (new_size + kBlockSize - 1u) / kBlockSize;

  // Grow the extent in place. The blocks immediately after the file must be
  // free; flatfs extents are contiguous and this deliberately does not relocate.
  if (new_sectors > old_sectors)
    {
      const std::uint32_t extra = new_sectors - old_sectors;
      const std::uint32_t first = start + old_sectors - geom_.data_start;
      if (first + extra > geom_.data_blocks)
        {
          return Result::err_no_space;
        }
      for (std::uint32_t i = 0; i < extra; ++i)
        {
          if (bit_test (first + i))
            {
              return Result::err_no_space;
            }
        }
      for (std::uint32_t i = 0; i < extra; ++i)
        {
          bit_set (first + i);
        }
      free_blocks_ -= extra;
    }

  const std::uint8_t* src = static_cast<const std::uint8_t*> (data);
  std::uint32_t written = 0;
  std::uint8_t sector[kBlockSize];

  // Head: the file's last sector is usually partial, so it is a
  // read-modify-write; everything after it is written whole.
  const std::uint32_t head_off = old_size % kBlockSize;
  if (head_off != 0)
    {
      const std::uint32_t sec = start + old_size / kBlockSize;
      if (!card_->read_sectors (sec, 1, sector))
        {
          return Result::err_io;
        }
      std::uint32_t n = kBlockSize - head_off;
      if (n > size)
        {
          n = size;
        }
      std::memcpy (sector + head_off, src, n);
      if (!card_->write_sectors (sec, 1, sector))
        {
          return Result::err_io;
        }
      written = n;
    }

  // Body: whole sectors straight from the caller's buffer.
  const std::uint32_t body_start = start + (old_size + written) / kBlockSize;
  const std::uint32_t full = (size - written) / kBlockSize;
  if (full > 0)
    {
      if (!card_->write_sectors (body_start, full, src + written))
        {
          return Result::err_io;
        }
      written += full * kBlockSize;
    }

  // Tail: a final partial sector, zero-padded to the block size.
  const std::uint32_t rem = size - written;
  if (rem > 0)
    {
      const std::uint32_t sec = start + (old_size + written) / kBlockSize;
      std::memset (sector, 0, sizeof (sector));
      std::memcpy (sector, src + written, rem);
      if (!card_->write_sectors (sec, 1, sector))
        {
          return Result::err_io;
        }
    }

  store_le32 (q + E_SIZE, new_size);
  last_ = flush ();
  return last_;
}

FlatFs::Result
FlatFs::read_file_at (const char* name, std::uint32_t offset, void* buf,
                      std::uint32_t cap, std::uint32_t& out_len)
{
  out_len = 0;
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  const int idx = find_file (name);
  if (idx < 0)
    {
      return Result::err_not_found;
    }
  const std::uint8_t* p = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  const std::uint32_t start = load_le32 (p + E_START);
  const std::uint32_t size = load_le32 (p + E_SIZE);
  if (offset >= size)
    {
      return Result::ok; // at/after EOF: no bytes, not an error
    }

  std::uint32_t want = size - offset;
  if (want > cap)
    {
      want = cap;
    }

  // Read whole sectors directly into the caller's buffer, then slide the
  // payload down over the leading partial sector. The buffer therefore has to
  // hold the sector-aligned span, so trim `want` until it does.
  const std::uint32_t sec_off = offset % kBlockSize;
  while (want > 0
         && ((sec_off + want + kBlockSize - 1u) / kBlockSize) * kBlockSize > cap)
    {
      want -= kBlockSize;
    }
  if (want == 0)
    {
      return Result::err_buf;
    }

  const std::uint32_t sec_first = start + offset / kBlockSize;
  const std::uint32_t nsec = (sec_off + want + kBlockSize - 1u) / kBlockSize;
  std::uint8_t* dst = static_cast<std::uint8_t*> (buf);
  if (!card_->read_sectors (sec_first, nsec, dst))
    {
      return Result::err_io;
    }
  if (sec_off != 0)
    {
      std::memmove (dst, dst + sec_off, want);
    }
  out_len = want;
  return Result::ok;
}

FlatFs::Result
FlatFs::read_file (const char* name, void* buf, std::uint32_t cap,
                   std::uint32_t* out_size)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  const int idx = find_file (name);
  if (idx < 0)
    {
      return Result::err_not_found;
    }
  const std::uint8_t* p
      = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  const std::uint32_t start = load_le32 (p + E_START);
  const std::uint32_t size = load_le32 (p + E_SIZE);
  if (out_size)
    {
      *out_size = size;
    }
  if (size == 0)
    {
      return Result::ok;
    }
  if (cap < size)
    {
      return Result::err_buf;
    }

  const std::uint32_t sectors = (size + kBlockSize - 1u) / kBlockSize;
  const std::uint32_t rounded = sectors * kBlockSize;

  if (cap >= rounded)
    {
      // Caller gave headroom for whole sectors: single (possibly multi-block)
      // read straight into the buffer.
      if (!card_->read_sectors (start, sectors, buf))
        {
          return Result::err_io;
        }
    }
  else
    {
      // Safe path: copy only the meaningful bytes, never past `cap`.
      std::uint8_t sector[kBlockSize];
      std::uint8_t* dst = static_cast<std::uint8_t*> (buf);
      std::uint32_t left = size;
      for (std::uint32_t s = 0; s < sectors; ++s)
        {
          if (!card_->read_sectors (start + s, 1, sector))
            {
              return Result::err_io;
            }
          const std::uint32_t take = left < kBlockSize ? left : kBlockSize;
          std::memcpy (dst + static_cast<std::size_t> (s) * kBlockSize, sector,
                       take);
          left -= take;
        }
    }
  return Result::ok;
}

FlatFs::Result
FlatFs::file_size (const char* name, std::uint32_t& size)
{
  const int idx = find_file (name);
  if (idx < 0)
    {
      return Result::err_not_found;
    }
  const std::uint8_t* p
      = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  size = load_le32 (p + E_SIZE);
  return Result::ok;
}

FlatFs::Result
FlatFs::remove_file (const char* name)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  const int idx = find_file (name);
  if (idx < 0)
    {
      return Result::err_not_found;
    }
  std::uint8_t* p = table_ + static_cast<std::size_t> (idx) * kEntryBytes;
  const std::uint32_t start = load_le32 (p + E_START);
  const std::uint32_t size = load_le32 (p + E_SIZE);
  free_blocks (start, (size + kBlockSize - 1u) / kBlockSize);
  std::memset (p, 0, kEntryBytes);
  last_ = flush ();
  return last_;
}

FlatFs::Result
FlatFs::stat (std::uint32_t& total_sectors, std::uint32_t& free_sectors,
              std::uint32_t& file_count)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  total_sectors = geom_.total_blocks;
  free_sectors = free_blocks_;
  file_count = 0;
  for (std::uint32_t e = 0; e < geom_.max_files; ++e)
    {
      if (load_le32 (table_ + static_cast<std::size_t> (e) * kEntryBytes
                     + E_FLAGS)
          & kFlagValid)
        {
          ++file_count;
        }
    }
  return Result::ok;
}

FlatFs::Result
FlatFs::list (FileVisitor visitor, void* ctx)
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  for (std::uint32_t e = 0; e < geom_.max_files; ++e)
    {
      const std::uint8_t* p
          = table_ + static_cast<std::size_t> (e) * kEntryBytes;
      if (load_le32 (p + E_FLAGS) & kFlagValid)
        {
          FileEntry fe;
          std::memcpy (fe.name, p + E_NAME, kNameMax + 1);
          fe.name[kNameMax] = '\0';
          fe.start_block = load_le32 (p + E_START);
          fe.size_bytes = load_le32 (p + E_SIZE);
          fe.valid = true;
          if (visitor && !visitor (fe, ctx))
            {
              break;
            }
        }
    }
  return Result::ok;
}

FlatFs::Result
FlatFs::flush ()
{
  if (!mounted_)
    {
      return Result::err_no_fs;
    }
  if (flush_table () != Result::ok)
    {
      return Result::err_io;
    }
  if (flush_bitmap_dirty () != Result::ok)
    {
      return Result::err_io;
    }
  if (write_sb () != Result::ok)
    {
      return Result::err_io;
    }
  return Result::ok;
}

} // namespace flatfs
