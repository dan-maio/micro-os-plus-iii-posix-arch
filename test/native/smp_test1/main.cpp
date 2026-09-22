/*
 * smp_test1 — Raspberry Pi Zero 2W µOS++ semaphore ping-pong + LED.
 *
 * Two threads bounce a token back and forth across cores via two binary
 * semaphores (pinger pinned to core 0, ponger to core 1). A logger thread on
 * core 2 blinks the user LED once per second and prints the round count and
 * the core each participant is running on — proving cross-core semaphore
 * signalling and the periodic system tick. Adapted from the a7 smp_test1.
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

extern "C" unsigned port_cpu_id(void);
using namespace os::rtos;

static semaphore_binary g_ping { "ping", 1 };   // starts signalled
static semaphore_binary g_pong { "pong", 0 };
static volatile std::uint32_t g_rounds = 0;
static volatile int g_ping_core = -1, g_pong_core = -1;

static void* pinger (void*)
{
  for (;;) { g_ping.wait(); g_ping_core = (int)port_cpu_id(); ++g_rounds; g_pong.post(); }
  return nullptr;
}
static void* ponger (void*)
{
  for (;;) { g_pong.wait(); g_pong_core = (int)port_cpu_id(); g_ping.post(); }
  return nullptr;
}
static void* logger (void*)
{
  bool s = false;
  for (;;)
  {
    s = !s; led::set(s);
    uart::uart1 << "[c" << (int)port_cpu_id() << "] rounds=" << g_rounds
                << "  ping@c" << g_ping_core << " pong@c" << g_pong_core
                << "  t=" << (std::uint32_t)sysclock.now() << "ms\n";
    sysclock.sleep_for(1000);
  }
  return nullptr;
}

// --- shared scaffolding (identical across tests) ----------------------------
extern "C"
{
  extern char __heap_start[]; extern char __heap_end[];
  extern char __fiq_stack_top[]; extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }
  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init(); led::init();
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP TEST 1 : semaphore ping-pong + LED ==+\n\n";
    os_startup_initialize_free_store(__heap_start,
        static_cast<std::size_t>(__heap_end - __heap_start));
    exception::init();
  }
  [[noreturn]] static void custom_main_trampoline (void) { std::exit (os_main (0, nullptr)); }
  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();
    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack; attr.th_stack_size_bytes = sizeof(main_stack);
    static thread main_thread { "main",
        reinterpret_cast<thread::func_t> (custom_main_trampoline), nullptr, attr };
    os_main_thread = &main_thread;
    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}

static thread::stack::element_t s_ping[4096], s_pong[4096], s_log[4096];

int os_main (int, char*[])
{
  using uart::uart1;
  // Unmask IRQs before the scheduler starts. Portable across every
  // port: the architecture supplies the instruction, not the test.
  (void)os::rtos::interrupts::uncritical_section::enter ();
  smp_install_boot_threads();
  smp::start_secondary_cores();
  const int waited = test_wait_secondaries (3000);
  uart1 << "join: " << test_join_summary () << " (" << waited << "ms)\n";

  thread::attributes a = thread::initializer;
  a.th_stack_address = s_ping; a.th_stack_size_bytes = sizeof(s_ping);
  static thread t_ping { "ping", pinger, nullptr, a };
  a.th_stack_address = s_pong; a.th_stack_size_bytes = sizeof(s_pong);
  static thread t_pong { "pong", ponger, nullptr, a };
  a.th_stack_address = s_log; a.th_stack_size_bytes = sizeof(s_log);
  static thread t_log { "logger", logger, nullptr, a };
  t_ping.cpu_affinity(1u << 0);
  t_pong.cpu_affinity(1u << 1);
  t_log.cpu_affinity(1u << 2);

  // Give the ping-pong demo exactly 10 one-second beats, then run the
  // self-check and end through a semihosted exit() (SEMIHOST builds). The idle
  // fallback is only reached on a plain SEMIHOST=0 / SD-boot image.
  constexpr std::uint32_t kCheckBeats = 10;
  for (std::uint32_t beats = 0; beats < kCheckBeats; ++beats)
    {
      sysclock.sleep_for(1000);
    }
  const bool ok = (test_secondaries_joined () && g_rounds > 0);
  // Single puts() = single semihosted SYS_WRITE0 -> the line cannot be
  // interleaved with another core's output in the OpenOCD log.
  uart1.puts (ok ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");
  if (ok)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }
  for (;;) sysclock.sleep_for(1000);
}
