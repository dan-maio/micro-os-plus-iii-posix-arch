/**
 * @file  hw_result.hpp
 * @brief Bounded "done" helper, POSIX host.
 *
 * The sibling of micro-os-plus-iii-aarch64/include/hw_result.hpp. A test
 * prints its RESULT line and then calls ok()/fail().
 *
 * On the silicon ports these compile to a semihosted SYS_EXIT under SEMIHOST
 * and to nothing otherwise, so a plain image keeps its idle-forever
 * behaviour. Here there is no "otherwise": the host always has an exit
 * status, and a test process that idled for ever would have to be killed by
 * the runner and could not report anything. So they always end the run, and
 * the verdict reaches the shell as the exit code.
 *
 * _exit(), not exit(). Other CPUs are still running threads; exit() would run
 * static destructors underneath them and turn a clean PASS into a crash in
 * the teardown.
 *
 * But _exit() does not flush stdio either, so these flush first. Most tests
 * here print through uart::uart1, which is write(2) and therefore already on
 * its way out; a test carried from upstream's own suite prints with printf(),
 * and to a pipe -- which is how the runner captures it -- stdout is fully
 * buffered. Without the flush its entire output, verdict included, is
 * discarded at the exit. (Found exactly that way: mutex-stress ran, passed and
 * printed nothing.)
 *
 * Pattern in a test:
 *
 *     uart1 << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
 *     if (ok) hw_result::ok (); else hw_result::fail ();
 *     for (;;) sysclock.sleep_for (1000);   // never reached
 */
#pragma once

#include <cstdio>
#include <unistd.h>

namespace hw_result
{
  [[noreturn]] inline void
  ok () noexcept
  {
    std::fflush (nullptr);
    ::_exit (0);
  }

  [[noreturn]] inline void
  fail () noexcept
  {
    std::fflush (nullptr);
    ::_exit (1);
  }
} // namespace hw_result
