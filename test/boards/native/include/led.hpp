/**
 * @file  led.hpp
 * @brief The user LED of the `native` board: a line on the console.
 * @details A host has no GPIO, and pretending otherwise would make the LED
 *          tests silently pass while proving nothing. So the LED is printed
 *          instead, and only when it CHANGES -- a test that blinks at 1 Hz
 *          produces two lines a second, not a flood.
 *
 *          The hardware finding this project already made is why it prints at
 *          all: every blinking test used to drive GPIO16 on a board whose LED
 *          is GPIO29, so nothing lit and nobody noticed. Output that can be
 *          read by a runner cannot fail that way.
 */
#pragma once

#include <cstdint>
#include <unistd.h>

namespace led {

namespace detail {

inline bool state = false;
inline bool state_valid = false;

inline void emit(bool on) noexcept {
    if (state_valid && state == on) return;
    state = on;
    state_valid = true;
    const char* s = on ? "[LED on]\n" : "[LED off]\n";
    std::size_t n = 0;
    while (s[n] != '\0') ++n;
    ssize_t r = ::write(1, s, n);
    (void)r;
}

} // namespace detail

inline void init() noexcept { detail::state_valid = false; detail::emit(false); }
inline void on()    noexcept { detail::emit(true); }
inline void off()   noexcept { detail::emit(false); }
inline void set(bool onoff) noexcept { onoff ? on() : off(); }

} // namespace led
