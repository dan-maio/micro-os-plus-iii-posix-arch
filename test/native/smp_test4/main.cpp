/*
 * smp_test4 — Raspberry Pi Zero 2W µOS++ no-affinity load balancing.
 *
 * Spawns NWORK CPU-bound worker threads with NO cpu_affinity, so the SMP
 * scheduler is free to place them on any of the OS_NCPU cores. Each worker records
 * how many time-slices it ran on each core; a reporter prints the per-worker
 * core histogram once a second and blinks the LED. If work lands on more than
 * one core (and across all of them over time), the load balancer is working.
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

extern "C" unsigned port_cpu_id(void);
using namespace os::rtos;

static constexpr unsigned NWORK = 8;
// Serialises console output: the reporter thread and the os_main RESULT line
// both write to the (non-reentrant) UART from different cores.
static mutex g_con { "con" };
static volatile std::uint32_t hist[NWORK][OS_NCPU] = {};

static void* worker (void* arg)
{
  unsigned id = (unsigned)(std::uintptr_t)arg;
  for (;;)
  {
    unsigned c = port_cpu_id();
    if (c < OS_NCPU) hist[id][c]++;
    // burn a little CPU so the scheduler has something to migrate
    volatile std::uint32_t x = 0;
    for (std::uint32_t i = 0; i < 200000; ++i) x += i;
    (void)x;
    this_thread::yield();
  }
  return nullptr;
}
static void* reporter (void*)
{
  bool s = false;
  for (;;)
  {
    s = !s; led::set(s);
    g_con.lock();
    for (unsigned w = 0; w < NWORK; ++w)
    {
      uart::uart1 << "w" << (int)w << " [";
      for (unsigned c = 0; c < OS_NCPU; ++c)
        uart::uart1 << " c" << (int)c << "=" << hist[w][c];
      uart::uart1 << " ]\n";
    }
    uart::uart1 << "-- reporter on c" << (int)port_cpu_id()
                << " t=" << (std::uint32_t)sysclock.now() << "ms --\n";
    g_con.unlock();
    sysclock.sleep_for(1000);
  }
  return nullptr;
}

extern "C"
{
  extern char __heap_start[]; extern char __heap_end[];
  extern char __fiq_stack_top[]; extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }
  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init(); led::init();
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ SMP TEST 4 : no-affinity load balancing ==+\n\n";
    os_startup_initialize_free_store(__heap_start,
        static_cast<std::size_t>(__heap_end - __heap_start));
    exception::init();
  }
  [[noreturn]] static void custom_main_trampoline (void) { std::exit (os_main (0, nullptr)); }
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
    attr.th_stack_address = main_stack; attr.th_stack_size_bytes = sizeof(main_stack);
    static thread main_thread { "main",
        reinterpret_cast<thread::func_t> (custom_main_trampoline), nullptr, attr };
    os_main_thread = &main_thread;
    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}

static thread::stack::element_t wstk[NWORK][4096];
static thread::stack::element_t rstk[4096];

int os_main (int, char*[])
{
  using uart::uart1;
  // Unmask IRQs before the scheduler starts. Portable across every
  // port: the architecture supplies the instruction, not the test.
  (void)os::rtos::interrupts::uncritical_section::enter ();
  smp_install_boot_threads();
  smp::start_secondary_cores();
  const int waited = test_wait_secondaries (3000);
  uart1 << "join: " << test_join_summary () << " (" << waited << "ms)\n";

  thread::attributes a = thread::initializer;
  static thread* workers[NWORK];
  static char wn[NWORK][4];
  for (unsigned w = 0; w < NWORK; ++w)
  {
    wn[w][0]='w'; wn[w][1]='0'+w; wn[w][2]=0;
    a.th_stack_address = wstk[w]; a.th_stack_size_bytes = sizeof(wstk[w]);
    workers[w] = new thread(wn[w], worker, (void*)(std::uintptr_t)w, a);
    // NO cpu_affinity() — let the scheduler load-balance.
  }
  a.th_stack_address = rstk; a.th_stack_size_bytes = sizeof(rstk);
  static thread t_rep { "reporter", reporter, nullptr, a };
  t_rep.cpu_affinity(1u << 0);   // keep the console on core 0 for tidy output

  // Run the load-balancing demo for exactly 10 one-second beats, then check the
  // load actually spread across every core (no-affinity balancing) and end
  // through a semihosted exit() (SEMIHOST builds). The idle fallback is only
  // reached on a plain SEMIHOST=0 / SD-boot image.
  constexpr std::uint32_t kCheckBeats = 10;
  for (std::uint32_t beats = 0; beats < kCheckBeats; ++beats)
    {
      sysclock.sleep_for(1000);
    }
  bool ok = test_secondaries_joined ();
  for (unsigned c = 0; ok && c < OS_NCPU; ++c)
    {
      std::uint32_t tot = 0;
      for (unsigned w = 0; w < NWORK; ++w)
        {
          tot += hist[w][c];
        }
      if (tot == 0)
        {
          ok = false;
        }
    }
  // Under the console mutex, so the marker a runner greps for is never
  // interleaved with a reporter line.
  g_con.lock();
  uart1 << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
  g_con.unlock();
  if (ok)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }
  for (;;) sysclock.sleep_for(1000);
}
