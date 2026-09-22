/*
 * smp_test3 — Raspberry Pi Zero 2W µOS++ message_queue producer/consumer.
 *
 * A producer (core 0) sends a stream of integers into a µOS++ message_queue;
 * a consumer (core 1) receives them, tests each for primality, toggles the LED
 * on a prime, and logs the value + the core it ran on. A logger (core 2)
 * prints throughput once a second. Proves cross-core message_queue transfer
 * and blocking send/receive. Adapted from the a7 smp_test3/2.
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

static message_queue g_mq { "mq", 8, sizeof(void*) };  /* AArch64: slot >= sizeof(void*) */
static mutex g_print { "print" };   // serialize UART across cores
static volatile std::uint32_t g_produced = 0, g_consumed = 0, g_primes = 0;
static volatile unsigned g_beats = 0;

static bool is_prime(std::uint32_t n)
{
  if (n < 2) return false;
  for (std::uint32_t i = 2; i * i <= n; ++i) if (n % i == 0) return false;
  return true;
}

static void* producer (void*)
{
  std::uint32_t v = 2;
  for (;;) { g_mq.send(&v, sizeof(v), 0); ++g_produced; ++v; sysclock.sleep_for(20); }
  return nullptr;
}
static void* consumer (void*)
{
  std::uint32_t v; bool led_state = false;
  for (;;)
  {
    if (g_mq.receive(&v, sizeof(v), nullptr) == result::ok)
    {
      ++g_consumed;
      bool p = is_prime(v);
      if (p) { ++g_primes; led_state = !led_state; led::set(led_state); }
      g_print.lock();
      uart::uart1 << "[c" << (int)port_cpu_id() << "] recv " << v
                  << (p ? " PRIME\n" : " composite\n");
      g_print.unlock();
    }
  }
  return nullptr;
}
static void* logger (void*)
{
  for (;;)
  {
    g_print.lock();
    uart::uart1 << "[c" << (int)port_cpu_id() << "] produced=" << g_produced
                << " consumed=" << g_consumed << " primes=" << g_primes
                << " t=" << (std::uint32_t)sysclock.now() << "ms\n";
    g_print.unlock();
    sysclock.sleep_for(1000);
  }
  return nullptr;
}

extern "C"
{
  extern char __heap_start[]; extern char __heap_end[];
  extern char __fiq_stack_top[]; extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }
  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init(); led::init();
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP TEST 3 : message_queue producer/consumer ==+\n\n";
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

static thread::stack::element_t s_prod[4096], s_cons[4096], s_log[4096];

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
  a.th_stack_address = s_prod; a.th_stack_size_bytes = sizeof(s_prod);
  static thread t_prod { "producer", producer, nullptr, a };
  a.th_stack_address = s_cons; a.th_stack_size_bytes = sizeof(s_cons);
  static thread t_cons { "consumer", consumer, nullptr, a };
  a.th_stack_address = s_log; a.th_stack_size_bytes = sizeof(s_log);
  static thread t_log { "logger", logger, nullptr, a };
  t_prod.cpu_affinity(1u << 0);
  t_cons.cpu_affinity(1u << 1);
  t_log.cpu_affinity(1u << 2);

  // Give the producer/consumer demo exactly 10 one-second beats, then run the
  // self-check and end through a semihosted exit() (SEMIHOST builds). The idle
  // fallback is only reached on a plain SEMIHOST=0 / SD-boot image.
  for (unsigned b = 1; b <= 10; ++b)
    {
      sysclock.sleep_for(1000);
      g_beats = b;
    }
  const bool ok = (test_secondaries_joined () && g_consumed > 0);
  // Single puts() = single SYS_WRITE0: cannot interleave with another core's
  // output in the OpenOCD log.
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
