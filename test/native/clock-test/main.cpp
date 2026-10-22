/*
 * clock-test (native host) -- hrclock's origin.
 *
 * hrclock.sleep_until (now () + 100 ms) must last 100 ms of CLOCK_MONOTONIC,
 * and hrclock.now() be close to the steady count its sleeps compare with.
 * Guards the 2026-10-22 fix: now() was CLOCK_MONOTONIC since the host's boot
 * while the steady count runs from the kernel's start, so the sleep waited
 * the host's uptime plus 100 ms. (The rest of this file is smp_test0's
 * start-up.)
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <hw_result.hpp>

extern "C" unsigned port_cpu_id(void);

extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init();
    uart::uart1 << "\n\n";
    uart::uart1 << "+=============================================+\n";
    uart::uart1 << "|  " PORT_BANNER_LONG "  -  µOS++ SMP TEST 0  |\n";
    uart::uart1 << "|  Phase 1: UART + MMU + 1ms tick (core 0)    |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start,
                                     static_cast<std::size_t>(__heap_end - __heap_start));
    exception::init();
  }

  [[noreturn]] static void custom_main_trampoline (void)
  {
    int code = os_main (0, nullptr);
    std::exit (code);
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int main (int, char*[])
  {
    using namespace os::rtos;

#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif

    scheduler::initialize ();

    static os::rtos::thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof(main_stack);
    static thread main_thread { "main",
        reinterpret_cast<thread::func_t> (custom_main_trampoline), nullptr, attr };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}


#include <time.h>
static std::uint64_t mono_us (void)
{
  timespec tp;
  ::clock_gettime (CLOCK_MONOTONIC, &tp);
  return static_cast<std::uint64_t> (tp.tv_sec) * 1000000ULL + static_cast<std::uint64_t> (tp.tv_nsec) / 1000ULL;
}

int os_main (int, char*[])
{
  using namespace os::rtos;
  sysclock.sleep_for (2);
  const std::uint64_t gap0 = hrclock.now () - hrclock.steady_now ();
  const std::uint64_t t0 = mono_us ();
  hrclock.sleep_until (hrclock.now () + 100000u); // 100 ms in microseconds
  const std::uint64_t d = mono_us () - t0;
  uart::uart1 << "hrclock.now " << static_cast<std::uint32_t> (hrclock.now ()) << " us, now - steady_now " << static_cast<std::uint32_t> (gap0)
              << " us; sleep_until(now + 100 ms) lasted " << static_cast<std::uint32_t> (d) << " us\n";
  if (d >= 99000 && d <= 110000 && gap0 <= 5000)
    {
      uart::uart1 << "RESULT: PASS (clock-test: hrclock origin)\n";
      hw_result::ok ();
    }
  uart::uart1 << "RESULT: FAIL (clock-test: hrclock origin)\n";
  hw_result::fail ();
  for (;;) sysclock.sleep_for (1000);
}
