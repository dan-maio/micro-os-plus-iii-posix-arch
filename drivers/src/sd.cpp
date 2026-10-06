/*
 * sd.cpp — polled Arasan EMMC / SDHCI driver for BCM2837 (Pi Zero 2W, Pi 3).
 *
 * Register map is the standard SD Host Controller (SDHCI) v3 layout that the
 * Arasan "EMMC" block implements, all MMIO below the ARM-physical peripheral
 * base 0x3F000000. Every transfer is PIO through the Buffer Data Port, so no
 * DMA / cache maintenance and no interrupt is required.
 *
 * The QEMU raspi models implement this controller with the same semantics
 * (hw/sd/sdhci.c): commands are latched by a 16-bit write to 0x0E, the data
 * buffer is filled/drained through 0x20, statuses only assert while their
 * status-enable registers are set, and multi-block PIO transfers are bounded
 * by the block count and finished with a transfer-complete flag.
 */
#include <cstdint>
#include <cstring>

#include "sd.hpp"

namespace sd
{

namespace
{

constexpr std::uint32_t kEmmcBase = 0x3F300000u; // bcm::PERIPH_BASE + 0x300000

namespace reg
{
constexpr std::uint32_t BLKSIZE  = kEmmcBase + 0x04; // block size (16-bit)
constexpr std::uint32_t ARG      = kEmmcBase + 0x08; // command argument
constexpr std::uint32_t TRNMOD   = kEmmcBase + 0x0C; // transfer mode (16-bit)
constexpr std::uint32_t CMD      = kEmmcBase + 0x0E; // command reg  (16-bit)
constexpr std::uint32_t RSP0     = kEmmcBase + 0x10; // response[0]
constexpr std::uint32_t BDATA    = kEmmcBase + 0x20; // buffer data port
constexpr std::uint32_t PRNSTS   = kEmmcBase + 0x24; // present state
constexpr std::uint32_t HOSTCTL  = kEmmcBase + 0x28; // host control (byte)
constexpr std::uint32_t PWR      = kEmmcBase + 0x29; // power control (byte)
constexpr std::uint32_t CLK      = kEmmcBase + 0x2C; // clock control (16-bit)
constexpr std::uint32_t TMOUT    = kEmmcBase + 0x2E; // timeout control (byte)
constexpr std::uint32_t SWRST    = kEmmcBase + 0x2F; // software reset (byte)
constexpr std::uint32_t NORINT   = kEmmcBase + 0x30; // normal int status
constexpr std::uint32_t ERRINT   = kEmmcBase + 0x32; // error int status
constexpr std::uint32_t NORINTEN = kEmmcBase + 0x34; // normal int enable
constexpr std::uint32_t ERRINTEN = kEmmcBase + 0x36; // error int enable
constexpr std::uint32_t NORSIG   = kEmmcBase + 0x38; // normal sig enable
constexpr std::uint32_t ERRSIG   = kEmmcBase + 0x3A; // error sig enable
} // namespace reg

namespace bits
{
// present state (0x24)
constexpr std::uint32_t PRS_CMD_INHIBIT  = 1u << 0;
constexpr std::uint32_t PRS_DATA_INHIBIT = 1u << 1;
constexpr std::uint32_t PRS_CARD_PRESENT = 1u << 16;
// host control (0x28)
constexpr std::uint8_t HC_4BITBUS = 0x02;
// clock control (0x2C)
constexpr std::uint16_t CLK_INT_EN     = 0x0001;
constexpr std::uint16_t CLK_INT_STABLE = 0x0002;
constexpr std::uint16_t CLK_SDCLK_EN   = 0x0004;
// software reset (0x2F)
constexpr std::uint8_t RST_ALL = 0x01;
// normal interrupt status / enable
constexpr std::uint16_t NIS_CMDCMP  = 0x0001;
constexpr std::uint16_t NIS_TRSCMP  = 0x0002;
constexpr std::uint16_t NIS_WBUFRDY = 0x0010;
constexpr std::uint16_t NIS_RBUFRDY = 0x0020;
// error interrupt status / enable
constexpr std::uint16_t EIS_CMDTIMEOUT = 0x0001;
constexpr std::uint16_t EIS_CMDERR     = 0x000F;
constexpr std::uint16_t EIS_DATATIMEOUT = 0x0010;
constexpr std::uint16_t EIS_DATACRC     = 0x0020;
constexpr std::uint16_t EIS_DATAEND     = 0x0040;
constexpr std::uint16_t EIS_DATAERR     = 0x0070; // any data-path error
// command register flag bits (low byte of the 16-bit command register)
constexpr std::uint16_t CMD_CRC_CHECK    = 0x0008;
constexpr std::uint16_t CMD_DATA_PRESENT = 0x0020;
// transfer mode (0x0C)
constexpr std::uint16_t TRNS_BLK_CNT_EN = 0x0002;
constexpr std::uint16_t TRNS_READ       = 0x0010;
constexpr std::uint16_t TRNS_MULTI      = 0x0020;
} // namespace bits

constexpr std::uint8_t CMD0  = 0;
constexpr std::uint8_t CMD2  = 2;
constexpr std::uint8_t CMD3  = 3;
constexpr std::uint8_t CMD6  = 6;
constexpr std::uint8_t CMD7  = 7;
constexpr std::uint8_t CMD8  = 8;
constexpr std::uint8_t CMD9  = 9;
constexpr std::uint8_t CMD12 = 12;
constexpr std::uint8_t CMD17 = 17;
constexpr std::uint8_t CMD18 = 18;
constexpr std::uint8_t CMD24 = 24;
constexpr std::uint8_t CMD25 = 25;
constexpr std::uint8_t CMD41 = 41;
constexpr std::uint8_t CMD55 = 55;

inline void dsb () noexcept { __asm__ volatile ("dsb sy" ::: "memory"); }

inline std::uint8_t io8 (std::uint32_t a) noexcept
{
  return *reinterpret_cast<volatile std::uint8_t*> (a);
}
inline std::uint16_t io16 (std::uint32_t a) noexcept
{
  return *reinterpret_cast<volatile std::uint16_t*> (a);
}
inline std::uint32_t io32 (std::uint32_t a) noexcept
{
  return *reinterpret_cast<volatile std::uint32_t*> (a);
}
inline void wo8 (std::uint32_t a, std::uint8_t v) noexcept
{
  *reinterpret_cast<volatile std::uint8_t*> (a) = v;
  dsb ();
}
inline void wo16 (std::uint32_t a, std::uint16_t v) noexcept
{
  *reinterpret_cast<volatile std::uint16_t*> (a) = v;
  dsb ();
}
inline void wo32 (std::uint32_t a, std::uint32_t v) noexcept
{
  *reinterpret_cast<volatile std::uint32_t*> (a) = v;
  dsb ();
}

/// Spin until none of `mask` bits is set in the present-state register.
bool wait_clear_state (std::uint32_t mask, std::uint32_t spins) noexcept
{
  while (spins--)
    {
      if (!(io32 (reg::PRNSTS) & mask))
        {
          return true;
        }
    }
  return false;
}

void delay_loops (std::uint32_t n) noexcept
{
  volatile std::uint32_t c = n;
  while (c != 0u)
    {
      c = c - 1u;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Controller register setup
// ---------------------------------------------------------------------------
void SdCard::io_clear_status ()
{
  wo16 (reg::NORINT, 0xFFFF); // write-1-to-clear normal status
  wo16 (reg::ERRINT, 0xFFFF); // write-1-to-clear error status
}

void SdCard::io_enable_status ()
{
  // Assert every status bit we poll, but keep the actual IRQ lines masked.
  wo16 (reg::NORINTEN, 0xFFFF);
  wo16 (reg::ERRINTEN, 0xFFFF);
  wo16 (reg::NORSIG, 0);
  wo16 (reg::ERRSIG, 0);
  io_clear_status ();
}

void SdCard::io_reset_all ()
{
  wo8 (reg::SWRST, bits::RST_ALL);
  for (std::uint32_t i = 0; i < 0x10000; ++i)
    {
      if (io8 (reg::SWRST) == 0)
        {
          break;
        }
    }
  io_clear_status ();
}

void SdCard::io_power_on ()
{
  // SD bus power on at 3.3 V.
  wo8 (reg::PWR, 0x0Fu);
  delay_loops (0x80000);
}

bool SdCard::io_set_clock (std::uint16_t divisor)
{
  // Internal clock on, then wait for it to stabilise.
  wo16 (reg::CLK, bits::CLK_INT_EN);
  for (std::uint32_t i = 0; i < 0x10000; ++i)
    {
      if (io16 (reg::CLK) & bits::CLK_INT_STABLE)
        {
          break;
        }
    }
  // Enable the SD clock with the requested divisor (bits [15:8]).
  wo16 (reg::CLK, static_cast<std::uint16_t> (
                      (divisor << 8) | bits::CLK_INT_EN | bits::CLK_SDCLK_EN));
  return true;
}

bool SdCard::io_wait_card_present ()
{
  // Tolerate a short settle, but fail fast if the controller insists there is
  // no medium (QEMU reports "removed" when no -drive is attached).
  for (std::uint32_t i = 0; i < 0x100000; ++i)
    {
      if (io32 (reg::PRNSTS) & bits::PRS_CARD_PRESENT)
        {
          return true;
        }
    }
  return false;
}

// ---------------------------------------------------------------------------
// Command layer
// ---------------------------------------------------------------------------
bool SdCard::wait_cmd_done (std::uint32_t spins)
{
  while (spins--)
    {
      const std::uint16_t n = io16 (reg::NORINT);
      const std::uint16_t e = io16 (reg::ERRINT);
      if (e & bits::EIS_CMDERR)
        {
          set_result ((e & bits::EIS_CMDTIMEOUT) ? Result::err_timeout
                                                 : Result::err_cmd,
                      "command error");
          io_clear_status ();
          return false;
        }
      if (n & bits::NIS_CMDCMP)
        {
          wo16 (reg::NORINT, bits::NIS_CMDCMP);
          return true;
        }
    }
  set_result (Result::err_timeout, "command response timeout");
  io_clear_status ();
  return false;
}

bool SdCard::wait_transfer_done ()
{
  std::uint32_t spins = 0x800000;
  while (spins--)
    {
      const std::uint16_t n = io16 (reg::NORINT);
      const std::uint16_t e = io16 (reg::ERRINT);
      if (e & (bits::EIS_CMDERR | bits::EIS_DATAERR))
        {
          set_result (
              (e & bits::EIS_DATATIMEOUT) ? Result::err_timeout
                                          : Result::err_crc,
              (e & bits::EIS_DATATIMEOUT) ? "data timeout at transfer end"
                                          : "data error at transfer end");
          io_clear_status ();
          return false;
        }
      if (n & bits::NIS_TRSCMP)
        {
          wo16 (reg::NORINT, bits::NIS_TRSCMP);
          return true;
        }
    }
  set_result (Result::err_timeout, "transfer complete timeout");
  io_clear_status ();
  return false;
}

bool SdCard::cmd (std::uint8_t index, std::uint32_t arg, Rsp rsp, bool crc,
                  bool data)
{
  // Wait for any previous transfer to release the bus before a new command.
  if (data)
    {
      wait_clear_state (bits::PRS_DATA_INHIBIT, 0x200000);
    }
  wait_clear_state (bits::PRS_CMD_INHIBIT, 0x200000);

  std::uint16_t flags = static_cast<std::uint16_t> (rsp);
  if (crc)
    {
      flags |= bits::CMD_CRC_CHECK;
    }
  if (data)
    {
      flags |= bits::CMD_DATA_PRESENT;
    }

  wo32 (reg::ARG, arg);
  // Command index lives in the high byte of the 16-bit command register; the
  // low byte carries response-type / CRC / data-present flags.
  wo16 (reg::CMD, static_cast<std::uint16_t> ((index << 8) | flags));
  return true;
}

std::uint32_t SdCard::rsp (unsigned n) const
{
  return io32 (reg::RSP0 + 4u * n);
}

// ---------------------------------------------------------------------------
// Block data transfers (PIO). `addr` is already in card addressing units:
// byte address for SDSC, block address for SDHC/SDXC.
// ---------------------------------------------------------------------------
bool SdCard::xfer_single (bool read, std::uint32_t addr, void* buf)
{
  io_clear_status ();
  wo32 (reg::BLKSIZE, (1u << 16) | kSectorSize); // count=1, size=512

  wo16 (reg::TRNMOD, static_cast<std::uint16_t> (
                         bits::TRNS_BLK_CNT_EN
                         | (read ? bits::TRNS_READ : 0)));

  if (!cmd (read ? CMD17 : CMD24, addr, Rsp::short_, true, true))
    {
      return false;
    }

  std::uint8_t* p = static_cast<std::uint8_t*> (buf);
  std::uint32_t spins = 0x800000;
  const std::uint16_t want = read ? bits::NIS_RBUFRDY : bits::NIS_WBUFRDY;
    for (;;)
    {
      const std::uint16_t n = io16 (reg::NORINT);
      const std::uint16_t e = io16 (reg::ERRINT);
      if (e & (bits::EIS_CMDERR | bits::EIS_DATAERR))
        {
          set_result ((e & bits::EIS_DATATIMEOUT) ? Result::err_timeout
                                                  : Result::err_crc,
                      "data error");
          io_clear_status ();
          return false;
        }
      if (n & want)
        {
          break;
        }
      if (spins-- == 0)
        {
          set_result (Result::err_timeout, "buffer ready timeout");
          io_clear_status ();
          return false;
        }
    }

  if (read)
    {
      for (unsigned i = 0; i < kSectorSize / 4u; ++i)
        {
          const std::uint32_t w = io32 (reg::BDATA);
          std::memcpy (p + 4u * i, &w, 4u);
        }
    }
  else
    {
      for (unsigned i = 0; i < kSectorSize / 4u; ++i)
        {
          std::uint32_t w;
          std::memcpy (&w, p + 4u * i, 4u);
          wo32 (reg::BDATA, w);
        }
    }

  return wait_transfer_done ();
}

bool SdCard::xfer_multi (bool read, std::uint32_t addr, std::uint32_t count,
                         void* buf)
{
  io_clear_status ();
  wo32 (reg::BLKSIZE, (count << 16) | kSectorSize);

  wo16 (reg::TRNMOD, static_cast<std::uint16_t> (
                         bits::TRNS_BLK_CNT_EN | bits::TRNS_MULTI
                         | (read ? bits::TRNS_READ : 0)));

  if (!cmd (read ? CMD18 : CMD25, addr, Rsp::short_, true, true))
    {
      return false;
    }

  std::uint8_t* p = static_cast<std::uint8_t*> (buf);
  for (std::uint32_t b = 0; b < count; ++b)
    {
      std::uint8_t* const block = p + b * kSectorSize;

      const std::uint16_t want
          = read ? bits::NIS_RBUFRDY : bits::NIS_WBUFRDY;
      std::uint32_t spins = 0x800000;
      for (;;)
        {
          const std::uint16_t n = io16 (reg::NORINT);
          const std::uint16_t e = io16 (reg::ERRINT);
          if (e & (bits::EIS_CMDERR | bits::EIS_DATAERR))
            {
              set_result ((e & bits::EIS_DATATIMEOUT) ? Result::err_timeout
                                                      : Result::err_crc,
                          "data error (multi)");
              io_clear_status ();
              return false;
            }
          if (n & want)
            {
              wo16 (reg::NORINT, want); // clear before draining this block
              break;
            }
          if (spins-- == 0)
            {
              set_result (Result::err_timeout,
                          "buffer ready timeout (multi)");
              io_clear_status ();
              return false;
            }
        }

      if (read)
        {
          for (unsigned i = 0; i < kSectorSize / 4u; ++i)
            {
              const std::uint32_t w = io32 (reg::BDATA);
              std::memcpy (block + 4u * i, &w, 4u);
            }
        }
      else
        {
          for (unsigned i = 0; i < kSectorSize / 4u; ++i)
            {
              std::uint32_t w;
              std::memcpy (&w, block + 4u * i, 4u);
              wo32 (reg::BDATA, w);
            }
        }
    }

  if (!wait_transfer_done ())
    {
      return false;
    }
  return send_stop ();
}

bool SdCard::send_stop ()
{
  io_clear_status ();
  if (!cmd (CMD12, 0, Rsp::shortb, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }
  // Make sure the data lines are released before the next command.
  wait_clear_state (bits::PRS_DATA_INHIBIT, 0x200000);
  return true;
}

bool SdCard::xfer (bool read, std::uint32_t lba, std::uint32_t count,
                   void* buf)
{
  if (!ready_)
    {
      set_result (Result::err_no_card, "card not initialised");
      return false;
    }
  if (count == 0)
    {
      return true;
    }
  if (static_cast<std::uint64_t> (lba) + count > sectors_)
    {
      set_result (Result::err_range, "sector range beyond card end");
      return false;
    }

  std::uint8_t* p = static_cast<std::uint8_t*> (buf);
  std::uint32_t done = 0;
  while (done < count)
    {
      // Controller block count is 16-bit; slice into <=512-sector chunks.
      std::uint32_t chunk = count - done;
      if (chunk > 512u)
        {
          chunk = 512u;
        }

      // SDHC/SDXC address blocks directly; SDSC uses byte addresses.
      const std::uint32_t addr = hc_ ? (lba + done) : (lba + done) * 512u;

      bool ok;
      if (chunk == 1)
        {
          ok = xfer_single (read, addr, p + done * kSectorSize);
        }
      else
        {
          ok = xfer_multi (read, addr, chunk, p + done * kSectorSize);
        }
      if (!ok)
        {
          return false;
        }
      done += chunk;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Card identification
// ---------------------------------------------------------------------------
bool SdCard::read_csd ()
{
  io_clear_status ();
  if (!cmd (CMD9, rca_ << 16, Rsp::r2, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }

  // Rebuild the CSD byte stream (CSD[127:0], big-endian order) from the four
  // SDHCI response registers.
  std::uint8_t c[15];
  const std::uint32_t r3 = rsp (3);
  const std::uint32_t r2 = rsp (2);
  const std::uint32_t r1 = rsp (1);
  const std::uint32_t r0 = rsp (0);
  c[0] = static_cast<std::uint8_t> (r3 >> 16);
  c[1] = static_cast<std::uint8_t> (r3 >> 8);
  c[2] = static_cast<std::uint8_t> (r3);
  c[3] = static_cast<std::uint8_t> (r2 >> 24);
  c[4] = static_cast<std::uint8_t> (r2 >> 16);
  c[5] = static_cast<std::uint8_t> (r2 >> 8);
  c[6] = static_cast<std::uint8_t> (r2);
  c[7] = static_cast<std::uint8_t> (r1 >> 24);
  c[8] = static_cast<std::uint8_t> (r1 >> 16);
  c[9] = static_cast<std::uint8_t> (r1 >> 8);
  c[10] = static_cast<std::uint8_t> (r1);
  c[11] = static_cast<std::uint8_t> (r0 >> 24);
  c[12] = static_cast<std::uint8_t> (r0 >> 16);
  c[13] = static_cast<std::uint8_t> (r0 >> 8);
  c[14] = static_cast<std::uint8_t> (r0);

  const std::uint8_t structure = static_cast<std::uint8_t> (c[0] >> 6);
  if (structure == 1u)
    {
      // CSD v2.0 (SDHC/SDXC): 22-bit C_SIZE in bytes 7..9.
      const std::uint32_t c_size
          = (static_cast<std::uint32_t> (c[7] & 0x3Fu) << 16)
            | (static_cast<std::uint32_t> (c[8]) << 8) | c[9];
      sectors_ = (c_size + 1u) << 10; // (C_SIZE+1) * 512 KiB / 512 B
      hc_ = true;
    }
  else if (structure == 0u)
    {
      // CSD v1.0 (SDSC): block count from C_SIZE / C_SIZE_MULT / READ_BL_LEN.
      const std::uint32_t read_bl_len = c[5] & 0x0Fu;
      const std::uint32_t c_size
          = (static_cast<std::uint32_t> (c[6] & 0x03u) << 10)
            | (static_cast<std::uint32_t> (c[7]) << 2) | (c[8] >> 6);
      const std::uint32_t c_size_mult
          = (static_cast<std::uint32_t> (c[9] & 0x03u) << 1) | (c[10] >> 7);
      const std::uint64_t block_nr
          = static_cast<std::uint64_t> (c_size + 1u) << (c_size_mult + 2u);
      const std::uint64_t block_len = 1ull << read_bl_len;
      sectors_ = static_cast<std::uint32_t> ((block_nr * block_len) >> 9);
      hc_ = false;
    }
  else
    {
      set_result (Result::err_unsupported, "unknown CSD structure");
      return false;
    }
  return true;
}

bool SdCard::card_init ()
{
  // CMD0: reset the card to idle.
  io_clear_status ();
  if (!cmd (CMD0, 0, Rsp::none, false, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }
  delay_loops (0x20000);

  // CMD8: SD v2 detection.
  io_clear_status ();
  cmd (CMD8, 0x000001AAu, Rsp::short_, true, false);
  const bool cmd8_ok = wait_cmd_done (0x800000);
  bool sd_v2 = false;
  if (cmd8_ok)
    {
      const std::uint32_t r = rsp (0);
      sd_v2 = ((r & 0xFFu) == 0xAAu) && (((r >> 8) & 0xFFu) == 0x01u);
    }
  else
    {
      // Legacy SD v1 card: no CMD8. Fall through without the HCS hint.
      io_clear_status ();
    }

  // ACMD41 (via CMD55): power up + read OCR.
  std::uint32_t ocr = 0;
  bool powered = false;
  for (int attempt = 0; attempt < 64; ++attempt)
    {
      io_clear_status ();
      if (!cmd (CMD55, 0, Rsp::short_, true, false))
        {
          return false;
        }
      if (!wait_cmd_done (0x800000))
        {
          set_result (Result::err_unsupported, "CMD55 rejected (not SD?)");
          return false;
        }

      io_clear_status ();
      // 2.7-3.6 V window plus, for v2 cards, the HCS (block addressing) bit.
      std::uint32_t arg = 0x00FF8000u;
      if (sd_v2)
        {
          arg |= 0x40000000u;
        }
      if (!cmd (CMD41, arg, Rsp::short_, false, false))
        {
          return false;
        }
      if (!wait_cmd_done (0x800000))
        {
          set_result (Result::err_voltage, "ACMD41 not accepted");
          return false;
        }
      ocr = rsp (0);
      if (ocr & 0x80000000u)
        {
          powered = true;
          break;
        }
      delay_loops (0x40000);
    }
  if (!powered)
    {
      set_result (Result::err_voltage, "card did not power up (ACMD41)");
      return false;
    }
  hc_ = (ocr & 0x40000000u) != 0;

  // CMD2 (CID), CMD3 (RCA), CMD9 (CSD).
  io_clear_status ();
  if (!cmd (CMD2, 0, Rsp::r2, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }

  io_clear_status ();
  if (!cmd (CMD3, 0, Rsp::short_, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }
  rca_ = rsp (0) >> 16;
  if (rca_ == 0)
    {
      set_result (Result::err_unsupported, "card returned RCA 0");
      return false;
    }

  if (!read_csd ())
    {
      return false;
    }

  // CMD7: select the card (standby -> transfer).
  io_clear_status ();
  if (!cmd (CMD7, rca_ << 16, Rsp::shortb, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }

  // ACMD6: switch card to the 4-bit data bus, then set the host side.
  io_clear_status ();
  if (!cmd (CMD55, rca_ << 16, Rsp::short_, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }
  io_clear_status ();
  if (!cmd (CMD6, 0x2u, Rsp::short_, true, false))
    {
      return false;
    }
  if (!wait_cmd_done (0x800000))
    {
      return false;
    }
  wo8 (reg::HOSTCTL, static_cast<std::uint8_t> (io8 (reg::HOSTCTL)
                                                | bits::HC_4BITBUS));
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool SdCard::init ()
{
  ready_ = false;
  sectors_ = 0;
  rca_ = 0;
  hc_ = false;
  mark_ok ();

  io_reset_all ();
  io_enable_status ();
  io_power_on ();
  io_set_clock (0x80u); // very slow clock for the identification phase

  // Data timeout: reset default 0x0 is the SHORTEST value (~2^13 card clocks),
  // far too small for real-card writes (programming can take ms). Use the max
  // (0xE = 2^27 card clocks). Without this, writes raise a data-timeout error
  // and time out on real hardware (QEMU does not model the timeout register).
  wo8 (reg::TMOUT, 0x0Eu);

  if (!io_wait_card_present ())
    {
      set_result (Result::err_no_card, "no SD card present");
      return false;
    }
  io_clear_status ();

  if (!card_init ())
    {
      return false;
    }

  // Raise the clock for the data phase (still safely below 25 MHz).
  io_set_clock (0x04u);

  ready_ = true;
  mark_ok ();
  return true;
}

bool SdCard::read_sectors (std::uint32_t lba, std::uint32_t count, void* dst)
{
  return xfer (true, lba, count, dst);
}

bool SdCard::write_sectors (std::uint32_t lba, std::uint32_t count,
                            const void* src)
{
  return xfer (false, lba, count, const_cast<void*> (src));
}

void SdCard::set_result (Result r, const char* msg)
{
  result_ = r;
  err_ = msg;
}

void SdCard::mark_ok ()
{
  result_ = Result::ok;
  err_ = "no error";
}

} // namespace sd
