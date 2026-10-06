/*
 * smp-mutex-stress - the SMP leg of upstream's mutex-stress; see test.cpp.
 *
 * The start-up is smp_test2's: a main thread of our own, the per-core idle
 * threads, the secondaries released and joined, and only then the test. The
 * body after the join is upstream's os_main (tests/sources/mutex-stress).
 */

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

#include <test-smp-boot.hpp>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/time.h>

#include <test.h>

#if !defined(RUN_SECONDS)
#define RUN_SECONDS (10)
#endif

using namespace os;
using namespace os::rtos;

void
busy_wait (unsigned int micros)
{
  /* struct */ timeval tp;
  gettimeofday (&tp, nullptr);
  uint64_t until_micros;
  until_micros
      = static_cast<uint64_t> (tp.tv_sec * 1000000 + tp.tv_usec) + micros;

  uint64_t now_micros;
  do
    {
      gettimeofday (&tp, nullptr);
      now_micros = static_cast<uint64_t> (tp.tv_sec * 1000000 + tp.tv_usec);
    }
  while (now_micros < until_micros);
}

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
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP MUTEX STRESS : " TEST_NCPU_STR " cores ==+\n\n";
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
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();

    static thread::stack::element_t main_stack[8192];
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

int
os_main (int, char*[])
{
  (void)os::rtos::interrupts::uncritical_section::enter ();

  printf ("os_main on core %u\n", port_cpu_id ());
  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  const int waited = test_wait_secondaries (3000);
  printf ("join: %s (%dms)\n", test_join_summary (), waited);
  if (!test_secondaries_joined ())
    {
      printf ("\nRESULT: FAIL (secondaries did not join)\n");
      hw_result::fail ();
    }

  printf ("\nMutex stress & uniformity test, " TEST_NCPU_STR " cores\n");
#if defined(__clang__)
  printf ("Built with clang " __VERSION__ "\n");
#else
  printf ("Built with GCC " __VERSION__ "\n");
#endif

  timeval tp;
  gettimeofday (&tp, nullptr);
  const uint32_t seed = static_cast<uint32_t> (
      (tp.tv_sec + tp.tv_usec + 15485863) * 179424673);
  printf ("Seed %u\n", static_cast<unsigned int> (seed));

  const int status = run_tests (RUN_SECONDS, seed);

  printf ("\nRESULT: %s\n", (status == 0) ? "PASS" : "FAIL");
  if (status == 0)
    {
      hw_result::ok ();
    }
  hw_result::fail ();
}
