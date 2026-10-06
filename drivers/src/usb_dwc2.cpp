/*
 * usb_dwc2.cpp — Synopsys DWC2 USB device-mode (gadget) driver for BCM2837.
 *
 * Internal (buffer) DMA: GHWCFG2.ARCHITECTURE == 2 and GHWCFG4.DESC_DMA == 0,
 * so buffer DMA is the only DMA this core has. Every address handed to the core
 * must be the VideoCore BUS alias (see phys()), and an OUT byte count comes from
 * the DOEPTSIZ.XFRSIZ residue, not from the RX FIFO reader.
 *
 * One ISR (GPU IRQ 9, routed to core 0) and one service thread (pinned to
 * core 0). The ISR masks the events it observes in GINTMSK,
 * records them and posts a semaphore; all FIFO movement, the EP0 state machine
 * and the test's command loop run in the service thread.
 *
 * See docs/superpowers/specs/2026-09-16-rpi-zero-2w-usb-gadget-design.md for
 * the design and the SMP contract (S1..S8). This file is byte-identical
 * between the AArch32 and AArch64 ports.
 */
#include <cstdint>

#include <cmsis-plus/rtos/os.h>

#include "bcm2837.hpp"
#include "bcm_irq.hpp"
#include "mailbox.hpp"
#include "usb_dwc2.hpp"

extern "C" unsigned port_cpu_id (void);

namespace usb_dwc2
{

namespace
{

using os::rtos::semaphore_binary;

// ---------------------------------------------------------------------------
// MMIO helpers (all register and FIFO access is 32-bit).
// ---------------------------------------------------------------------------
inline std::uint32_t
rd (std::uint32_t off) noexcept
{
  return bcm::mmio_read (bcm::USB_BASE + off);
}

inline void
wr (std::uint32_t off, std::uint32_t v) noexcept
{
  bcm::mmio_write (bcm::USB_BASE + off, v);
}

inline void
setb (std::uint32_t off, std::uint32_t m) noexcept
{
  wr (off, rd (off) | m);
}

inline void
clrb (std::uint32_t off, std::uint32_t m) noexcept
{
  wr (off, rd (off) & ~m);
}

inline void
modify (std::uint32_t off, std::uint32_t clr, std::uint32_t set) noexcept
{
  wr (off, (rd (off) & ~clr) | set);
}

inline std::uint32_t DIEPCTL (unsigned n) noexcept { return reg::DIEPCTL0 + reg::EP_STRIDE * n; }
inline std::uint32_t DIEPINT (unsigned n) noexcept { return reg::DIEPINT0 + reg::EP_STRIDE * n; }
inline std::uint32_t DIEPTSIZ (unsigned n) noexcept { return reg::DIEPTSIZ0 + reg::EP_STRIDE * n; }
inline std::uint32_t DIEPDMA (unsigned n) noexcept { return reg::DIEPDMA0 + reg::EP_STRIDE * n; }
inline std::uint32_t DOEPCTL (unsigned n) noexcept { return reg::DOEPCTL0 + reg::EP_STRIDE * n; }
inline std::uint32_t DOEPINT (unsigned n) noexcept { return reg::DOEPINT0 + reg::EP_STRIDE * n; }
inline std::uint32_t DOEPTSIZ (unsigned n) noexcept { return reg::DOEPTSIZ0 + reg::EP_STRIDE * n; }
inline std::uint32_t DOEPDMA (unsigned n) noexcept { return reg::DOEPDMA0 + reg::EP_STRIDE * n; }
inline std::uint32_t FIFO (unsigned n) noexcept { return reg::FIFO0 + reg::FIFO_STRIDE * n; }

// ---------------------------------------------------------------------------
// Cache maintenance. The DWC2 is outside the CPU coherency domain: DMA reads
// need the buffer cleaned to PoC first, DMA writes need the CPU's stale lines
// invalidated before it reads. Data cache line = 64 bytes on A7/A53.
// ---------------------------------------------------------------------------
inline void
dc_clean (const void* p, std::uint32_t len) noexcept
{
  constexpr std::uintptr_t kLine = 64u;
  std::uintptr_t a = reinterpret_cast<std::uintptr_t> (p) & ~(kLine - 1u);
  const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (p) + len;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; a < end; a += kLine)
    {
#if defined(__aarch64__)
      __asm__ volatile ("dc cvac, %0" :: "r" (a) : "memory");
#else
      __asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (a) : "memory");
#endif
    }
  __asm__ volatile ("dsb sy" ::: "memory");
}

inline void
dc_inval (const void* p, std::uint32_t len) noexcept
{
  constexpr std::uintptr_t kLine = 64u;
  std::uintptr_t a = reinterpret_cast<std::uintptr_t> (p) & ~(kLine - 1u);
  const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (p) + len;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; a < end; a += kLine)
    {
#if defined(__aarch64__)
      __asm__ volatile ("dc ivac, %0" :: "r" (a) : "memory");
#else
      __asm__ volatile ("mcr p15, 0, %0, c7, c6, 1" :: "r" (a) : "memory");
#endif
    }
  __asm__ volatile ("dsb sy" ::: "memory");
}

inline void
dc_clean_inval (const void* p, std::uint32_t len) noexcept
{
  constexpr std::uintptr_t kLine = 64u;
  std::uintptr_t a = reinterpret_cast<std::uintptr_t> (p) & ~(kLine - 1u);
  const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (p) + len;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; a < end; a += kLine)
    {
#if defined(__aarch64__)
      __asm__ volatile ("dc civac, %0" :: "r" (a) : "memory");
#else
      __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1" :: "r" (a) : "memory");
#endif
    }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Translate an ARM physical address into the BUS address the DWC2 internal
// DMA master must be given. The USB core is an AXI master on the VideoCore
// bus, not on the ARM's: the BCM2837 device tree expresses this as
// "dma-ranges = <0xc0000000 0x00000000 0x3f000000>". Alias 0x00000000 is the
// VideoCore's *cached* view of RAM, so a DMA descriptor pointing there is
// served out of the VideoCore L2 - which no amount of ARM cache maintenance
// can reach. The first fetch of a buffer then reads RAM correctly and every
// later fetch of the same line returns the first fetch's stale bytes. Alias
// 0xC0000000 is the uncached view and is the only correct one here.
inline std::uint32_t
phys (const void* p) noexcept
{
  return static_cast<std::uint32_t> (reinterpret_cast<std::uintptr_t> (p))
         | 0xC0000000u;
}

// DMA staging buffers (64-byte aligned).
alignas (64) std::uint8_t g_setup_dma[64] = {};
alignas (64) std::uint8_t g_ep0_tx[64] = {};
alignas (64) std::uint8_t g_ep0_rx[64] = {};

constexpr std::uint32_t kSpinLong  = 1'000'000u;
#ifdef USB_FORCE_FS
constexpr std::uint32_t kMpsHs      = 64u;  // full-speed bulk max packet
#else
constexpr std::uint32_t kMpsHs      = 512u; // high-speed bulk max packet
#endif
constexpr std::uint32_t kMpsEp0     = 64u;

#ifdef USB_FORCE_FS
#define EP_MPS_LO 0x40u
#define EP_MPS_HI 0x00u
#else
#define EP_MPS_LO 0x00u
#define EP_MPS_HI 0x02u
#endif

// A transfer is bounded by TWO fields of D{I,O}EPTSIZ, not one. XFRSIZ is
// XferSizeWidth bits and PKTCNT is PktSizeWidth bits, both reported by
// GHWCFG3 -- on the BCM2837 core that prints GHWCFG3 = 0x0ff000e8 they are
// 19 and 10, so XFRSIZ tops out at 524287 bytes but PKTCNT tops out at 1023
// PACKETS. The packet count is what binds:
//
//     full speed  (64 B)  1023 x 64  =  65472 bytes
//     high speed (512 B)  1023 x 512 = 523776 bytes
//
// The old constant was 0x7F000, chosen against XFRSIZ alone and against
// 512-byte packets. Under USB_FORCE_FS it asked the core for 8128 packets in
// a 10-bit field -- an eightfold overflow -- so a large OUT transfer stopped
// early and the file arrived truncated. 65536 was the last size to survive
// because it overflows the field by exactly one packet.
constexpr std::uint32_t kMaxPktCnt   = 1023u;          // GHWCFG3 PktSizeWidth
constexpr std::uint32_t kInChunkMax  = 512u * 4u;      // 2048 B == EP1 TX FIFO
constexpr std::uint32_t kOutChunkMax = kMaxPktCnt * kMpsHs;

static_assert (kOutChunkMax <= 0x7FFFFu, "chunk exceeds the XFRSIZ field");
static_assert (kInChunkMax / kMpsHs <= kMaxPktCnt, "chunk exceeds PKTCNT");

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------
semaphore_binary* g_evt_sem = nullptr;   // posted by the ISR
volatile std::uint32_t g_pending = 0u;
volatile bool g_configured = false;
volatile Debug g_dbg = {};               // bring-up counters
configured_callback_t g_cb = nullptr;
void* g_cb_arg = nullptr;

// EP0
std::uint8_t g_setup[8] = {};
std::uint16_t g_ep0_wlength = 0u;
enum class Ep0After { none, set_address };
Ep0After g_ep0_after = Ep0After::none;
std::uint8_t g_pending_addr = 0u;
// True when the current control-IN transfer has a data stage, so an OUT status
// packet follows the IN data. False for no-data requests, whose status is the
// IN zero-length packet itself.
bool g_ep0_expect_status_out = false;
// Set when the 8 SETUP bytes have been read from the RX FIFO; the request is
// only processed on SETUP-stage-complete (pktsts=4), like Linux/TinyUSB.
bool g_setup_pending = false;

// Bulk OUT / IN transfer books
const std::uint8_t* g_out_buf = nullptr;
std::uint32_t g_out_len = 0u;
std::uint32_t g_out_got = 0u;
volatile bool g_out_done = false;
const std::uint8_t* g_in_buf = nullptr;
std::uint32_t g_in_len = 0u;
std::uint32_t g_in_sent = 0u;
volatile bool g_in_done = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
bool
wait_bits_set (std::uint32_t off, std::uint32_t mask, std::uint32_t limit) noexcept
{
  for (std::uint32_t i = 0; i < limit; ++i)
    {
      if ((rd (off) & mask) == mask)
        {
          return true;
        }
    }
  return false;
}

bool
wait_bits_clear (std::uint32_t off, std::uint32_t mask, std::uint32_t limit) noexcept
{
  for (std::uint32_t i = 0; i < limit; ++i)
    {
      if ((rd (off) & mask) == 0u)
        {
          return true;
        }
    }
  return false;
}

void
flush_rx_fifo () noexcept
{
  setb (reg::GRSTCTL, bit::RXFFLSH);
  for (std::uint32_t i = 0; i < kSpinLong; ++i)
    {
      if ((rd (reg::GRSTCTL) & bit::RXFFLSH) == 0u)
        {
          break;
        }
    }
}

void
flush_tx_fifos () noexcept
{
  wr (reg::GRSTCTL, bit::TXFFLSH | bit::TXFNUM_ALL);
  for (std::uint32_t i = 0; i < kSpinLong; ++i)
    {
      if ((rd (reg::GRSTCTL) & bit::TXFFLSH) == 0u)
        {
          break;
        }
    }
}

// Partition the DWC2 FIFO RAM. GRXFSIZ/GNPTXFSIZ/DIEPTXF1 hold (start, depth)
// in 32-bit words; the RX FIFO always starts at word 0.
void
configure_fifos () noexcept
{
  std::uint32_t depth = (rd (reg::GHWCFG3) >> 16) & 0xFFFFu;
  if (depth == 0u)
    {
      depth = 4096u;
    }

  std::uint32_t rx = 512u;
  std::uint32_t ep0 = 128u;
  std::uint32_t ep1 = 512u;
  if (rx + ep0 + ep1 > depth)
    {
      rx = 256u;
      ep0 = 64u;
      ep1 = (depth > rx + ep0) ? (depth - rx - ep0) : 64u;
    }

  wr (reg::GRXFSIZ, rx);
  wr (reg::GNPTXFSIZ, (ep0 << 16) | rx);
  wr (reg::DIEPTXF1, (ep1 << 16) | (rx + ep0));

  // Internal-DMA mode: the core shadows the DMA address registers in an
  // "EPInfo" block that must be reserved at the top of FIFO RAM. Bits [15:0]
  // carry the total FIFO size, bits [31:16] the EPInfo base (Linux
  // dwc2_hsotg_init_fifo writes the same pair).
  wr (reg::GDFIFOCFG, depth | ((rx + ep0 + ep1) << 16));
}

void
read_fifo (unsigned ep, void* dst, std::uint32_t n) noexcept
{
  std::uint8_t* d = static_cast<std::uint8_t*> (dst);
  std::uint32_t i = 0;
  for (; i + 4u <= n; i += 4u)
    {
      const std::uint32_t w = rd (FIFO (ep));
      d[i] = static_cast<std::uint8_t> (w);
      d[i + 1] = static_cast<std::uint8_t> (w >> 8);
      d[i + 2] = static_cast<std::uint8_t> (w >> 16);
      d[i + 3] = static_cast<std::uint8_t> (w >> 24);
    }
  if (i < n)
    {
      const std::uint32_t w = rd (FIFO (ep));
      for (std::uint32_t b = 0; b < (n - i); ++b)
        {
          d[i + b] = static_cast<std::uint8_t> (w >> (8u * b));
        }
    }
}

void
drain_fifo (unsigned ep, std::uint32_t n) noexcept
{
  std::uint32_t words = (n + 3u) / 4u;
  while (words-- != 0u)
    {
      (void)rd (FIFO (ep));
    }
}

void
fifo_write (unsigned ep, const std::uint8_t* src, std::uint32_t n) noexcept
{
  std::uint32_t i = 0;
  for (; i + 4u <= n; i += 4u)
    {
      const std::uint32_t w = std::uint32_t (src[i]) | (std::uint32_t (src[i + 1]) << 8)
                              | (std::uint32_t (src[i + 2]) << 16)
                              | (std::uint32_t (src[i + 3]) << 24);
      wr (FIFO (ep), w);
    }
  if (i < n)
    {
      std::uint32_t w = 0u;
      for (std::uint32_t b = 0; b < (n - i); ++b)
        {
          w |= std::uint32_t (src[i + b]) << (8u * b);
        }
      wr (FIFO (ep), w);
    }
}

// ---------------------------------------------------------------------------
// EP0
// ---------------------------------------------------------------------------
// Arm EP0 OUT to receive a SETUP packet: PKTCNT=1, XFRSIZ=8 (the setup packet
// is exactly 8 bytes). Mirrors Linux dwc2_hsotg_enqueue_setup().
void
arm_ep0_setup () noexcept
{
  // SUPCNT=3 tolerates back-to-back SETUPs, PKTCNT=1, XFRSIZ=8; the 8 bytes
  // are DMA'd into g_setup_dma.
  dc_clean_inval (g_setup_dma, sizeof (g_setup_dma));
  wr (DOEPDMA (0), phys (g_setup_dma));
  wr (DOEPTSIZ (0), (3u << 29) | (1u << 19) | 8u);
  wr (DOEPCTL (0),
      (rd (DOEPCTL (0)) & bit::MPS_MASK)
          | bit::USBACTEP | bit::EPENA);
}

// Arm EP0 OUT for the STATUS stage of a control-IN transfer: a 0-byte OUT
// packet. PKTCNT=1, XFRSIZ=0. Mirrors Linux
// dwc2_hsotg_ep0_zlp(hsotg, false) / dwc2_hsotg_program_zlp().
void
arm_ep0_status_out () noexcept
{
  // SUPCNT/MC=1, PKTCNT=1, XFRSIZ=0 (Linux dwc2_hsotg_program_zlp); the
  // (empty) status packet is DMA'd into a dummy buffer.
  dc_clean_inval (g_ep0_rx, sizeof (g_ep0_rx));
  wr (DOEPDMA (0), phys (g_ep0_rx));
  wr (DOEPTSIZ (0), (1u << 29) | (1u << 19) | 0u);
  wr (DOEPCTL (0),
      (rd (DOEPCTL (0)) & bit::MPS_MASK)
          | bit::USBACTEP | bit::EPENA | bit::CNAK);
}

void
stall_ep0 () noexcept
{
  setb (DIEPCTL (0), bit::STALL);
  setb (DOEPCTL (0), bit::STALL);
  // The core clears STALL when it takes the next SETUP; re-arm the receiver.
  g_ep0_expect_status_out = false;
  arm_ep0_setup ();
}

void
ep0_send (const void* data, std::uint32_t len) noexcept
{
  if (len > g_ep0_wlength)
    {
      len = g_ep0_wlength;
    }
  if (len > kMpsEp0)
    {
      len = kMpsEp0; // all standard descriptors used here fit one packet
    }

  const std::uint8_t* p = static_cast<const std::uint8_t*> (data);
  for (std::uint32_t i = 0; i < len; ++i)
    {
      g_ep0_tx[i] = p[i];
    }
  if (len >= 4u)
    {
      g_dbg.ep0_tx0 = std::uint32_t (g_ep0_tx[0])
                      | (std::uint32_t (g_ep0_tx[1]) << 8)
                      | (std::uint32_t (g_ep0_tx[2]) << 16)
                      | (std::uint32_t (g_ep0_tx[3]) << 24);
    }
  g_dbg.ep0_txlen = len;
  // Clean to PoC so the DMA engine sees the bytes, then program the internal
  // DMA address and enable the transfer.
  dc_clean (g_ep0_tx, (len != 0u) ? len : 1u);

  const std::uint32_t pktcnt = 1u;
  wr (DIEPDMA (0), phys (g_ep0_tx));
  wr (DIEPTSIZ (0), (pktcnt << 19) | (len & 0x7Fu));
  wr (DIEPCTL (0),
      (rd (DIEPCTL (0)) & bit::MPS_MASK)
          | bit::USBACTEP | bit::EPENA | bit::CNAK);
}

// --- descriptors -----------------------------------------------------------
constexpr std::uint8_t kDeviceDesc[] = {
  18,   0x01, 0x00, 0x02,       // bLength, DEVICE, bcdUSB 2.00
  0xFF, 0x00, 0x00, 64,         // class 0xFF/0/0, bMaxPacketSize0
  0x6B, 0x1D, 0x04, 0x01,       // idVendor 0x1d6b, idProduct 0x0104
  0x00, 0x01,                   // bcdDevice 1.00
  1,    2,    3,                // iManufacturer/iProduct/iSerialNumber
  1                             // bNumConfigurations
};

constexpr std::uint8_t kConfigDesc[] = {
  9, 0x02, 32, 0x00, 1, 1, 0, 0x80, 50,  // CONFIG, total 32, 100 mA
  9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0, // INTERFACE 0, 0xFF, 2 EPs
  7, 0x05, 0x81, 0x02, EP_MPS_LO, EP_MPS_HI, 0, // EP 0x81 BULK IN
  7, 0x05, 0x01, 0x02, EP_MPS_LO, EP_MPS_HI, 0  // EP 0x01 BULK OUT
};

std::uint32_t
build_string (unsigned index, std::uint8_t* out, std::uint32_t cap) noexcept
{
  if (index == 0u)
    {
      if (cap < 4u)
        {
          return 0u;
        }
      out[0] = 4u;
      out[1] = 0x03u;
      out[2] = 0x09u; // langid 0x0409
      out[3] = 0x04u;
      return 4u;
    }
  const char* s = nullptr;
  switch (index)
    {
    case 1:
      s = "uOS++";
      break;
    case 2:
      s = "uOS++ bulk test gadget";
      break;
    case 3:
      s = "0001";
      break;
    default:
      return 0u;
    }
  std::uint32_t n = 0;
  while (s[n] != '\0')
    {
      ++n;
    }
  if (2u + 2u * n > cap)
    {
      n = (cap >= 2u) ? (cap - 2u) / 2u : 0u;
    }
  out[0] = static_cast<std::uint8_t> (2u + 2u * n);
  out[1] = 0x03u;
  for (std::uint32_t i = 0; i < n; ++i)
    {
      out[2u + 2u * i] = static_cast<std::uint8_t> (s[i]);
      out[3u + 2u * i] = 0u;
    }
  return 2u + 2u * n;
}

void
set_configuration (bool on) noexcept
{
  if (on)
    {
      // EP1 IN must use TX FIFO 1 (DIEPTXF1), not the shared non-periodic
      // FIFO 0 that EP0 uses.
      wr (DIEPCTL (1),
          bit::USBACTEP | bit::EPTYP_BULK | kMpsHs | bit::TXFNUM (1u));
      wr (DOEPCTL (1), bit::USBACTEP | bit::EPTYP_BULK | kMpsHs | bit::CNAK);
      setb (reg::DAINTMSK, bit::DAINT_IN (1u) | bit::DAINT_OUT (1u));
      g_configured = true;
    }
  else
    {
      clrb (reg::DAINTMSK, bit::DAINT_IN (1u) | bit::DAINT_OUT (1u));
      setb (DIEPCTL (1), bit::SNAK);
      setb (DOEPCTL (1), bit::SNAK);
      g_configured = false;
    }
}

void
handle_setup () noexcept
{
  clrb (DIEPCTL (0), bit::STALL);
  clrb (DOEPCTL (0), bit::STALL);

  const std::uint8_t bm_rt = g_setup[0];
  const std::uint8_t b_req = g_setup[1];
  const std::uint16_t w_value = static_cast<std::uint16_t> (g_setup[2] | (g_setup[3] << 8));
  const std::uint16_t w_index = static_cast<std::uint16_t> (g_setup[4] | (g_setup[5] << 8));
  g_ep0_wlength = static_cast<std::uint16_t> (g_setup[6] | (g_setup[7] << 8));
  g_dbg.last_bmrt = bm_rt;
  g_dbg.last_brequest = b_req;
  g_dbg.last_wlength = g_ep0_wlength;
  g_dbg.setup_hist[g_dbg.setup_idx & 7u]
      = (static_cast<std::uint32_t> (bm_rt) << 24)
        | (static_cast<std::uint32_t> (b_req) << 16) | g_ep0_wlength;
  g_dbg.setup_idx = g_dbg.setup_idx + 1;
  g_ep0_after = Ep0After::none;
  // A non-zero wLength on a standard request here is a device-to-host data
  // stage, so an OUT status stage follows the IN data.
  g_ep0_expect_status_out = (g_ep0_wlength != 0u);

  // Only the nine standard requests. Class and vendor requests STALL.
  if ((bm_rt & 0x60u) != 0u)
    {
      stall_ep0 ();
      return;
    }

  switch (b_req)
    {
    case 0x06: // GET_DESCRIPTOR
      {
        const unsigned type = static_cast<unsigned> (w_value >> 8);
        const unsigned index = static_cast<unsigned> (w_value & 0xFFu);
        if (type == 1u)
          {
            ep0_send (kDeviceDesc, sizeof (kDeviceDesc));
          }
        else if (type == 2u)
          {
            ep0_send (kConfigDesc, sizeof (kConfigDesc));
          }
        else if (type == 3u)
          {
            static std::uint8_t strbuf[64];
            const std::uint32_t n = build_string (index, strbuf, sizeof (strbuf));
            if (n == 0u)
              {
                stall_ep0 ();
              }
            else
              {
                ep0_send (strbuf, n);
              }
          }
        else
          {
            stall_ep0 (); // device-qualifier etc.
          }
      }
      return;

    case 0x05: // SET_ADDRESS
      // Apply immediately; the core holds the new address until the status
      // stage completes. Deferring the write to the ZLP completion made the
      // address change land mid-transfer (host saw -32/STALL).
      modify (reg::DCFG, bit::DAD_MASK,
              (static_cast<std::uint32_t> (w_value & 0x7Fu) << bit::DAD_SHIFT)
                  & bit::DAD_MASK);
      ep0_send (nullptr, 0u);
      return;

    case 0x08: // GET_CONFIGURATION
      {
        const std::uint8_t v = g_configured ? 1u : 0u;
        ep0_send (&v, 1u);
      }
      return;

    case 0x09: // SET_CONFIGURATION
      set_configuration (w_value != 0u);
      ep0_send (nullptr, 0u);
      return;

    case 0x0A: // GET_INTERFACE
      {
        const std::uint8_t v = 0u;
        ep0_send (&v, 1u);
      }
      return;

    case 0x0B: // SET_INTERFACE
      ep0_send (nullptr, 0u);
      return;

    case 0x00: // GET_STATUS
      {
        std::uint8_t st[2] = { 0u, 0u };
        (void)w_index;
        ep0_send (st, 2u);
      }
      return;

    case 0x01: // CLEAR_FEATURE
    case 0x03: // SET_FEATURE
      ep0_send (nullptr, 0u);
      return;

    default:
      stall_ep0 ();
      return;
    }
}

// ---------------------------------------------------------------------------
// Bulk endpoint arming
// ---------------------------------------------------------------------------
void
arm_bulk_out (std::uint32_t len) noexcept
{
  std::uint32_t pktcnt = (len + kMpsHs - 1u) / kMpsHs;
  if (pktcnt == 0u)
    {
      pktcnt = 1u;
    }
  // Discard stale CPU lines, then point the internal DMA at the buffer.
  dc_clean_inval (g_out_buf, (len != 0u) ? len : 1u);
  wr (DOEPDMA (1), phys (g_out_buf));
  wr (DOEPTSIZ (1), (pktcnt << 19) | (len & 0x7FFFFu));
  wr (DOEPCTL (1),
      bit::USBACTEP | bit::EPTYP_BULK | kMpsHs | bit::EPENA | bit::CNAK);
}

void
program_bulk_in (const std::uint8_t* p, std::uint32_t len) noexcept
{
  std::uint32_t pktcnt = (len + kMpsHs - 1u) / kMpsHs;
  if (pktcnt == 0u)
    {
      pktcnt = 1u;
    }
  // Clean to PoC, then point the internal DMA at the caller's buffer.
  dc_clean (p, (len != 0u) ? len : 1u);
  wr (DIEPDMA (1), phys (p));
  wr (DIEPTSIZ (1), (pktcnt << 19) | (len & 0x7FFFFu));
  wr (DIEPCTL (1), bit::USBACTEP | bit::EPTYP_BULK | kMpsHs | bit::TXFNUM (1u)
                       | bit::EPENA | bit::CNAK);
}

// ---------------------------------------------------------------------------
// Event handlers (all run in the service thread)
// ---------------------------------------------------------------------------
void
handle_bus_reset () noexcept
{
  g_dbg.usbrst = g_dbg.usbrst + 1;
  g_configured = false;
  g_ep0_after = Ep0After::none;
  g_pending_addr = 0u;
  modify (reg::DCFG, bit::DAD_MASK, 0u);
  clrb (DIEPCTL (0), bit::STALL);
  clrb (DOEPCTL (0), bit::STALL);
  wr (reg::DAINTMSK, bit::DAINT_IN (0u) | bit::DAINT_OUT (0u));
  g_ep0_expect_status_out = false;
  flush_rx_fifo ();
  flush_tx_fifos ();
  arm_ep0_setup ();
}

void
handle_enum_done () noexcept
{
  g_dbg.enumdone = g_dbg.enumdone + 1;
  g_dbg.dsts = rd (reg::DSTS);
  // EP0 max packet is 64 for both HS and FS: MPS field 0 in DIEPCTL0 / DOEPCTL0.
  modify (DIEPCTL (0), bit::MPS_MASK, 0u);
  modify (DOEPCTL (0), bit::MPS_MASK, 0u);
  setb (DIEPCTL (0), bit::USBACTEP);
  setb (DOEPCTL (0), bit::USBACTEP);
  clrb (DIEPCTL (0), bit::STALL);
  clrb (DOEPCTL (0), bit::STALL);
  arm_ep0_setup ();
}

void
handle_ep_in () noexcept
{
  const std::uint32_t pending = rd (reg::DAINT) & rd (reg::DAINTMSK);
  for (unsigned n = 0; n < 16u; ++n)
    {
      if ((pending & bit::DAINT_IN (n)) == 0u)
        {
          continue;
        }
      const std::uint32_t st = rd (DIEPINT (n));
      wr (DIEPINT (n), st);
      if ((st & bit::XFERCOMPL) == 0u)
        {
          continue;
        }
      if (n == 0u)
        {
          g_dbg.ep0_in = g_dbg.ep0_in + 1;
          g_dbg.last_diepint0 = st;
          g_dbg.ep0_in_residue = rd (DIEPTSIZ (0)) & 0x7FFFFu;
          if (g_ep0_expect_status_out)
            {
              // Control-IN data stage done: the host now sends the OUT status,
              // so arm EP0 OUT for a 0-byte reception (Linux complete_in).
              arm_ep0_status_out ();
            }
          else
            {
              // No-data request: the IN zero-length packet we just sent WAS
              // the status stage; go straight back to waiting for a SETUP.
              arm_ep0_setup ();
            }
        }
      else if (n == 1u)
        {
          g_dbg.iep = g_dbg.iep + 1;
          g_in_done = true;
        }
    }
}

void
handle_ep_out () noexcept
{
  const std::uint32_t pending
      = (rd (reg::DAINT) & rd (reg::DAINTMSK)) >> 16;
  for (unsigned n = 0; n < 16u; ++n)
    {
      if ((pending & (1u << n)) == 0u)
        {
          continue;
        }
      const std::uint32_t st = rd (DOEPINT (n));
      wr (DOEPINT (n), st);
      if (n == 0u)
        {
          if ((st & bit::SETUP) != 0u)
            {
              // DMA mode: the 8 SETUP bytes were DMA'd into g_setup_dma.
              // Invalidate so the CPU reads what the core wrote.
              dc_inval (g_setup_dma, 8u);
              for (unsigned i = 0; i < 8u; ++i)
                {
                  g_setup[i] = g_setup_dma[i];
                }
              handle_setup ();
              continue;
            }
          if ((st & bit::XFERCOMPL) != 0u)
            {
              g_dbg.ep0_out = g_dbg.ep0_out + 1;
              g_dbg.last_doepint0 = st;
              // OUT status stage of a control-IN transfer completed.
              g_ep0_expect_status_out = false;
              arm_ep0_setup ();
            }
        }
      else if (n == 1u)
        {
          if ((st & bit::XFERCOMPL) != 0u)
            {
              g_dbg.oep = g_dbg.oep + 1;
              // Internal DMA moved the data straight into g_out_buf, so the
              // byte count cannot come from the RX FIFO reader (that path is
              // PIO-only and RXFLVL is not even unmasked in DMA mode). The
              // core decrements DOEPTSIZ.XFRSIZ by what it received, so the
              // residue is what is left of the length we programmed - this is
              // what Linux dwc2_hsotg_handle_outdone() does.
              const std::uint32_t residue = rd (DOEPTSIZ (1)) & 0x7FFFFu;
              g_out_got = (g_out_len > residue) ? (g_out_len - residue) : 0u;
              dc_inval (g_out_buf, (g_out_len != 0u) ? g_out_len : 1u);
              g_out_done = true;
            }
        }
    }
}

void
handle_rx_fifo () noexcept
{
  while ((rd (reg::GINTSTS) & bit::RXFLVL) != 0u)
    {
      const std::uint32_t sts = rd (reg::GRXSTSP);
      const unsigned ep = sts & bit::RXSTSP_EPNUM_MASK;
      const std::uint32_t bcnt = (sts >> bit::RXSTSP_BCNT_SHIFT) & bit::RXSTSP_BCNT_MASK;
      const unsigned pktsts = (sts >> bit::RXSTSP_PKTSTS_SHIFT) & bit::RXSTSP_PKTSTS_MASK;
      g_dbg.rx_pkts = g_dbg.rx_pkts + 1;

      switch (pktsts)
        {
        case 6u: // SETUP data received
          g_dbg.setups = g_dbg.setups + 1;
          read_fifo (0u, g_setup, (bcnt < 8u) ? bcnt : 8u);
          g_setup_pending = true; // process on SETUP-stage-complete (case 4)
          break;

        case 2u: // OUT data received
          if (ep == 0u)
            {
              drain_fifo (0u, bcnt);
            }
          else if (ep == 1u)
            {
              const std::uint32_t room
                  = (g_out_len > g_out_got) ? (g_out_len - g_out_got) : 0u;
              const std::uint32_t take = (bcnt < room) ? bcnt : room;
              if (take != 0u)
                {
                  read_fifo (1u, const_cast<std::uint8_t*> (g_out_buf + g_out_got),
                             take);
                }
              if (bcnt > take)
                {
                  drain_fifo (1u, bcnt - take);
                }
              g_out_got += take;
            }
          else
            {
              drain_fifo (ep, bcnt);
            }
          break;

        case 3u: // OUT transfer complete
          if (ep == 1u)
            {
              g_out_done = true;
            }
          break;

        case 4u: // SETUP stage complete
          if (g_setup_pending)
            {
              g_setup_pending = false;
              // Re-arm the setup counter and clear the SETUP interrupt so the
              // core will take the next SETUP (mirrors Linux/TinyUSB).
              setb (DOEPTSIZ (0), 3u << 29);
              wr (DOEPINT (0), bit::SETUP);
              handle_setup ();
            }
          if (bcnt != 0u)
            {
              drain_fifo (ep, bcnt);
            }
          break;

        case 1u: // global OUT NAK
        default:
          if (bcnt != 0u)
            {
              drain_fifo (ep, bcnt);
            }
          break;
        }
    }
}

// Pump exactly one batch of events. Blocks on the ISR semaphore. Because the
// protocol command loop runs in the service thread and ep_read/ep_write call
// this same function, there is never more than one waiter.
void
service_events_once () noexcept
{
  g_dbg.service_loops = g_dbg.service_loops + 1;
  g_evt_sem->wait ();
  std::uint32_t ev;
  {
    os::rtos::interrupts::critical_section ics;
    ev = g_pending;
    g_pending = 0u;
  }
  if (ev == 0u)
    {
      return;
    }
  if ((ev & bit::USBRST) != 0u)
    {
      handle_bus_reset ();
    }
  if ((ev & bit::ENUMDONE) != 0u)
    {
      handle_enum_done ();
    }
  if ((ev & bit::RXFLVL) != 0u)
    {
      handle_rx_fifo ();
    }
  if ((ev & bit::IEPINT) != 0u)
    {
      handle_ep_in ();
    }
  if ((ev & bit::OEPINT) != 0u)
    {
      handle_ep_out ();
    }
  // USBSUSP is acknowledged by clearing the status bit below.

  wr (reg::GINTSTS, ev);
  setb (reg::GINTMSK, ev & (bit::USBRST | bit::ENUMDONE | bit::RXFLVL
                           | bit::IEPINT | bit::OEPINT | bit::USBSUSP));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
const char*
init_result_str (InitResult r) noexcept
{
  switch (r)
    {
    case InitResult::ok:
      return "ok";
    case InitResult::no_power:
      return "USB power domain on failed";
    case InitResult::bad_id:
      return "not a DWC2 core";
    case InitResult::no_device_mode:
      return "no USB device mode";
    case InitResult::reset_timeout:
      return "core reset timeout";
    }
  return "unknown";
}

HwInfo
probe_hw () noexcept
{
  HwInfo h;
  h.snpsid = rd (reg::GSNPSID);
  h.hwcfg1 = rd (reg::GHWCFG1);
  h.hwcfg2 = rd (reg::GHWCFG2);
  h.hwcfg3 = rd (reg::GHWCFG3);
  h.hwcfg4 = rd (reg::GHWCFG4);
  return h;
}

InitResult
init () noexcept
{
  // Power the USB block. The firmware may already have left it on, and on some
  // boards SET_POWER_STATE reports a state we do not recognise, so the real
  // gate is the DWC2 identity read below, not this call. Its result is still
  // surfaced to the caller/test for the bring-up log.
  (void)mailbox::set_power_state (mailbox::kPowerDeviceUsb, true);

  // A DWC2 core identifies as "OT" + version in the top half of GSNPSID.
  if ((rd (reg::GSNPSID) & 0xFFFF'0000u) != 0x4F54'0000u)
    {
      return InitResult::bad_id;
    }

  if (g_evt_sem == nullptr)
    {
      g_evt_sem = new semaphore_binary ("usb-evt", 0u);
      if (g_evt_sem == nullptr)
        {
          return InitResult::no_power;
        }
    }

  if (!wait_bits_set (reg::GRSTCTL, bit::AHBIDLE, kSpinLong))
    {
      return InitResult::reset_timeout;
    }
  setb (reg::GRSTCTL, bit::CSRST);
  if (!wait_bits_clear (reg::GRSTCTL, bit::CSRST, kSpinLong))
    {
      return InitResult::reset_timeout;
    }
  if (!wait_bits_set (reg::GRSTCTL, bit::AHBIDLE, kSpinLong))
    {
      return InitResult::reset_timeout;
    }

  // Force device mode, clear host mode, program the UTMI+ turnaround time.
  // The BCM2837's internal PHY is 8-bit UTMI (Linux bcm2835 params:
  // phy_utmi_width = 8), which needs TRDT = 9 PHY clocks. The 16-bit value (5)
  // copied from the RK3506 port corrupts high-speed packets.
  modify (reg::GUSBCFG, bit::FHMOD | bit::TRDT_MASK,
          bit::FDMOD | (9u << bit::TRDT_SHIFT));

  bool device = false;
  for (std::uint32_t i = 0; i < kSpinLong; ++i)
    {
      if ((rd (reg::GINTSTS) & bit::CURMOD) == 0u)
        {
          device = true;
          break;
        }
    }
  if (!device)
    {
      // QEMU models DWC2 as a host controller only, so device mode never
      // settles there. This is the SKIP signal, and it reports honestly on
      // silicon too if the block is unpowered or the PHY is down.
      return InitResult::no_device_mode;
    }

  wr (reg::PCGCCTL, 0u);
  // Force B-session valid so the core runs as a peripheral regardless of the
  // OTG ID/VBUS state the firmware left behind, and enable the non-zero-length
  // status OUT handshake (TinyUSB does both for DWC2 device mode).
  modify (reg::GOTGCTL, bit::AVALOEN, bit::BVALOEN | bit::BVALOVAL);
  setb (reg::DCFG, bit::NZLSOHSK);
#ifdef USB_FORCE_FS
  modify (reg::DCFG, bit::DSPD_MASK, bit::DSPD_FS);
#else
  modify (reg::DCFG, bit::DSPD_MASK, bit::DSPD_HS);
#endif
  modify (reg::DCFG, bit::DAD_MASK, 0u);

  // Pulse Power-On Programming Done to commit device configuration.
  setb (reg::DCTL, bit::POPRGDNE);
  for (int i = 0; i < 1000; ++i)
    {
      __asm__ volatile ("nop");
    }
  clrb (reg::DCTL, bit::POPRGDNE);

  configure_fifos ();
  flush_rx_fifo ();
  flush_tx_fifos ();

  // Clear any stale endpoint/bus interrupt latches left by the boot firmware
  // so they cannot be mistaken for events of this session.
  wr (reg::GINTSTS, 0xFFFF'FFFFu);
  wr (DIEPINT (0u), 0xFFFF'FFFFu);
  wr (DOEPINT (0u), 0xFFFF'FFFFu);
  wr (DIEPINT (1u), 0xFFFF'FFFFu);
  wr (DOEPINT (1u), 0xFFFF'FFFFu);

  // Internal DMA: OUT data is DMA'd to memory, so there is no RXFLVL data to
  // read; EP0 setup and completions arrive as DOEPINT/DIEPINT instead.
  wr (reg::GINTMSK,
      bit::USBRST | bit::ENUMDONE | bit::IEPINT | bit::OEPINT | bit::USBSUSP);
  wr (reg::DIEPMSK, bit::XFERCOMPL);
  wr (reg::DOEPMSK, bit::XFERCOMPL | bit::SETUP);
  wr (reg::DAINTMSK, bit::DAINT_IN (0u) | bit::DAINT_OUT (0u));

  // Arm EP0 OUT to receive SETUP packets before attaching to the bus.
  arm_ep0_setup ();

  // Order matters: route and enable the IRQ, then global enable, then release
  // the soft disconnect, so the host cannot enumerate into an unwired core.
  bcm_irq::route_gpu_irqs_to_core (0u);
  bcm_irq::enable_gpu_irq (bcm::IRQ_USB);

  setb (reg::GAHBCFG, bit::GINT_GLBL | bit::DMAEN);

  // Release soft disconnect (SDIS=0) to attach to host, ensuring Remote Wakeup
  // Signaling (RWUSIG=0) is disabled.
  clrb (reg::DCTL, bit::SDIS | bit::RWUSIG);
  return InitResult::ok;
}

void
isr () noexcept
{
  g_dbg.isr_calls = g_dbg.isr_calls + 1;
  if (port_cpu_id () != 0u)
    {
      g_dbg.isr_offcore = g_dbg.isr_offcore + 1;
      return;
    }
  g_dbg.gintsts = rd (reg::GINTSTS);
  const std::uint32_t sts = g_dbg.gintsts & rd (reg::GINTMSK);
  if (sts == 0u)
    {
      return;
    }
  clrb (reg::GINTMSK, sts);
  {
    os::rtos::interrupts::critical_section ics;
    g_pending |= sts;
  }
  if (g_evt_sem != nullptr)
    {
      g_evt_sem->post ();
    }
}

void
set_configured_callback (configured_callback_t cb, void* arg) noexcept
{
  g_cb = cb;
  g_cb_arg = arg;
}

bool
configured () noexcept
{
  return g_configured;
}

const volatile Debug&
debug () noexcept
{
  return g_dbg;
}

RegSnapshot
snapshot () noexcept
{
  RegSnapshot s;
  s.diepctl0 = rd (DIEPCTL (0u));
  s.dieptsiz0 = rd (DIEPTSIZ (0u));
  s.diepint0 = rd (DIEPINT (0u));
  s.doepctl0 = rd (DOEPCTL (0u));
  s.doeptsiz0 = rd (DOEPTSIZ (0u));
  s.doepint0 = rd (DOEPINT (0u));
  s.dcfg = rd (reg::DCFG);
  s.dctl = rd (reg::DCTL);
  s.dsts = rd (reg::DSTS);
  s.gintsts = rd (reg::GINTSTS);
  s.gintmsk = rd (reg::GINTMSK);
  s.gahbcfg = rd (reg::GAHBCFG);
  return s;
}

void
service_thread_body () noexcept
{
  arm_ep0_setup ();
  while (!g_configured)
    {
      service_events_once ();
    }
  if (g_cb != nullptr)
    {
      g_cb (g_cb_arg);
    }
  for (;;)
    {
      service_events_once ();
    }
}

int
ep_read (void* buf, std::uint32_t len) noexcept
{
  if (!g_configured)
    {
      return -1;
    }
  std::uint8_t* p = static_cast<std::uint8_t*> (buf);
  std::uint32_t done = 0u;
  while (done < len)
    {
      std::uint32_t chunk = len - done;
      if (chunk > kOutChunkMax)
        {
          chunk = kOutChunkMax;
        }
      g_out_buf = p + done;
      g_out_len = chunk;
      g_out_got = 0u;
      g_out_done = false;
      arm_bulk_out (chunk);
      while (!g_out_done)
        {
          service_events_once ();
        }
      done += g_out_got;
      if (g_out_got < chunk)
        {
          break; // short packet terminates the OUT transfer
        }
    }
  return static_cast<int> (done);
}

int
ep_write (const void* buf, std::uint32_t len) noexcept
{
  if (!g_configured)
    {
      return -1;
    }
  const std::uint8_t* p = static_cast<const std::uint8_t*> (buf);
  std::uint32_t done = 0u;
  while (done < len)
    {
      std::uint32_t chunk = len - done;
      if (chunk > kInChunkMax)
        {
          chunk = kInChunkMax;
        }
      g_in_done = false;
      program_bulk_in (p + done, chunk);
      while (!g_in_done)
        {
          service_events_once ();
        }
      done += chunk;
    }

  // An exact multiple of wMaxPacketSize needs a zero-length packet, or the
  // host's libusb read blocks until its timeout instead of completing.
  if (len != 0u && (len % kMpsHs) == 0u)
    {
      g_in_done = false;
      program_bulk_in (p, 0u);
      while (!g_in_done)
        {
          service_events_once ();
        }
    }
  return static_cast<int> (done);
}

} // namespace usb_dwc2

// ---------------------------------------------------------------------------
// The strong external-IRQ hook. Lives in this translation unit so only the
// tests that link the driver get it; the weak default stays a no-op.
// ---------------------------------------------------------------------------
extern "C" void
port_external_irq_handler (std::uint32_t source)
{
  if ((source & bcm::local::SRC_GPU) == 0u)
    {
      return;
    }
  if (!bcm_irq::gpu_irq_pending (bcm::IRQ_USB))
    {
      return; // some other GPU IRQ; not ours
    }
  usb_dwc2::isr ();
}
