/*
 * test-smp-boot.hpp - secondary-core bring-up shared by the SMP tests.
 *
 * Every multi-core test needs the same three things before it can start its
 * own threads: idle stacks, an idle body that parks a secondary core, and an
 * smp_install_boot_threads() that registers one idle thread per CPU. Each
 * application used to carry its own copy; they live here once.
 *
 * ISA-neutral: the only machine instructions involved are "dsb sy" and "wfi",
 * both of which assemble on ARMv7-A and ARMv8-A alike. Everything else goes
 * through the kernel API.
 */

#ifndef UOS_TEST_SMP_BOOT_HPP_
#define UOS_TEST_SMP_BOOT_HPP_

#include <cmsis-plus/rtos/os.h>

#include <cstdint>

namespace os
{
  namespace rtos
  {
    namespace scheduler
    {
      // Defined by the kernel in src/rtos/os-core.cpp, which publishes no
      // header for it; declaring it here keeps the tests from each repeating
      // the extern and keeps the kernel tree merge-clean against upstream.
      extern thread* os_idle_thread_core[OS_NCPU];
    } // namespace scheduler
  } // namespace rtos
} // namespace os

// The CPU index this code is running on, clamped to a valid array slot.
//
// Tests index per-core tallies by CPU. They used to write
// `port_cpu_id () & 3u`, which is a modulo only while the core count is a
// power of two: on a three-core port core 2 would land in slot 0 and the
// distribution report would be silently wrong. A clamp is correct for every
// count, and a port that reported an impossible CPU id would be a port bug,
// not something to fold into slot 0.
extern "C" unsigned port_cpu_id (void);

inline unsigned
cpu_slot (void)
{
  const unsigned id = port_cpu_id ();
  return (id < OS_NCPU) ? id : 0u;
}

// The core count as text, for banners and messages. A test that prints
// "4-core" is a test that lies on a three-core board, and it lies in the one
// line a reader trusts most. OS_NCPU is a board fact (boards/<id>/board.cmake),
// so the string follows it.
#define TEST_STR_(x) #x
#define TEST_STR(x)  TEST_STR_(x)
#define TEST_NCPU_STR TEST_STR (OS_NCPU)

// Stack words per secondary idle thread. Override per application with
// -DTEST_IDLE_STACK_WORDS=<n> when a port needs deeper idle stacks.
#ifndef TEST_IDLE_STACK_WORDS
#define TEST_IDLE_STACK_WORDS 512
#endif

// Idle body for a secondary core: drop to idle priority, then wait for an
// interrupt and yield forever.
extern "C" void*
test_secondary_idle_func (void*);

// Creates an idle thread for CPUs 1..OS_NCPU-1 and registers each one in
// os_idle_thread_core[]. The port's start-up path calls this by name, before
// smp::start_secondary_cores() releases the secondaries.
extern "C" void
smp_install_boot_threads (void);

// ---- Joining the secondaries -----------------------------------------------
//
// Every SMP test does the same three things once the secondaries have been
// released: wait for each of them to record its join stage, print what it
// found, and fold that into the verdict. Each test used to spell out cores 1,
// 2 and 3 by hand, which is a fact about the BCM2837 rather than about the
// test -- on a three-core board it reads one core past the end of the array.
// These follow OS_NCPU instead.
//
// g_core_stage[] is defined by the port (boards/<board>/src/smp.cpp); the
// stage a joined secondary records is kTestCoreJoined.

inline constexpr std::uint32_t kTestCoreJoined = 3u;

// Spins in 50 ms sleeps until every secondary has joined or timeout_ms has
// passed. @return the milliseconds actually waited.
extern "C" int
test_wait_secondaries (int timeout_ms);

// @return true when every secondary reached kTestCoreJoined.
extern "C" bool
test_secondaries_joined (void);

// @return "c1=3 c2=3 ..." for this port's core count, in static storage.
extern "C" const char*
test_join_summary (void);

#endif /* UOS_TEST_SMP_BOOT_HPP_ */
