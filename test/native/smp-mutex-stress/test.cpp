/*
 * smp-mutex-stress - upstream's mutex stress, at the board's OS_NCPU.
 *
 * The body is upstream's (tests/sources/mutex-stress/src/test.cpp): ten
 * threads t0..t9 loop busy -> sleep -> lock -> busy -> sleep -> unlock, and a
 * periodic thread prints how evenly they got the mutex. What this copy adds
 * is the proof that the run was SMP rather than merely built SMP:
 *
 *   - t<i> is pinned to core i % OS_NCPU AT CONSTRUCTION (th_cpu_affinity in
 *     its attributes). Pinning after the constructor would race the other
 *     cores, which may already have picked the thread off the ready list.
 *   - every lock records the core it was taken on, and the verdict requires
 *     each core 0..OS_NCPU-1 to have taken the mutex;
 *   - a lock taken on any core but the thread's own is an affinity breach;
 *   - an atomic holder count catches two owners at once;
 *   - a plain counter is read before the in-lock sleep and written after it,
 *     so a mutex that let two cores in would lose updates, and the final
 *     value must equal the sum of the per-thread counts.
 *
 * rand() is replaced by a per-thread xorshift32. glibc's rand() takes a
 * process-wide lock; a µOS++ thread preempted while holding it (preemption is
 * a signal on this port) would block every other thread that calls rand() on
 * the same host CPU -- and a pinned thread cannot be resumed elsewhere.
 */

#include <cstring>
#include <cstdio>
#include <atomic>

#include <test.h>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <cmath>

extern "C" unsigned port_cpu_id (void);

using namespace os;
using namespace os::rtos;

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
#pragma clang diagnostic ignored "-Wmissing-variable-declarations"
#endif

mutex mx;

#pragma GCC diagnostic pop

// The per-core evidence.
static std::atomic<unsigned> g_holders{ 0 };
static std::atomic<unsigned> g_overlaps{ 0 };
static std::atomic<unsigned> g_wrong_core{ 0 };
static std::atomic<unsigned> g_locks_on_core[OS_NCPU];
static volatile unsigned g_counter = 0; // written only under mx

class periodic;

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wpadded"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpadded"
#endif

class mutex_test
{
public:
  mutex_test (const char* name, unsigned core, std::uint32_t seed);

  void*
  object_main (void);

  rtos::thread&
  thread (void)
  {
    return th_;
  }

protected:
  friend class periodic;
  friend int run_tests (unsigned int, std::uint32_t);

  static thread::attributes
  pinned (unsigned core)
  {
    thread::attributes a = thread::initializer;
    a.th_cpu_affinity = (1u << core);
    return a;
  }

  unsigned int
  next (void)
  {
    rnd_ ^= rnd_ << 13;
    rnd_ ^= rnd_ >> 17;
    rnd_ ^= rnd_ << 5;
    return rnd_;
  }

  unsigned int min_micros_ = 10;
  unsigned int max_micros_ = 90;
  unsigned int min_ticks_ = 10;
  unsigned int max_ticks_ = 200;

  clock::duration_t ticks_ = 0;
  unsigned int accumulated_count_ = 0;
  unsigned int count_ = 0;

  unsigned core_;
  std::uint32_t rnd_;

  rtos::thread th_;
};

#pragma GCC diagnostic pop

mutex_test::mutex_test (const char* name, unsigned core, std::uint32_t seed)
    : core_ (core), rnd_ (seed | 1u),
      th_{ name,
           [] (void* attr) -> void* {
             return static_cast<mutex_test*> (attr)->object_main ();
           },
           this, pinned (core) }
{
  trace::printf ("%s @%p %s core %u\n", __func__, this, name, core);
}

void*
mutex_test::object_main (void)
{
  while (!thread ().interrupted ())
    {
      unsigned int nbusy = (next () % (max_micros_ - min_micros_))
                           + min_micros_;
      unsigned int nsleep = (next () % (max_ticks_ - min_ticks_))
                            + min_ticks_;

      busy_wait (nbusy);

      sysclock.sleep_for (nsleep);
      ticks_ += nsleep;

      // At the end run_tests() interrupts every thread, and one blocked
      // here then returns EINTR WITHOUT the mutex. Upstream ignores the
      // result; counted as a holder it would read as two owners at once.
      if (mx.lock () != result::ok)
        {
          break;
        }
      {
        if (g_holders.fetch_add (1) != 0)
          {
            g_overlaps++;
          }
        const unsigned c = port_cpu_id ();
        if (c != core_)
          {
            g_wrong_core++;
          }
        if (c < OS_NCPU)
          {
            g_locks_on_core[c]++;
          }
        const unsigned v = g_counter;

        nbusy = (next () % (max_micros_ / 10 - min_micros_ / 10))
                + min_micros_ / 10;
        nsleep = (next () % (max_ticks_ / 10 - min_ticks_ / 10))
                 + min_ticks_ / 10;

        busy_wait (nbusy);

        sysclock.sleep_for (nsleep);
        ticks_ += nsleep;

        g_counter = v + 1; // lost if another holder got in meanwhile
        accumulated_count_++;
        count_++;
        g_holders.fetch_sub (1);
      }
      mx.unlock ();
    }
  return nullptr;
}

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-variable-declarations"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
mutex_test* mt[10];
#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wpadded"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpadded"
#endif

class periodic
{
public:
  periodic (unsigned int seconds);

  void*
  object_main (void);

  rtos::thread&
  thread (void)
  {
    return th_;
  }

protected:
  unsigned int seconds_;
  rtos::thread th_;
};

#pragma GCC diagnostic pop

periodic::periodic (unsigned int seconds)
    : seconds_ (seconds), //
      th_{ "P",
           [] (void* attr) -> void* {
             return static_cast<periodic*> (attr)->object_main ();
           },
           this }
{
  trace::printf ("%s @%p\n", __func__, this);
}

void*
periodic::object_main (void)
{
  th_.priority (thread::priority::above_normal);

  unsigned int t = 0;
  while (true)
    {
      sysclock.sleep_for (5000);
      t += 5;

      {
        scheduler::critical_section scs;

        printf ("[%3us] ", t);

        unsigned int sum = 0;
        for (auto m : mt)
          {
            unsigned int cnt = m->accumulated_count_;

            sum += cnt;

            printf ("%s:%-4u ", m->thread ().name (), cnt);
          }
        unsigned int count = sizeof (mt) / sizeof (mt[0]);
        int average = static_cast<int> ((sum + (count / 2)) / count);

        printf ("sum=%u, avg=%d", sum, average);

        int min = 0;
        int max = 0;

        unsigned int sigma_squares_sum = 0;

        for (auto m : mt)
          {
            int delta = static_cast<int> (m->accumulated_count_);
            delta -= average;

            sigma_squares_sum += static_cast<unsigned int> (delta * delta);

            if (delta < min)
              min = delta;

            if (delta > max)
              max = delta;
          }

        int sigma = static_cast<int> (
            sqrt (static_cast<double> (sigma_squares_sum / count)));
        printf (", sigma=%d", sigma);

        if (average != 0)
          {
            printf (", delta in [%d,%d] [%d%%,%d%%]", min, max,
                    (min * 100 + average / 2) / average,
                    (max * 100 + average / 2) / average);
          }

        printf (", per core:");
        for (unsigned c = 0; c < OS_NCPU; ++c)
          {
            printf (" c%u=%u", c, g_locks_on_core[c].load ());
          }

        puts ("");
      }

      if (seconds_ != 0 && t > seconds_)
        break;
    }

  for (auto m : mt)
    {
      m->thread ().interrupt ();
      m->thread ().join ();
    }
  return nullptr;
}

int
run_tests (unsigned int seconds, std::uint32_t seed)
{
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      g_locks_on_core[c] = 0;
    }

  mutex_test mt0 ("t0", 0 % OS_NCPU, seed + 0);
  mutex_test mt1 ("t1", 1 % OS_NCPU, seed + 1);
  mutex_test mt2 ("t2", 2 % OS_NCPU, seed + 2);
  mutex_test mt3 ("t3", 3 % OS_NCPU, seed + 3);
  mutex_test mt4 ("t4", 4 % OS_NCPU, seed + 4);
  mutex_test mt5 ("t5", 5 % OS_NCPU, seed + 5);
  mutex_test mt6 ("t6", 6 % OS_NCPU, seed + 6);
  mutex_test mt7 ("t7", 7 % OS_NCPU, seed + 7);
  mutex_test mt8 ("t8", 8 % OS_NCPU, seed + 8);
  mutex_test mt9 ("t9", 9 % OS_NCPU, seed + 9);

#pragma GCC diagnostic push
#if defined(__clang__)
#if __clang_major__ == 17 || __clang_major__ == 18
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
#endif
  mt[0] = &mt0;
  mt[1] = &mt1;
  mt[2] = &mt2;
  mt[3] = &mt3;
  mt[4] = &mt4;
  mt[5] = &mt5;
  mt[6] = &mt6;
  mt[7] = &mt7;
  mt[8] = &mt8;
  mt[9] = &mt9;
#pragma GCC diagnostic pop

  periodic pm{ seconds };

  pm.thread ().join ();

  // The verdict.
  int status = 0;
  unsigned sum = 0;
  for (auto m : mt)
    {
      sum += m->accumulated_count_;
    }

  printf ("\n==== per-core checks (OS_NCPU=%u) ====\n",
          static_cast<unsigned> (OS_NCPU));
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      const unsigned n = g_locks_on_core[c].load ();
      printf ("core %u: %u locks%s\n", c, n, (n == 0) ? "  <- NEVER" : "");
      if (n == 0)
        {
          status = 1;
        }
    }
  printf ("overlapping holders: %u\n", g_overlaps.load ());
  printf ("locks off the pinned core: %u\n", g_wrong_core.load ());
  printf ("counter=%u, sum of thread counts=%u\n", g_counter, sum);

  if (g_overlaps.load () != 0 || g_wrong_core.load () != 0
      || g_counter != sum || sum == 0)
    {
      status = 1;
    }

  puts ("Done.");
  return status;
}
