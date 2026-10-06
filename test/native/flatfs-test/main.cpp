// flatfs-test: appending to a file that was created empty.
//
// write_file() gives a zero-length file no extent (start = 0). append_file()
// used to grow that "extent" in place anyway: first = start + old_sectors -
// data_start wrapped around, the bounds check passed on the wrapped sum, and
// the append wrote from physical sector 0 -- over the superblock and the file
// table. The append here is large enough (128 sectors, data_start is 33 on the
// default 32 MiB image) for the wrapped range to pass that check.
//
// Checks: the append succeeds, the file reads back exactly, a file written
// beside it is untouched, and the volume still mounts with both files.

#include <cmsis-plus/rtos/os.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <hw_result.hpp>
#include <sd.hpp>
#include <flatfs.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
  sd::SdCard g_card;
  flatfs::FlatFs g_fs;

  constexpr std::uint32_t kOtherSize = 1500;
  constexpr std::uint32_t kAppendSize = 128 * flatfs::kBlockSize; // 64 KiB

  std::uint8_t g_other[kOtherSize];
  std::uint8_t g_data[kAppendSize];
  std::uint8_t g_back[kAppendSize + flatfs::kBlockSize];

  unsigned g_failures = 0;

  void
  say (const char* s)
  {
    uart::uart1 << s;
  }

  void
  check (bool cond, const char* what)
  {
    char b[160];
    std::snprintf (b, sizeof (b), "  %-52s %s\n", what, cond ? "ok" : "FAIL");
    say (b);
    if (!cond)
      {
        ++g_failures;
      }
  }

  bool
  same_file (flatfs::FlatFs& fs, const char* name, const std::uint8_t* want,
             std::uint32_t size)
  {
    std::uint32_t got = 0;
    if (fs.read_file (name, g_back, sizeof (g_back), &got)
        != flatfs::FlatFs::Result::ok)
      {
        return false;
      }
    return got == size && std::memcmp (g_back, want, size) == 0;
  }
} // namespace

int
os_main (int, char*[])
{
  say ("\nflatfs-test: append to a file created empty\n");

  for (std::uint32_t i = 0; i < kOtherSize; ++i)
    {
      g_other[i] = static_cast<std::uint8_t> (0xA5 ^ i);
    }
  for (std::uint32_t i = 0; i < kAppendSize; ++i)
    {
      g_data[i] = static_cast<std::uint8_t> (i * 7 + (i >> 9));
    }

  if (!g_card.init ())
    {
      say ("  SD init failed\nRESULT: FAIL\n");
      hw_result::fail ();
      return 1;
    }
  check (g_fs.format (g_card) == flatfs::FlatFs::Result::ok, "format");

  using R = flatfs::FlatFs::Result;
  check (g_fs.write_file ("log", nullptr, 0) == R::ok, "write_file(log, 0 bytes)");
  check (g_fs.write_file ("other", g_other, kOtherSize) == R::ok,
         "write_file(other, 1500 bytes)");

  R r = g_fs.append_file ("log", g_data, kAppendSize);
  {
    char b[96];
    std::snprintf (b, sizeof (b), "  append_file(log, %u bytes) -> %s\n",
                   static_cast<unsigned> (kAppendSize),
                   flatfs::FlatFs::result_str (r));
    say (b);
  }
  check (r == R::ok, "append to the empty file succeeds");

  std::uint32_t size = 0;
  check (g_fs.file_size ("log", size) == R::ok && size == kAppendSize,
         "log size is the appended size");
  check (same_file (g_fs, "log", g_data, kAppendSize), "log reads back exactly");
  check (same_file (g_fs, "other", g_other, kOtherSize), "other is untouched");

  // The superblock and the file table survive: a fresh mount sees both.
  flatfs::FlatFs again;
  check (again.mount (g_card) == R::ok, "volume mounts again");
  std::uint32_t total = 0, free_sectors = 0, files = 0;
  check (again.stat (total, free_sectors, files) == R::ok && files == 2,
         "two files after remount");
  check (same_file (again, "log", g_data, kAppendSize),
         "log reads back after remount");
  check (same_file (again, "other", g_other, kOtherSize),
         "other reads back after remount");

  const bool ok = (g_failures == 0);
  say (ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
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
    using namespace os::rtos;

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
