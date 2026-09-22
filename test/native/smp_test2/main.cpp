/*
 * smp_test2 — µOS++ Phase-2 SMP lock-coherency test.
 *
 * Brings up all OS_NCPU Cortex-A cores -- four on a BCM2837, three on an
 * RK3506. One worker thread is pinned to each core; every worker bumps a
 * shared counter under a µOS++ mutex ITER times. If the final counter ==
 * OS_NCPU*ITER, the LDREX/STREX kernel lock + cacheable/shareable DRAM
 * mapping are coherent across cores (no lost updates). Each line is tagged
 * with the core it actually ran on (port_cpu_id).
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

extern "C" unsigned port_cpu_id(void);

using namespace os::rtos;

static constexpr std::uint32_t ITER = 2000;
static volatile std::uint32_t g_prog[OS_NCPU] = {};

static mutex       g_mutex { "cnt" };
static volatile std::uint32_t g_counter = 0;
static volatile std::uint32_t g_done = 0;

static void* worker (void*)
{
  for (std::uint32_t i = 0; i < ITER; ++i)
    {
      g_mutex.lock();
      std::uint32_t v = g_counter;
      __asm__ volatile("" ::: "memory");
      g_counter = v + 1;                 // read-modify-write under the lock
      g_mutex.unlock();
      const unsigned me = port_cpu_id ();
      if (me < OS_NCPU)
        {
          g_prog[me] = i;
        }
      if ((i & 0x7F) == 0)
        this_thread::yield();
    }
  g_mutex.lock();
  uart::uart1 << "[c" << static_cast<int>(port_cpu_id())
              << "] worker done, counter=" << g_counter << "\n";
  ++g_done;
  g_mutex.unlock();
  return nullptr;
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
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP TEST 2 : " TEST_NCPU_STR "-core lock coherency ==+\n\n";
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

static thread::stack::element_t wstack[OS_NCPU][4096];

int os_main (int, char*[])
{
  using uart::uart1;
  // Unmask IRQs before the scheduler starts. Portable across every
  // port: the architecture supplies the instruction, not the test.
  (void)os::rtos::interrupts::uncritical_section::enter ();

  uart1 << "os_main on core " << static_cast<int>(port_cpu_id()) << "\n";
  smp_install_boot_threads();
  uart1 << "Releasing cores 1.." << (OS_NCPU - 1) << "...\n";
  smp::start_secondary_cores();

  const int waited = test_wait_secondaries (3000);
  uart1 << "join: " << test_join_summary () << " (" << waited << "ms)\n";

  thread::attributes attr = thread::initializer;
  static thread* workers[OS_NCPU];
  // Names must outlive the thread, and there is one per CPU, so they are
  // generated rather than listed -- a list is a statement about how many
  // cores the board has.
  static char wn[OS_NCPU][4];
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      wn[c][0] = 'w'; wn[c][1] = static_cast<char>('0' + c); wn[c][2] = '\0';
      attr.th_stack_address = wstack[c];
      attr.th_stack_size_bytes = sizeof(wstack[c]);
      workers[c] = new thread(wn[c], worker, nullptr, attr);
      workers[c]->cpu_affinity(1u << c);
    }

  while (g_done < OS_NCPU)
  {
    sysclock.sleep_for(500);
    uart1 << "progress:";
    for (unsigned c = 0; c < OS_NCPU; ++c)
      {
        uart1 << " " << wn[c] << "=" << g_prog[c];
      }
    uart1 << " done=" << g_done << " counter=" << g_counter << "\n";
  }

  std::uint32_t expected = ITER * OS_NCPU;
  uart1 << "\n==== RESULT ====\n";
  uart1 << "counter = " << g_counter << "  expected = " << expected << "\n";
  uart1 << (g_counter == expected
                ? "PASS: lock coherent across " TEST_NCPU_STR " cores\n"
                : "FAIL: lost updates!\n");
  uart1 << "\nRESULT: " << (g_counter == expected ? "PASS" : "FAIL") << "\n";
  if (g_counter == expected)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }
  for (;;) sysclock.sleep_for(10000);
}
