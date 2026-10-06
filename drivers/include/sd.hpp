/**
 * @file  sd.hpp
 * @brief Polled SD card driver for the Raspberry Pi Zero 2W / Pi 3 (BCM2837)
 *        over the Arasan EMMC controller (SDHCI) @ 0x3F300000 — AArch32.
 *
 * The Pi Zero 2W (and Pi 3) route the microSD slot through the on-SoC
 * "EMMC" peripheral, an SD Host Controller (SDHCI spec v3.0). QEMU models
 * the same controller (bcm2835_peripherals -> sdhci @ 0x3F300000) and, by
 * default after reset, attaches the SD card to this controller, so the very
 * same driver runs on real hardware and under `qemu-system-arm -M raspi2b`
 * / `qemu-system-aarch64 -M raspi3b` (attach with `-drive file=x,if=sd`).
 *
 * Implementation notes
 *  - 100 % polling: no DMA, no SDHCI interrupts. Data moves through the
 *    "Buffer Data Port" (PIO) so there is no cache/DMA-coherency work and no
 *    IRQ glue; adequate for a boot / flat-filesystem driver.
 *  - SD v2 (CMD8 + HCS), SDHC block addressing and legacy SDSC byte
 *    addressing are both supported (drives sized <=2 GiB use byte
 *    addressing, >2 GiB use block addressing). Card capacity is read from
 *    the CSD so the filesystem layer can size itself.
 *  - Multi-block transfers (CMD18/CMD25) are terminated with CMD12, matching
 *    the classic open-ended SD protocol the SDHCI PIO path relies on.
 */
#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// A second SoC's SD host arrives as a second FILE, not as edits to this one.
//
// An SoC has one SD controller, so a build has one back end, and the board
// names it. When the board names a different one, this header hands over
// whole and the driver below is not compiled at all -- so adding the RK3506
// changed nothing a Raspberry Pi compiles, links or runs. That is the rule:
// a new board must not be able to disturb a board that already passed.
//
//   (default)            Arasan SDHCI @ 0x3F300000   (BCM2837)  -- this file
//   SD_BACKEND_DWMMC     Synopsys DW-MMC @ 0xFF480000 (RK3506)  -- soc/rk3506/
//   SD_BACKEND_HOSTFILE  an image file on the host    (POSIX)   -- soc/native/
//
// All three define `sd::SdCard` with the same eight methods; flatfs, FatFs'
// diskio and the tests name only that.
//
// The third one has no silicon behind it and is not a simulation of the other
// two: it is a block device that happens to be a file, which is all any of
// them is to the layers above. That is what lets smp-num-test and
// smp-pipeline-test run on the POSIX port with no edit to either test.
// ---------------------------------------------------------------------------
#if defined(SD_BACKEND_DWMMC)

#include <sd_dwmmc.hpp>

#elif defined(SD_BACKEND_HOSTFILE)

#include <sd_hostfile.hpp>

#else

namespace sd {

// 512-byte logical sector.
inline constexpr std::uint32_t kSectorSize = 512u;

/**
 * Polled SDHCI-backed SD card. Not thread safe: use from a single thread
 * (core 0) while no other thread issues card traffic.
 */
class SdCard
{
public:
  enum class Result
  {
    ok,
    err_no_card,      // controller present-state reports no medium
    err_timeout,      // command or data timed out
    err_crc,          // command/data CRC error
    err_cmd,          // command error (illegal / index)
    err_voltage,      // ACMD41 rejected our voltage window
    err_unsupported,  // not an SD card / unknown CSD structure
    err_range,        // LBA+count beyond end of card
  };

  SdCard () = default;
  SdCard (const SdCard&) = delete;
  SdCard& operator= (const SdCard&) = delete;

  /**
   * Power up, clock and identify the card; reads the CSD so sector_count()
   * is valid afterwards. Returns false (see last_result()) if no card is
   * present or it could not be brought to transfer state.
   */
  bool init ();

  /// Total 512-byte sectors reported by the CSD.
  [[nodiscard]] std::uint32_t sector_count () const noexcept
  {
    return sectors_;
  }

  /// Capacity in bytes.
  [[nodiscard]] std::uint64_t capacity_bytes () const noexcept
  {
    return static_cast<std::uint64_t> (sectors_) * kSectorSize;
  }

  /// True when the card uses block addressing (SDHC/SDXC, >2 GiB).
  [[nodiscard]] bool high_capacity () const noexcept { return hc_; }

  /// True when init() has successfully identified a card.
  [[nodiscard]] bool ready () const noexcept { return ready_; }

  /// Human readable reason of the last failed operation.
  [[nodiscard]] const char* last_error () const noexcept { return err_; }

  /// Result code of the last failed operation.
  [[nodiscard]] Result last_result () const noexcept { return result_; }

  /// Read `count` 512-byte sectors starting at `lba` into `dst`.
  /// `dst` may be unaligned (the PIO word loop copies byte-wise).
  bool read_sectors (std::uint32_t lba, std::uint32_t count, void* dst);

  /// Write `count` 512-byte sectors from `src` to logical sector `lba`.
  bool write_sectors (std::uint32_t lba, std::uint32_t count, const void* src);

private:
  // ---- command/response plumbing ---------------------------------------
  enum class Rsp
  {
    none  = 0,  // no response
    r2    = 1,  // 136-bit
    short_= 2,  // 48-bit  (R1/R3/R6/R7)
    shortb= 3,  // 48-bit + busy (R1b)
  };

  bool cmd (std::uint8_t index, std::uint32_t arg, Rsp rsp, bool crc,
            bool data);
  bool wait_cmd_done (std::uint32_t spins);
  std::uint32_t rsp (unsigned n) const;

  // ---- data-path helpers -----------------------------------------------
  bool xfer (bool read, std::uint32_t lba, std::uint32_t count, void* buf);
  bool xfer_single (bool read, std::uint32_t addr, void* buf);
  bool xfer_multi (bool read, std::uint32_t addr, std::uint32_t count,
                   void* buf);
  bool wait_transfer_done ();
  bool send_stop ();

  bool card_init ();   // CMD0..CMD7 (+ACMD41/ACMD6) sequence
  bool read_csd ();    // CMD9, parse capacity into sectors_

  void io_reset_all ();
  void io_power_on ();
  bool io_set_clock (std::uint16_t divisor);
  void io_enable_status ();
  void io_clear_status ();
  bool io_wait_card_present ();

  void set_result (Result r, const char* msg);
  void mark_ok ();

  // ---- state -----------------------------------------------------------
  bool ready_ = false;
  bool hc_ = false;               // SDHC/SDXC block addressing
  std::uint32_t sectors_ = 0;     // CSD-derived 512-byte sector count
  std::uint32_t rca_ = 0;         // relative card address
  Result result_ = Result::ok;
  const char* err_ = "no error";
};

} // namespace sd

#endif // SD_BACKEND_DWMMC
