/**
 * @file  usb_dwc2.hpp
 * @brief Synopsys DesignWare DWC2 USB device-mode (gadget) driver for BCM2837.
 *
 * PIO only — no DMA. The core is outside the CPU coherency domain, so letting
 * it bus-master would turn every transfer buffer into a cache-maintenance
 * problem on two architectures. All data moves through the FIFO windows.
 *
 * The driver owns exactly one interrupt (GPU IRQ 9) and one service thread,
 * both pinned to core 0 by construction (GPU_IRQ_ROUTING admits a single
 * target core). The ISR is short and moves no data; the service thread runs
 * the EP0 state machine, the bulk FIFO work and (via a registered callback)
 * the test's command loop.
 *
 * This header is byte-identical between the AArch32 (32b) and AArch64 (64b)
 * ports: every register is 32-bit MMIO, and the driver never does pointer
 * arithmetic into a register window.
 */
#pragma once

#include <cstdint>

namespace usb_dwc2
{

// ---------------------------------------------------------------------------
// Register map (offsets from bcm::USB_BASE). Standard DWC2 layout.
// ---------------------------------------------------------------------------
namespace reg {
inline constexpr std::uint32_t GOTGCTL   = 0x000u;
inline constexpr std::uint32_t GOTGINT   = 0x004u;
inline constexpr std::uint32_t GAHBCFG   = 0x008u;
inline constexpr std::uint32_t GUSBCFG   = 0x00Cu;
inline constexpr std::uint32_t GRSTCTL   = 0x010u;
inline constexpr std::uint32_t GINTSTS   = 0x014u;
inline constexpr std::uint32_t GINTMSK   = 0x018u;
inline constexpr std::uint32_t GRXSTSR   = 0x01Cu;
inline constexpr std::uint32_t GRXSTSP   = 0x020u;
inline constexpr std::uint32_t GRXFSIZ   = 0x024u;
inline constexpr std::uint32_t GNPTXFSIZ = 0x028u;
inline constexpr std::uint32_t GNPTXSTS  = 0x02Cu;
inline constexpr std::uint32_t GSNPSID   = 0x040u;
inline constexpr std::uint32_t GHWCFG1   = 0x044u;
inline constexpr std::uint32_t GHWCFG2   = 0x048u;
inline constexpr std::uint32_t GHWCFG3   = 0x04Cu;
inline constexpr std::uint32_t GHWCFG4   = 0x050u;
inline constexpr std::uint32_t GDFIFOCFG = 0x05Cu;
inline constexpr std::uint32_t DIEPTXF1  = 0x104u;

inline constexpr std::uint32_t DCFG      = 0x800u;
inline constexpr std::uint32_t DCTL      = 0x804u;
inline constexpr std::uint32_t DSTS      = 0x808u;
inline constexpr std::uint32_t DIEPMSK   = 0x810u;
inline constexpr std::uint32_t DOEPMSK   = 0x814u;
inline constexpr std::uint32_t DAINT     = 0x818u;
inline constexpr std::uint32_t DAINTMSK  = 0x81Cu;
inline constexpr std::uint32_t DTXFSTS0  = 0x918u;
inline constexpr std::uint32_t PCGCCTL   = 0xE00u;

// Per-endpoint register stride and the two blocks' base offsets.
inline constexpr std::uint32_t EP_STRIDE = 0x20u;
inline constexpr std::uint32_t DIEPCTL0  = 0x900u;
inline constexpr std::uint32_t DIEPINT0  = 0x908u;
inline constexpr std::uint32_t DIEPTSIZ0 = 0x910u;
inline constexpr std::uint32_t DIEPDMA0  = 0x914u;
inline constexpr std::uint32_t DOEPCTL0  = 0xB00u;
inline constexpr std::uint32_t DOEPINT0  = 0xB08u;
inline constexpr std::uint32_t DOEPTSIZ0 = 0xB10u;
inline constexpr std::uint32_t DOEPDMA0  = 0xB14u;

// DFIFO window for endpoint n is USB_BASE + 0x1000 * (n + 1).
inline constexpr std::uint32_t FIFO0      = 0x1000u;
inline constexpr std::uint32_t FIFO_STRIDE = 0x1000u;
} // namespace reg

// ---------------------------------------------------------------------------
// Bit definitions used by the driver.
// ---------------------------------------------------------------------------
namespace bit {
// GRSTCTL
inline constexpr std::uint32_t CSRST   = 1u << 0;
inline constexpr std::uint32_t RXFFLSH = 1u << 4;
inline constexpr std::uint32_t TXFFLSH = 1u << 5;
inline constexpr std::uint32_t TXFNUM_ALL = 0x10u << 6;
inline constexpr std::uint32_t AHBIDLE = 1u << 31;
// GOTGCTL
inline constexpr std::uint32_t AVALOEN = 1u << 4;
inline constexpr std::uint32_t BVALOVAL = 1u << 7;
inline constexpr std::uint32_t BVALOEN = 1u << 6;
// DCFG
inline constexpr std::uint32_t NZLSOHSK = 1u << 2;
// GUSBCFG
inline constexpr std::uint32_t FHMOD   = 1u << 29;
inline constexpr std::uint32_t FDMOD   = 1u << 30;
inline constexpr std::uint32_t TRDT_SHIFT = 10u;
inline constexpr std::uint32_t TRDT_MASK  = 0xFu << TRDT_SHIFT;
// GAHBCFG
inline constexpr std::uint32_t GINT_GLBL = 1u << 0;
inline constexpr std::uint32_t DMAEN     = 1u << 5;
// GINTSTS / GINTMSK
inline constexpr std::uint32_t CURMOD   = 1u << 0;
inline constexpr std::uint32_t RXFLVL   = 1u << 4;
inline constexpr std::uint32_t USBSUSP  = 1u << 11;
inline constexpr std::uint32_t USBRST   = 1u << 12;
inline constexpr std::uint32_t ENUMDONE = 1u << 13;
inline constexpr std::uint32_t IEPINT   = 1u << 18;
inline constexpr std::uint32_t OEPINT   = 1u << 19;
// DCTL
inline constexpr std::uint32_t RWUSIG   = 1u << 0;
inline constexpr std::uint32_t SDIS     = 1u << 1;
inline constexpr std::uint32_t POPRGDNE = 1u << 11;
// DCFG
inline constexpr std::uint32_t DSPD_HS   = 0u;
inline constexpr std::uint32_t DSPD_FS   = 1u;
inline constexpr std::uint32_t DSPD_MASK = 3u;
inline constexpr std::uint32_t DAD_SHIFT = 4u;
inline constexpr std::uint32_t DAD_MASK  = 0x7Fu << DAD_SHIFT;
// DSTS
inline constexpr std::uint32_t ENUMSPD_SHIFT = 1u;
inline constexpr std::uint32_t ENUMSPD_MASK  = 3u << ENUMSPD_SHIFT;
inline constexpr std::uint32_t ENUMSPD_HS    = 0u;
// EPCTL (IN and OUT share the layout we use)
inline constexpr std::uint32_t EPTYP_CTRL = 0u << 18;
inline constexpr std::uint32_t EPTYP_BULK = 2u << 18;
inline constexpr std::uint32_t USBACTEP   = 1u << 15;
inline constexpr std::uint32_t NAKSTS     = 1u << 13;
inline constexpr std::uint32_t EPDIS      = 1u << 30;
inline constexpr std::uint32_t EPENA      = 1u << 31;
inline constexpr std::uint32_t SNAK       = 1u << 27;
inline constexpr std::uint32_t CNAK       = 1u << 26;
inline constexpr std::uint32_t STALL      = 1u << 21;
inline constexpr std::uint32_t MPS_MASK   = 0x7FFu;
inline constexpr std::uint32_t MPS_512    = 512u;
// Device IN endpoint transmit-FIFO number, bits 25:22 of DIEPCTL.
constexpr std::uint32_t TXFNUM (unsigned n) noexcept { return (n & 0xFu) << 22; }
// EPINT
inline constexpr std::uint32_t XFERCOMPL = 1u << 0;
inline constexpr std::uint32_t EPDISBLD  = 1u << 1;
inline constexpr std::uint32_t SETUP     = 1u << 3;
// DOEPTSIZ0 (EP0 SETUP packet count)
inline constexpr std::uint32_t SUPCNT_1  = 1u << 29;
inline constexpr std::uint32_t SUPCNT_3  = 3u << 29;
// DAINT
constexpr std::uint32_t DAINT_IN (unsigned n) noexcept { return 1u << n; }
constexpr std::uint32_t DAINT_OUT (unsigned n) noexcept { return 1u << (16u + n); }
// GRXSTSP fields
inline constexpr std::uint32_t RXSTSP_EPNUM_SHIFT    = 0u;
inline constexpr std::uint32_t RXSTSP_EPNUM_MASK     = 0xFu;
inline constexpr std::uint32_t RXSTSP_BCNT_SHIFT     = 4u;
inline constexpr std::uint32_t RXSTSP_BCNT_MASK      = 0x7FFu;
inline constexpr std::uint32_t RXSTSP_DPID_SHIFT     = 15u;
inline constexpr std::uint32_t RXSTSP_PKTSTS_SHIFT   = 17u;
inline constexpr std::uint32_t RXSTSP_PKTSTS_MASK    = 0xFu;
} // namespace bit

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

/// Why init() did not reach device mode. Reported verbatim to the console.
enum class InitResult
{
  ok,
  no_power,
  bad_id,
  no_device_mode,
  reset_timeout
};

/// Human-readable, test-visible string for an InitResult.
const char* init_result_str (InitResult r) noexcept;

/// Raw DWC2 identity registers, for the hardware register-map validation task.
struct HwInfo
{
  std::uint32_t snpsid;
  std::uint32_t hwcfg1;
  std::uint32_t hwcfg2;
  std::uint32_t hwcfg3;
  std::uint32_t hwcfg4;
};

/// Read GSNPSID and GHWCFG1..4. Five MMIO reads, no side effects.
HwInfo probe_hw () noexcept;

/// Power the block, reset the core, force device mode, size the FIFOs, unmask
/// the device interrupts and route GPU IRQ 9 to core 0. Returns no_device_mode
/// when the core never settles in device mode (the QEMU detector).
InitResult init () noexcept;

/// Short, bounded ISR body: mask observed events, record them, post the
/// semaphore. No FIFO access, no loops over payload.
void isr () noexcept;

/// Never returns. The core-0-pinned USB service thread: it pumps events until
/// the device is configured, invokes the registered configured callback (the
/// test's command loop) and keeps pumping for it afterwards.
void service_thread_body () noexcept;

/// True once the host has issued SET_CONFIGURATION(1).
bool configured () noexcept;

/// Callback run in the service-thread context once the device is configured.
using configured_callback_t = void (*) (void* arg);
void set_configured_callback (configured_callback_t cb, void* arg) noexcept;

/// Blocking, service-thread-only. Returns bytes moved, or a negative error.
int ep_read  (void* buf, std::uint32_t len) noexcept;
int ep_write (const void* buf, std::uint32_t len) noexcept;

/// Bring-up instrumentation. Cheap counters updated by the ISR and handlers,
/// read by the test's monitor thread while the device is not yet configured.
struct Debug
{
  std::uint32_t isr_calls;
  std::uint32_t isr_offcore;
  std::uint32_t gintsts;
  std::uint32_t usbrst;
  std::uint32_t enumdone;
  std::uint32_t rx_pkts;
  std::uint32_t setups;
  std::uint32_t ep0_in;
  std::uint32_t ep0_out;
  std::uint32_t iep;
  std::uint32_t oep;
  std::uint32_t service_loops;
  std::uint32_t last_bmrt;
  std::uint32_t last_brequest;
  std::uint32_t last_wlength;
  std::uint32_t ep0_tx0;
  std::uint32_t ep0_txlen;
  std::uint32_t nptx_before;
  std::uint32_t nptx_after;
  std::uint32_t last_diepint0;
  std::uint32_t last_doepint0;
  std::uint32_t ep0_in_residue;
  // Last 8 SETUPs, packed bm<<24 | req<<16 | wLength.
  std::uint32_t setup_hist[8];
  std::uint32_t setup_idx;
  std::uint32_t dsts;
};
const volatile Debug& debug () noexcept;

/// A live snapshot of the registers that matter for EP0 bring-up.
struct RegSnapshot
{
  std::uint32_t diepctl0, dieptsiz0, diepint0;
  std::uint32_t doepctl0, doeptsiz0, doepint0;
  std::uint32_t dcfg, dctl, dsts, gintsts, gintmsk, gahbcfg;
};
RegSnapshot snapshot () noexcept;

} // namespace usb_dwc2
