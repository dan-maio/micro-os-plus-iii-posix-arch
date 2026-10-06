/*
 * smp-rtos-apis - the SMP leg of upstream's rtos-apis.
 *
 * The API tests themselves are ../rtos-apis/, unchanged; tests.cmake adds
 * them to this image. What differs is who calls them and on how many cores:
 *
 *   - the board runs at OS_NCPU, with the SMP scheduler, and the secondaries
 *     are released and joined first (smp_test2's start-up);
 *   - the whole upstream sequence -- C++ API, C API, ISO API, POSIX I/O --
 *     runs once per core, from a driver thread pinned to that core at
 *     construction;
 *   - the threads the API tests create are NOT pinned, so the SMP scheduler
 *     places them on any core while the driver waits on them from its own;
 *   - the per-core check: the driver must still be on its core after every
 *     sub-test, and every core must pass the sequence.
 */

#include <cmsis-plus/rtos/os.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

#include <test-smp-boot.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <test-cpp-api.h>
#include <test-c-api.h>
#include <test-iso-api.h>
#include <test-posix-io-api.h>

using namespace os;
using namespace os::rtos;

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
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP RTOS APIS : " TEST_NCPU_STR " cores ==+\n\n";
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

namespace
{
  struct run_t
  {
    unsigned core;
    int ret;
    unsigned off_core; // sub-tests after which the driver was elsewhere
  };

  run_t runs[OS_NCPU];

  void
  check_core (run_t* r, const char* after)
  {
    const unsigned now = port_cpu_id ();
    if (now != r->core)
      {
        r->off_core++;
        printf ("[core %u] driver found on core %u after %s\n", r->core, now,
                after);
      }
  }

  // Upstream's os_main sequence, verbatim in order and in its errno idiom.
  void*
  driver (void* arg)
  {
    run_t* r = static_cast<run_t*> (arg);
    printf ("\n==== core %u: driver started on core %u ====\n", r->core,
            port_cpu_id ());
    check_core (r, "start");

    int ret = 0;
    errno = 0;

    if (ret == 0)
      {
        ret = test_cpp_api ();
        printf ("errno=%d\n", errno);
        errno = 0;
        check_core (r, "test_cpp_api");
      }
    if (ret == 0)
      {
        ret = test_c_api ();
        printf ("errno=%d\n", errno);
        errno = 0;
        check_core (r, "test_c_api");
      }
    if (ret == 0)
      {
        ret = test_iso_api (false);
        printf ("errno=%d\n", errno);
        errno = 0;
        check_core (r, "test_iso_api");
      }
    if (ret == 0)
      {
        ret = test_posix_io_api (false);
        printf ("errno=%d\n", errno);
        errno = 0;
        check_core (r, "test_posix_io_api");
      }

    r->ret = ret;
    printf ("==== core %u: %s ====\n", r->core, (ret == 0) ? "passed" : "FAILED");
    return nullptr;
  }
} // namespace

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

  printf ("\nµOS++ RTOS simple APIs test, once per core, " TEST_NCPU_STR
          " cores\n");
#if defined(__clang__)
  printf ("Built with clang " __VERSION__ "\n");
#else
  printf ("Built with GCC " __VERSION__ "\n");
#endif

  // One core at a time: the API tests use static objects and fixed names,
  // so two sequences at once would test the tests, not the kernel.
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      runs[c] = { c, -1, 0 };
      thread::attributes a = thread::initializer;
      a.th_cpu_affinity = (1u << c);
      a.th_stack_size_bytes = 4 * port::stack::default_size_bytes;
      thread drv{ "drv", driver, &runs[c], a };
      drv.join ();
    }

  bool ok = true;
  printf ("\n==== per-core checks (OS_NCPU=%u) ====\n",
          static_cast<unsigned> (OS_NCPU));
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      const bool pass = (runs[c].ret == 0) && (runs[c].off_core == 0);
      printf ("core %u: sequence %s, off-core %u\n", c,
              (runs[c].ret == 0) ? "passed" : "FAILED", runs[c].off_core);
      ok = ok && pass;
    }

  printf ("done\n");
  printf ("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
  if (ok)
    {
      hw_result::ok ();
    }
  hw_result::fail ();
}
