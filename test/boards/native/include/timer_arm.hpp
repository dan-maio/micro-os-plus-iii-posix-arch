/**
 * @file timer_arm.hpp
 * @brief The free-running counter of the `native` board.
 * @details Named timer_arm to match the two ARM ports, because the shared
 *          test sources name it -- smp-mat-test times its passes with
 *          get_count()/get_freq() and must not have to know which machine it
 *          is on. CLOCK_MONOTONIC at nanosecond resolution replaces CNTPCT.
 *
 *          init() and start_1ms() are no-ops: the per-CPU tick is armed by
 *          the port (host_cpu::start_this_cpu_tick), not by the board, since
 *          on this machine the timer is host-kernel-provided rather than
 *          board-provided.
 */

#ifndef TIMER_ARM_HPP
#define TIMER_ARM_HPP

#include <cstdint>
#include <ctime>

namespace timer_arm {

inline constexpr std::uint32_t TIMER_ARM_FALLBACK_FREQ = 1'000'000'000;

inline std::uint32_t get_freq() noexcept { return TIMER_ARM_FALLBACK_FREQ; }

inline std::uint64_t get_count() noexcept {
    struct timespec tp;
    ::clock_gettime(CLOCK_MONOTONIC, &tp);
    return static_cast<std::uint64_t>(tp.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(tp.tv_nsec);
}

// No countdown register on this machine; the tick is a timer_create() timer
// owned by the port. Reported as a full period so that any test computing
// "time since tick" from it gets a sane, monotone answer rather than noise.
inline void set_tval(std::uint32_t) noexcept {}
inline std::uint32_t get_tval() noexcept { return get_freq() / 1000; }
inline void set_ctl(std::uint32_t) noexcept {}
inline std::uint32_t get_ctl() noexcept { return 1; }

inline void init() noexcept {}
inline void start_1ms() noexcept {}

} // namespace timer_arm

#endif /* TIMER_ARM_HPP */
