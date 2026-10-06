/*
 * sd_hostfile.cpp - see sd_hostfile.hpp.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

#if defined(SD_BACKEND_HOSTFILE)

#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <sd.hpp>

namespace
{
  constexpr std::uint32_t kDefaultSectors = 65536u; // 32 MiB

  const char*
  image_path (void)
  {
    const char* p = std::getenv ("UOS_SD_IMAGE");
    return (p != nullptr && p[0] != '\0') ? p : "disk.img";
  }

  std::uint32_t
  image_sectors (void)
  {
    const char* s = std::getenv ("UOS_SD_SECTORS");
    if (s == nullptr || s[0] == '\0')
      {
        return kDefaultSectors;
      }
    long v = std::strtol (s, nullptr, 0);
    return (v > 0) ? static_cast<std::uint32_t> (v) : kDefaultSectors;
  }
} // namespace

namespace sd
{
  void
  SdCard::set_result (Result r, const char* msg)
  {
    result_ = r;
    err_ = msg;
  }

  void
  SdCard::mark_ok ()
  {
    result_ = Result::ok;
    err_ = "no error";
  }

  bool
  SdCard::init ()
  {
    ready_ = false;

    const char* path = image_path ();

    fd_ = ::open (path, O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
      {
        set_result (Result::err_no_card, "cannot open the backing image");
        return false;
      }

    struct stat st;
    if (::fstat (fd_, &st) != 0)
      {
        set_result (Result::err_no_card, "cannot stat the backing image");
        return false;
      }

    std::uint64_t bytes = static_cast<std::uint64_t> (st.st_size);
    const std::uint64_t want
        = static_cast<std::uint64_t> (image_sectors ()) * kSectorSize;

    // Grow, never shrink: an existing image is a card that already has
    // something on it, and truncating it would silently destroy the very
    // state a re-run is meant to find.
    if (bytes < want)
      {
        if (::ftruncate (fd_, static_cast<off_t> (want)) != 0)
          {
            set_result (Result::err_no_card, "cannot size the backing image");
            return false;
          }
        bytes = want;
      }

    sectors_ = static_cast<std::uint32_t> (bytes / kSectorSize);
    if (sectors_ == 0)
      {
        set_result (Result::err_unsupported, "backing image holds no sector");
        return false;
      }

    ready_ = true;
    mark_ok ();
    return true;
  }

  bool
  SdCard::read_sectors (std::uint32_t lba, std::uint32_t count, void* dst)
  {
    if (!ready_)
      {
        set_result (Result::err_no_card, "card not initialised");
        return false;
      }
    // The same bound the two silicon back ends enforce, for the same reason:
    // a filesystem bug must stop here, not read whatever follows the image.
    if (count == 0 || lba > sectors_ || count > sectors_ - lba)
      {
        set_result (Result::err_range, "LBA beyond the end of the card");
        return false;
      }

    auto* p = static_cast<char*> (dst);
    std::size_t left = static_cast<std::size_t> (count) * kSectorSize;
    off_t off = static_cast<off_t> (lba) * kSectorSize;

    while (left > 0)
      {
        ssize_t n = ::pread (fd_, p, left, off);
        if (n <= 0)
          {
            set_result (Result::err_timeout, "short read from the image");
            return false;
          }
        p += n;
        off += n;
        left -= static_cast<std::size_t> (n);
      }

    mark_ok ();
    return true;
  }

  bool
  SdCard::write_sectors (std::uint32_t lba, std::uint32_t count,
                         const void* src)
  {
    if (!ready_)
      {
        set_result (Result::err_no_card, "card not initialised");
        return false;
      }
    if (count == 0 || lba > sectors_ || count > sectors_ - lba)
      {
        set_result (Result::err_range, "LBA beyond the end of the card");
        return false;
      }

    const auto* p = static_cast<const char*> (src);
    std::size_t left = static_cast<std::size_t> (count) * kSectorSize;
    off_t off = static_cast<off_t> (lba) * kSectorSize;

    while (left > 0)
      {
        ssize_t n = ::pwrite (fd_, p, left, off);
        if (n <= 0)
          {
            set_result (Result::err_timeout, "short write to the image");
            return false;
          }
        p += n;
        off += n;
        left -= static_cast<std::size_t> (n);
      }

    mark_ok ();
    return true;
  }

} // namespace sd

#endif /* defined(SD_BACKEND_HOSTFILE) */
