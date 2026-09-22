/*
 * test-smp-boot.cpp - see test-smp-boot.hpp.
 */

#include <test-smp-boot.hpp>

#include <cstdio>

using namespace os::rtos;

namespace
{
  thread::stack::element_t idle_stack[OS_NCPU][TEST_IDLE_STACK_WORDS];

  // Thread names must outlive the thread, so they are built once into static
  // storage. Generated rather than listed, so the helper follows OS_NCPU
  // instead of assuming the four cores of a BCM2837.
  char idle_name[OS_NCPU][8];

  // Room for "cN=S " per secondary, plus the terminator.
  char join_summary[OS_NCPU * 8 + 1];
} // namespace

// Defined by the port, in boards/<board>/src/smp.cpp. Declared here rather
// than included from <smp.hpp> so this file stays board-neutral.
extern "C" volatile std::uint32_t g_core_stage[OS_NCPU];

extern "C" void*
test_secondary_idle_func (void*)
{
  this_thread::thread ().priority (thread::priority::idle);
  for (;;)
    {
      // The kernel already has this as a port API, and every port implements
      // it: "dsb sy; wfi" on ARMv8-A, "dsb; wfi" on ARMv7-A, "__DSB(); __WFI()"
      // on Cortex-M, and sigsuspend() on the POSIX host, which has no such
      // instruction to spell. The raw asm that used to be here was the last
      // line of assembly left in a shared test source.
      port::scheduler::wait_for_interrupt ();
      this_thread::yield ();
    }
  return nullptr;
}

extern "C" void
smp_install_boot_threads (void)
{
  for (unsigned c = 1; c < OS_NCPU; ++c)
    {
      std::snprintf (idle_name[c], sizeof (idle_name[c]), "idle%u", c);

      thread::attributes a = thread::initializer;
      a.th_stack_address = idle_stack[c];
      a.th_stack_size_bytes = sizeof (idle_stack[c]);

      scheduler::os_idle_thread_core[c]
          = new thread (idle_name[c], test_secondary_idle_func, nullptr, a);
    }
}

extern "C" bool
test_secondaries_joined (void)
{
  for (unsigned c = 1; c < OS_NCPU; ++c)
    {
      if (g_core_stage[c] < kTestCoreJoined)
        {
          return false;
        }
    }
  return true;
}

extern "C" int
test_wait_secondaries (int timeout_ms)
{
  int waited = 0;
  while (!test_secondaries_joined () && waited < timeout_ms)
    {
      sysclock.sleep_for (50);
      waited += 50;
    }
  return waited;
}

extern "C" const char*
test_join_summary (void)
{
  int n = 0;
  for (unsigned c = 1; c < OS_NCPU; ++c)
    {
      const int w = std::snprintf (join_summary + n,
                                   sizeof (join_summary) - static_cast<unsigned> (n),
                                   (c == 1) ? "c%u=%u" : " c%u=%u", c,
                                   static_cast<unsigned> (g_core_stage[c]));
      if (w <= 0)
        {
          break;
        }
      n += w;
    }
  join_summary[(n > 0) ? n : 0] = '\0';
  return join_summary;
}
