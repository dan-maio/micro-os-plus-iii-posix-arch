/*
 * smp-num-test — Raspberry Pi Zero 2W µOS++ multi-thread I/O demo (" PORT_BANNER_ISA ").
 *
 * 64-bit sibling of ../../32b/smp-num-test. Five concurrent worker threads
 * with EXPLICIT priorities (highest first):
 *
 *   1. uart-writer      (prio high)          : console tick every 0.5 s
 *   2. led-blinker      (prio above_normal)  : GPIO29 ACT LED, 5 blinks per
 *                                              pass, 0.3 s on / 0.3 s off,
 *                                              printing "led on"/"led off" on
 *                                              every edge
 *   3. sd-text-writer   (prio normal)        : 20 lines of ~100 chars appended
 *                                              to num.txt on the SD card
 *   4. fp-compute       (prio below_normal)  : 20 sin/cos/log results appended
 *                                              to num.txt (as chars)
 *   5. sd-reader        (prio low)           : after each writer pass, reads
 *                                              num.txt and streams the new tail
 *
 * Synchronisation: every access to num.txt is serialised with one mutex, and
 * the LED thread paces the passes with a counting semaphore (3 posts per pass:
 * one for each writer, one for the reader).
 *
 * Storage:
 *   - HW build (make HW=1): existing FAT32 boot partition, files under /tests
 *     (num.txt), never formatted (FatFs).
 *   - QEMU build (!HW): flatfs over a raw disk.img, num.txt at the volume root.
 *
 * Run under QEMU: ./run.sh          (raspi3b, four A53s natively, no shim)
 * Quick smoke:    RUN_MS=8000 ./run.sh
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
// GPIO29 = onboard ACT LED (matches the 32b smp-num-test). This must be
// defined BEFORE including led.hpp (its default is GPIO16).
#ifndef LED_PIN
#define LED_PIN 29
#endif
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <sd.hpp>
#if defined(HW_BUILD)
#include <fatfs_hw.hpp>
#else
#include <flatfs.hpp>
#endif

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

// console() / console_uart() serialise output across cores; every test
// carried its own copy. See test_smpl/common/include/test-console.hpp.
#include <test-console.hpp>

extern "C" unsigned port_cpu_id (void);
using namespace os::rtos;

namespace
{

// Serialises console output (uart1 is not re-entrant across cores).

// ---------------------------------------------------------------------------
// Console helpers. console() writes to the UART and (on a SEMIHOST build)
// mirrors to the semihosting channel, so banners/status/LED edges reach the
// emulator or OpenOCD log. console_uart() is UART-only: the high-volume lines
// (file-content streams, per-tick status) would otherwise cost one trap each.
// ---------------------------------------------------------------------------
// Repetitive/high-volume lines (file-content streams, per-tick status).
// ---------------------------------------------------------------------------
// Run control / iteration counts
// ---------------------------------------------------------------------------
// Wall-clock run length in ms. Override for short QEMU smoke tests with
// make ... RUN_MS=5000  (Makefile forwards -DRUN_MS).
#ifndef RUN_MS
#define RUN_MS 60000
#endif
constexpr unsigned kTextLines = 20;    // SD text lines per pass
constexpr unsigned kComputeLines = 20; // FPU compute lines per pass
constexpr unsigned kLedBlinks = 5;     // LED blinks per pass (0.3 s each)

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
sd::SdCard g_card;
#if defined(HW_BUILD)
#else
flatfs::FlatFs g_fs;
#endif

// num.txt file name (8.3-safe).
static const char kNumFile[] = "num.txt";

// All num.txt access serialised by this mutex.
static mutex g_file_mtx { "file" };

// "Pass" beat. The LED thread posts 3 per LED pass (5 blinks); the two writers
// each take one to run their 20-line batch, and the reader takes one to dump
// the newly appended tail. This paces the batches to the LED (~3 s) so num.txt
// growth and console output stay bounded over the whole RUN_MS window.
static semaphore_counting g_beat { "beat", 3, 0 };

// Progress / result flags.
static volatile unsigned g_text_written = 0;
static volatile unsigned g_calc_written = 0;
static volatile unsigned g_text_fail = 0;
static volatile unsigned g_calc_fail = 0;
static volatile unsigned g_read_fail = 0;
static volatile unsigned g_led_fail = 0;

// Set by os_main just before RESULT so the infinite uart thread can exit.
static volatile bool g_finish = false;

// In-memory copy of num.txt: flatfs cannot append, so the whole file is
// rewritten on every line. num.txt grows over the RUN_MS window (writers run
// every LED beat); allow ~2 min of 40 lines / ~3 s -> ~1600 lines * ~110 B
// ~= 176 KB. Keep headroom.
static char g_num_buf[256 * 1024];
static unsigned g_num_len = 0;

// Whole-file read buffer (reader + final dump). Same bound as g_num_buf.
static char g_read_buf[256 * 1024];

// Per-core count of appended lines (the "[cN]" of each write), for a summary.
static volatile unsigned g_core_lines[OS_NCPU] = {};

// ---------------------------------------------------------------------------
// SD helpers. All called with g_file_mtx held.
// ---------------------------------------------------------------------------
bool
sd_append (const char* text, unsigned len)
{
  // Prepend "[cN] " where N is the core performing this write. sd_append is
  // always called with g_file_mtx held, so this is the core actually doing the
  // file I/O.
  char tagged[160];
  int tn = std::snprintf (tagged, sizeof (tagged), "[c%u] ",
                          (unsigned)port_cpu_id ());
  if (tn < 0)
    {
      tn = 0;
    }
  const unsigned core = (unsigned)port_cpu_id ();
  if (core < OS_NCPU)
    {
      ++g_core_lines[core];
    }
  if (static_cast<unsigned> (tn) + len > sizeof (tagged))
    {
      len = sizeof (tagged) - static_cast<unsigned> (tn);
    }
  std::memcpy (tagged + tn, text, len);

#if defined(HW_BUILD)
  return fatfshw::append_file (kNumFile, tagged,
                               static_cast<unsigned> (tn) + len);
#else
  // flatfs has no append: accumulate in RAM and rewrite the whole file.
  if (g_num_len + static_cast<unsigned> (tn) + len >= sizeof (g_num_buf))
    {
      return false;
    }
  std::memcpy (g_num_buf + g_num_len, tagged, static_cast<unsigned> (tn) + len);
  g_num_len += static_cast<unsigned> (tn) + len;
  return g_fs.write_file (kNumFile, g_num_buf, g_num_len)
         == flatfs::FlatFs::Result::ok;
#endif
}

bool
sd_read_all (char* out, unsigned cap, unsigned& len)
{
#if defined(HW_BUILD)
  std::uint32_t size = 0;
  if (!fatfshw::file_size (kNumFile, size) || size > cap)
    {
      return false;
    }
  if (!fatfshw::load_file (kNumFile, out, size))
    {
      return false;
    }
  len = size;
  return true;
#else
  std::uint32_t size = 0;
  if (g_fs.file_size (kNumFile, size) != flatfs::FlatFs::Result::ok
      || size > cap)
    {
      return false;
    }
  if (g_fs.read_file (kNumFile, out, size, nullptr)
      != flatfs::FlatFs::Result::ok)
    {
      return false;
    }
  len = size;
  return true;
#endif
}

void
sd_remove_num ()
{
  // Delete num.txt if present so each run starts from an empty file.
  // A missing file is fine (nothing to remove); a real I/O error is ignored.
#if defined(HW_BUILD)
  fatfshw::remove_file (kNumFile);
#else
  g_fs.remove_file (kNumFile);
#endif
  console ("  removed previous num.txt (if any)\n");
}

void
sd_prepare ()
{
  console ("Initialising SD card via SDHCI @0x3F300000...\n");
  if (!g_card.init ())
    {
      console ("  SD init FAILED: %s\n", g_card.last_error ());
      hw_result::fail ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  console ("  SD card ready: %u sectors (~%u MiB)\n", g_card.sector_count (),
           static_cast<unsigned> (g_card.capacity_bytes () >> 20));

#if defined(HW_BUILD)
  fatfshw::bind_card (g_card);
  if (!fatfshw::mount_volume ())
    {
      console ("  FAT32 mount failed: %s\n", fatfshw::last_error ());
      hw_result::fail ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  if (!fatfshw::ensure_tests_dir ())
    {
      console ("  mkdir tests failed: %s\n", fatfshw::last_error ());
      hw_result::fail ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  console ("  FAT32 /tests ready (boot files untouched).\n");
#else
  flatfs::FlatFs::Result r = g_fs.format (g_card);
  if (r != flatfs::FlatFs::Result::ok)
    {
      console ("  flatfs format failed: %s\n", flatfs::FlatFs::result_str (r));
      hw_result::fail ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  console ("  flatfs volume formatted.\n");
#endif
}

// ---------------------------------------------------------------------------
// Thread 1: uart-writer (prio high)
// ---------------------------------------------------------------------------
static void*
uart_thread (void*)
{
  unsigned tick = 0;
  while (!g_finish)
    {
      console_uart ("[c%u][uart] tick %u  (text=%u calc=%u)\n",
                    (unsigned)port_cpu_id (), tick, (unsigned)g_text_written,
                    (unsigned)g_calc_written);
      ++tick;
      sysclock.sleep_for (500);
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Thread 2: led-blinker (prio above_normal) — kLedBlinks blinks @ 0.3 s
// ---------------------------------------------------------------------------
static void*
led_thread (void*)
{
  // One pass = kLedBlinks blinks. After the pass, release one batch slot to
  // each writer and one to the reader (3 posts total), then repeat until the
  // run window ends.
  while (!g_finish)
    {
      for (unsigned i = 0; i < kLedBlinks; ++i)
        {
          led::on ();
          console ("[c%u] led on\n", (unsigned)port_cpu_id ());
          sysclock.sleep_for (300);
          led::off ();
          console ("[c%u] led off\n", (unsigned)port_cpu_id ());
          sysclock.sleep_for (300);
        }
      g_beat.post ();
      g_beat.post ();
      g_beat.post ();
    }
  console ("[c%u] LED stopped\n", (unsigned)port_cpu_id ());
  return nullptr;
}

// Keeps a worker CPU-bound (no sleeping) for a while so the no-affinity
// scheduler can spread ready threads onto idle cores 2/3, as in smp_test4.
void
cpu_burn_yield ()
{
  volatile float acc = 1.0f;
  for (unsigned k = 0; k < 80000u; ++k)
    {
      acc = acc * 1.000001f + 0.000001f;
    }
  (void)acc;
  this_thread::yield ();
}

// ---------------------------------------------------------------------------
// Thread 3: sd-text-writer (prio normal) — kTextLines ~100-char lines
// ---------------------------------------------------------------------------
static void*
sd_text_thread (void*)
{
  unsigned pass = 0;
  while (!g_finish)
    {
      // Wait for the next LED beat (one text batch per pass).
      while (!g_finish && g_beat.try_wait () != result::ok)
        {
          sysclock.sleep_for (10);
        }
      if (g_finish)
        {
          break;
        }
      for (unsigned i = 0; i < kTextLines; ++i)
        {
          // ~100-char, LETTERS-ONLY body (no digits, so it stays visually
          // distinct from the numeric CALC lines). sd_append() prefixes it
          // with the core tag "[cN] ". Vary by cycling the filler word.
          static const char* const kFill[4]
              = { "lorem", "ipsum", "dolor", "amet" };
          const char* fill = kFill[i & 3u];
          static const char kHead[]
              = "TEXT the quick brown fox jumps over the lazy dog ";
          char line[128];
          unsigned pos = 0;
          const unsigned head_len = sizeof (kHead) - 1u;
          std::memcpy (line, kHead, head_len);
          pos = head_len;
          // Pad with "word-..." until ~96 chars.
          while (pos < 96u)
            {
              for (const char* p = fill; *p && pos < 96u; ++p)
                {
                  line[pos++] = *p;
                }
              if (pos < 96u)
                {
                  line[pos++] = '-';
                }
            }
          // Trim a trailing '-' and append the alphabetic terminator.
          if (pos > 0u && line[pos - 1u] == '-')
            {
              --pos;
            }
          static const char kTail[] = " END\n";
          const unsigned tail_len = sizeof (kTail) - 1u;
          std::memcpy (line + pos, kTail, tail_len);
          pos += tail_len;

          g_file_mtx.lock ();
          bool ok = sd_append (line, pos);
          if (ok)
            {
              ++g_text_written;
            }
          else
            {
              ++g_text_fail;
            }
          g_file_mtx.unlock ();
          // Stay runnable (CPU burn + yield, no sleep) so idle cores 2/3 can
          // claim this thread; sleeping would pin it to core 0.
          cpu_burn_yield ();
        }
      ++pass;
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Thread 4: fp-compute (prio below_normal) — kComputeLines sin/cos/log lines
// ---------------------------------------------------------------------------
static void*
compute_thread (void*)
{
  unsigned pass = 0;
  while (!g_finish)
    {
      // Wait for the next LED beat (one compute batch per pass).
      while (!g_finish && g_beat.try_wait () != result::ok)
        {
          sysclock.sleep_for (10);
        }
      if (g_finish)
        {
          break;
        }
      for (unsigned i = 0; i < kComputeLines; ++i)
        {
          const float x = 0.1f + 0.37f * static_cast<float> (i);
          const float s = std::sin (x);
          const float c = std::cos (x);
          const float l = (x > 0.0f) ? std::log (x) : 0.0f;
          char line[160];
          int n = std::snprintf (line, sizeof (line),
                                 "CALC %03u: sin(%.3f)=%+.6f cos(%.3f)=%+.6f "
                                 "log(%.3f)=%+.6f\n",
                                 i, (double)x, (double)s, (double)x, (double)c,
                                 (double)x, (double)l);
          if (n < 0)
            {
              n = 0;
            }
          g_file_mtx.lock ();
          bool ok = sd_append (line, static_cast<unsigned> (n));
          if (ok)
            {
              ++g_calc_written;
            }
          else
            {
              ++g_calc_fail;
            }
          g_file_mtx.unlock ();
          // Stay runnable (CPU burn + yield, no sleep) so idle cores 2/3 can
          // claim this thread; sleeping would keep it on core 0.
          cpu_burn_yield ();
        }
      ++pass;
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Thread 5: sd-reader (prio low) — stream the newly appended tail of num.txt
// ---------------------------------------------------------------------------
static void*
reader_thread (void*)
{
  unsigned last_len = 0;
  unsigned pass = 0;
  while (!g_finish)
    {
      // One reader slot per LED pass.
      while (!g_finish && g_beat.try_wait () != result::ok)
        {
          sysclock.sleep_for (10);
        }
      if (g_finish)
        {
          break;
        }
      sysclock.sleep_for (20); // let the writers' last writes land

      unsigned len = 0;
      g_file_mtx.lock ();
      bool ok = sd_read_all (g_read_buf, sizeof (g_read_buf), len);
      g_file_mtx.unlock ();
      if (!ok)
        {
          ++g_read_fail;
          console ("[c%u][reader] read num.txt FAILED\n",
                   (unsigned)port_cpu_id ());
          continue;
        }
      if (len < last_len)
        {
          last_len = 0; // num.txt was removed/recreated: dump from the start
        }
      console ("[c%u][reader] pass %u: num.txt now %u bytes (+%u)\n",
               (unsigned)port_cpu_id (), pass, len, len - last_len);
      // Stream only the newly appended tail to the UART.
      constexpr unsigned kChunk = 128;
      for (unsigned off = last_len; off < len; off += kChunk)
        {
          char tmp[kChunk + 1];
          unsigned n = len - off;
          if (n > kChunk)
            {
              n = kChunk;
            }
          std::memcpy (tmp, g_read_buf + off, n);
          tmp[n] = '\0';
          uart::uart1.puts_uart (tmp);
          this_thread::yield ();
        }
      last_len = len;
      ++pass;
    }
  console ("[c%u][reader] stopped after %u dumps\n", (unsigned)port_cpu_id (),
           pass);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Startup scaffolding (idle threads, main trampoline)
// ---------------------------------------------------------------------------
} // namespace (anonymous)

static thread::stack::element_t uart_stk[4096];
static thread::stack::element_t led_stk[4096];
static thread::stack::element_t text_stk[4096];
static thread::stack::element_t calc_stk[8192];
static thread::stack::element_t reader_stk[8192];

int
os_main (int, char*[])
{
  console ("\n+== " PORT_BANNER_SHORT " µOS++ NUM TEST (" PORT_BANNER_ISA ") : uart+LED+SD+FPU ==+\n\n");
  sd_prepare ();
  sd_remove_num ();

  // Unmask IRQs before the scheduler starts. Portable across every
  // port: the architecture supplies the instruction, not the test.
  (void)os::rtos::interrupts::uncritical_section::enter ();
  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  int waited = 0;
  while ((g_core_stage[1] < 3 || g_core_stage[2] < 3 || g_core_stage[3] < 3)
         && waited < 3000)
    {
      sysclock.sleep_for (50);
      waited += 50;
    }
  console ("join: c1=%u c2=%u c3=%u (%d ms)\n", (unsigned)g_core_stage[1],
           (unsigned)g_core_stage[2], (unsigned)g_core_stage[3], waited);

  // Threads, with explicit priorities (high -> low as listed above).
  thread::attributes a = thread::initializer;

  a.th_stack_address = uart_stk; a.th_stack_size_bytes = sizeof (uart_stk);
  static thread t_uart { "uart", uart_thread, nullptr, a };
  t_uart.priority (thread::priority::high);

  a.th_stack_address = led_stk; a.th_stack_size_bytes = sizeof (led_stk);
  static thread t_led { "led", led_thread, nullptr, a };
  t_led.priority (thread::priority::above_normal);

  a.th_stack_address = text_stk; a.th_stack_size_bytes = sizeof (text_stk);
  static thread t_text { "sdtext", sd_text_thread, nullptr, a };
  t_text.priority (thread::priority::normal);

  a.th_stack_address = calc_stk; a.th_stack_size_bytes = sizeof (calc_stk);
  static thread t_calc { "compute", compute_thread, nullptr, a };
  t_calc.priority (thread::priority::below_normal);

  a.th_stack_address = reader_stk; a.th_stack_size_bytes = sizeof (reader_stk);
  static thread t_read { "sdread", reader_thread, nullptr, a };
  t_read.priority (thread::priority::low);

  console ("threads started (uart=high, led=above_normal, text=normal, "
           "compute=below_normal, reader=low)\n");

  // Run the repeating passes for the wall-clock window (default 1 minute).
  const std::uint32_t start_ms = static_cast<std::uint32_t> (sysclock.now ());
  const std::uint32_t run_ms = static_cast<std::uint32_t> (RUN_MS);
  console ("running for %u ms ...\n", run_ms);
  while (static_cast<std::uint32_t> (sysclock.now ()) - start_ms < run_ms)
    {
      sysclock.sleep_for (100);
    }

  // Stop the workers.
  g_finish = true;
  sysclock.sleep_for (200); // let threads see the flag and unwind
  g_beat.post ();           // wake any writer/reader still blocked on a beat
  g_beat.post ();
  g_beat.post ();
  sysclock.sleep_for (200);

  // Final dump of num.txt on the UART at the end of the run.
  {
    unsigned len = 0;
    g_file_mtx.lock ();
    const bool rd = sd_read_all (g_read_buf, sizeof (g_read_buf), len);
    g_file_mtx.unlock ();
    if (rd)
      {
        console ("\n---- num.txt on SD (%u bytes, final dump) ----\n", len);
        for (unsigned off = 0; off < len; off += 128)
          {
            char tmp[129];
            unsigned n = len - off;
            if (n > 128)
              {
                n = 128;
              }
            std::memcpy (tmp, g_read_buf + off, n);
            tmp[n] = '\0';
            uart::uart1.puts_uart (tmp);
          }
        console ("---- end num.txt ----\n");
      }
    else
      {
        ++g_read_fail;
        console ("[main] final num.txt read FAILED\n");
      }
  }

  const bool ok = (g_text_written >= kTextLines)
                  && (g_calc_written >= kComputeLines) && (g_text_fail == 0)
                  && (g_calc_fail == 0) && (g_read_fail == 0)
                  && (g_led_fail == 0);
  console ("num.txt lines by core: c0=%u c1=%u c2=%u c3=%u\n",
           (unsigned)g_core_lines[0], (unsigned)g_core_lines[1],
           (unsigned)g_core_lines[2], (unsigned)g_core_lines[3]);
  console ("run %u ms, text=%u calc=%u\n", run_ms, (unsigned)g_text_written,
           (unsigned)g_calc_written);
  uart::uart1.puts (ok ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");
  if (ok)
    {
      hw_result::ok ();
    }
  else
    {
    hw_result::fail ();
    }
  for (;;)
    {
      sysclock.sleep_for (10000);
    }
}

// ----------------------------------------------------------------------------
// µOS++ startup scaffolding (same pattern as smp_test0..4)
// ----------------------------------------------------------------------------
extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    led::init ();
    os_startup_initialize_free_store (
        __heap_start, static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    std::exit (os_main (0, nullptr));
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int
  main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*> (__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();

    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
    static thread main_thread {
      "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}
