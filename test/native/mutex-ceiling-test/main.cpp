// mutex-ceiling-test: a protect-protocol lock refused with EINVAL leaves
// nothing behind.
//
// A thread whose priority is above a protect mutex's ceiling must get EINVAL
// from lock() and must not own the mutex. mutex::internal_try_lock_() used to
// link the mutex into the thread's list of owned mutexes (and count it) before
// the ceiling check, and the EINVAL path cleared only owner_. The refused
// thread then exited "owning" it: internal_exit_() asserts that list is empty,
// and internal_destroy_() marked the (robust) mutex owner-dead, so the next
// rightful lock() returned EOWNERDEAD.
//
// Checks: the refused lock is EINVAL; after the refused thread exits and is
// destroyed, a thread under the ceiling locks and unlocks normally, twice.

#include <cmsis-plus/rtos/os.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <hw_result.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace os;
using namespace os::rtos;

namespace
{
  mutex* g_mx = nullptr;
  result_t g_high_res = result::ok;
  result_t g_low_lock = result::ok;
  result_t g_low_unlock = result::ok;

  unsigned g_failures = 0;

  void
  check (bool cond, const char* what, result_t res)
  {
    char b[160];
    std::snprintf (b, sizeof (b), "  %-48s res=%3u  %s\n", what,
                   static_cast<unsigned> (res), cond ? "ok" : "FAIL");
    uart::uart1 << b;
    if (!cond)
      {
        ++g_failures;
      }
  }

  void*
  high_func (void*)
  {
    g_high_res = g_mx->lock ();
    return nullptr;
  }

  void*
  low_func (void*)
  {
    g_low_lock = g_mx->lock ();
    g_low_unlock = g_mx->unlock ();
    return nullptr;
  }
} // namespace

int
os_main (int, char*[])
{
  uart::uart1 << "\nmutex-ceiling-test: a refused protect lock leaves nothing\n";

  mutex::attributes ma;
  ma.mx_protocol = mutex::protocol::protect;
  ma.mx_robustness = mutex::robustness::robust;
  ma.mx_priority_ceiling = thread::priority::normal;
  static mutex mx{ "mx", ma };
  g_mx = &mx;

  // Above the ceiling: lock() must be refused.
  thread::attributes ha;
  ha.th_priority = thread::priority::above_normal;
  thread* high = new thread{ "high", high_func, nullptr, ha };
  high->join ();
  delete high;
  check (g_high_res == EINVAL, "lock above the ceiling is EINVAL", g_high_res);

  // At the ceiling, from a fresh thread and from main: plain lock/unlock.
  thread::attributes la;
  la.th_priority = thread::priority::normal;
  thread* low = new thread{ "low", low_func, nullptr, la };
  low->join ();
  delete low;
  check (g_low_lock == result::ok, "lock at the ceiling after it (thread)",
         g_low_lock);
  check (g_low_unlock == result::ok, "unlock (thread)", g_low_unlock);

  result_t r = mx.lock ();
  check (r == result::ok, "lock at the ceiling (main)", r);
  r = mx.unlock ();
  check (r == result::ok, "unlock (main)", r);

  const bool ok = (g_failures == 0);
  uart::uart1 << (ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  if (ok)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }
  return ok ? 0 : 1;
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
