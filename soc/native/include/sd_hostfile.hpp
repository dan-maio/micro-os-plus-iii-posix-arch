/**
 * @file  sd_hostfile.hpp
 * @brief The POSIX host back end behind `sd::SdCard`: a file is the card.
 *
 * The third back end, after the BCM2837's Arasan SDHCI and the RK3506's
 * DW-MMC, and it arrives the way sd.hpp says a back end must: as a new FILE,
 * changing nothing either of the other two compiles.
 *
 * There is no silicon to bring up, so there is no bring-up sequence to get
 * wrong -- but the CONTRACT is the whole point and is honoured exactly:
 * 512-byte sectors, a sector count read once at init(), range-checked
 * transfers, and the same eight methods with the same names and the same
 * failure vocabulary. flatfs, FatFs' diskio and the tests name only those,
 * so smp-num-test and smp-pipeline-test compile and run here with no edit at
 * all -- which is what makes them worth carrying.
 *
 * Which file, and how big:
 *
 *   UOS_SD_IMAGE   path of the backing image   (default "disk.img")
 *   UOS_SD_SECTORS sectors to create it with   (default 65536 = 32 MiB)
 *
 * The image is created if missing and never deleted, so a run can be examined
 * afterwards with any ordinary tool -- which is a genuine advantage of this
 * back end over the two real ones, not a compromise.
 */
#pragma once

#include <cstdint>

namespace sd
{
  // 512-byte logical sector. Declared here as well as in sd.hpp's SDHCI
  // branch, because exactly one of the three branches is ever compiled.
  inline constexpr std::uint32_t kSectorSize = 512u;

  class SdCard
  {
  public:
    // Same names as the other two back ends', so a caller that switches
    // machines does not switch vocabulary.
    enum class Result
    {
      ok,
      err_no_card,
      err_timeout,
      err_crc,
      err_cmd,
      err_voltage,
      err_unsupported,
      err_range,
    };

    SdCard () = default;
    SdCard (const SdCard&) = delete;
    SdCard& operator= (const SdCard&) = delete;

    bool init ();

    [[nodiscard]] std::uint32_t sector_count () const noexcept
    {
      return sectors_;
    }

    [[nodiscard]] std::uint64_t capacity_bytes () const noexcept
    {
      return static_cast<std::uint64_t> (sectors_) * kSectorSize;
    }

    // Always true: an image file is addressed by block, which is what
    // high-capacity means to every caller of this.
    [[nodiscard]] bool high_capacity () const noexcept { return true; }

    [[nodiscard]] bool ready () const noexcept { return ready_; }

    [[nodiscard]] const char* last_error () const noexcept { return err_; }

    [[nodiscard]] Result last_result () const noexcept { return result_; }

    bool read_sectors (std::uint32_t lba, std::uint32_t count, void* dst);

    bool write_sectors (std::uint32_t lba, std::uint32_t count,
                        const void* src);

  private:
    void set_result (Result r, const char* msg);
    void mark_ok ();

    int fd_ = -1;
    bool ready_ = false;
    std::uint32_t sectors_ = 0;
    Result result_ = Result::ok;
    const char* err_ = "no error";
  };

} // namespace sd
