/*
 * test-console.hpp - serialised formatted output for the test applications.
 *
 * Every SMP test prints from several cores at once, and uart1/semihosting are
 * not re-entrant: without a lock a "RESULT: PASS" line can be split by another
 * core's output, and the runners grep for that exact string. Each application
 * used to carry its own copy of this helper; they are collected here so the
 * locking rule is stated once.
 *
 * The UART itself is supplied by the architecture port (uart.hpp), so this
 * header is ISA-neutral.
 */

#ifndef UOS_TEST_CONSOLE_HPP_
#define UOS_TEST_CONSOLE_HPP_

#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace test
{
  // One lock for every console path below, so puts() and puts_uart() cannot
  // interleave with each other either.
  inline os::rtos::mutex&
  console_mutex (void)
  {
    static os::rtos::mutex m { "con" };
    return m;
  }

  namespace detail
  {
    // Formats into `buf` and always NUL-terminates, whatever vsnprintf
    // returns; a negative result and an overlong result both truncate.
    inline void
    format (char* buf, std::size_t size, const char* fmt, std::va_list args)
    {
      int n = std::vsnprintf (buf, size, fmt, args);
      if (n < 0)
        {
          n = 0;
        }
      if (n > static_cast<int> (size) - 1)
        {
          n = static_cast<int> (size) - 1;
        }
      buf[n] = '\0';
    }
  } // namespace detail
} // namespace test

// Console output: UART plus, on a SEMIHOST build, the semihosting channel, so
// banners and status lines reach the emulator or the OpenOCD log.
inline void
console (const char* fmt, ...)
{
  char b[256];
  std::va_list args;
  va_start (args, fmt);
  test::detail::format (b, sizeof (b), fmt, args);
  va_end (args);

  test::console_mutex ().lock ();
  uart::uart1.puts (b);
  test::console_mutex ().unlock ();
}

// UART only, for the high-volume lines (file-content streams, per-tick
// status) that would otherwise cost one semihosting trap each.
inline void
console_uart (const char* fmt, ...)
{
  char b[256];
  std::va_list args;
  va_start (args, fmt);
  test::detail::format (b, sizeof (b), fmt, args);
  va_end (args);

  test::console_mutex ().lock ();
  uart::uart1.puts_uart (b);
  test::console_mutex ().unlock ();
}


// Prints one per-CPU tally array as "<label> : c0=.. c1=.. ..", for as many
// cores as this port has. Tests that report work distribution used to spell
// out c0 through c3, which is a statement about the BCM2837 rather than about
// the test.
inline void
report_per_core (const char* label, const std::uint32_t* counts)
{
  char b[32 * OS_NCPU + 64];
  int n = std::snprintf (b, sizeof (b), "  %s : ", label);
  for (unsigned c = 0; c < OS_NCPU && n > 0 && n < static_cast<int> (sizeof (b)); ++c)
    {
      const int w = std::snprintf (b + n, sizeof (b) - static_cast<unsigned> (n),
                                   "c%u=%u ", c, counts[c]);
      if (w <= 0)
        {
          break;
        }
      n += w;
    }
  console ("%s\n", b);
}

#endif /* UOS_TEST_CONSOLE_HPP_ */
