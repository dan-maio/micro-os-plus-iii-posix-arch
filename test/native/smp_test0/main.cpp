/*
 * smp_test0 — Raspberry Pi Zero 2W µOS++ Phase-1 bring-up.
 *
 * Single active core (core 0). Proves the board layer: PL011 UART, MMU
 * (cacheable+shareable DRAM), ARM generic-timer 1 ms tick, and the µOS++
 * scheduler running a thread that sleeps on the system clock. Cores 1-3 are
 * parked in startup.S (Phase 2 brings them up).
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

int os_main (int, char*[])
{
  uart::uart1 << "Scheduler started on core " << static_cast<int>(port_cpu_id())
              << ". Heartbeat via sysclock.sleep_for(1000):\n";

  // Exactly 10 heartbeats prove the 1 ms tick + scheduler, then end the run
  // through a semihosted exit() (SEMIHOST builds). The idle fallback is only
  // reached on a plain SEMIHOST=0 / SD-boot image.
  for (std::uint32_t beat = 0; beat < 10; ++beat)
  {
    uart::uart1 << "[tick] heartbeat " << beat
                << "  os_clock=" << static_cast<std::uint32_t>(os::rtos::sysclock.now())
                << " ms\n";
    os::rtos::sysclock.sleep_for(1000);
  }
  uart::uart1 << "RESULT: PASS (10 heartbeats on core "
              << static_cast<int>(port_cpu_id()) << ")\n";
  hw_result::ok ();   // ends a SEMIHOST run; no-op otherwise
  for (;;) os::rtos::sysclock.sleep_for(1000);
}
