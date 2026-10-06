/**
 * @file  uart.hpp
 * @brief The console of the `native` board: file descriptor 1.
 * @details Same streaming API (`putc`, `puts`, `print_hex`, `print_dec`,
 *          `operator<<`, global `uart::uart1`) as the BCM2837 and RK3506
 *          boards, so every carried test compiles against it unchanged. That
 *          is the whole reason this file exists: a test names `uart::uart1`
 *          and must not have to know whether that is a PL011 or a pipe.
 *
 *          write(2), not printf(3). Output happens from inside signal
 *          handlers and from several CPUs at once; write() is
 *          async-signal-safe and stdio is not. It is also unbuffered, so a
 *          test that deadlocks still shows everything it printed before it
 *          did -- which is exactly when the output matters most.
 */
#pragma once

#include <cstdint>
#include <unistd.h>

// ---- Board label used by the runtime test banners ---------------------------
#define PORT_BANNER_LONG  "POSIX synthetic host"
#define PORT_BANNER_SHORT "native"

// The shared test applications print the ISA and the CPU complex through
// these, so one copy of a test can name the machine it is running on.
#if defined(__x86_64__)
#define PORT_BANNER_ISA   "x86-64"
#elif defined(__aarch64__)
#define PORT_BANNER_ISA   "AArch64"
#else
#define PORT_BANNER_ISA   "host"
#endif

#if !defined(PORT_BANNER_CPU)
#define PORT_BANNER_CPU   "POSIX host threads as CPUs"
#endif

namespace uart {

class Uart1 {
public:
    Uart1() = default;
    Uart1(const Uart1&) = delete;
    Uart1& operator=(const Uart1&) = delete;

    void init() noexcept { initialized_ = true; }

    void putc(char c) const noexcept { write_raw(&c, 1); }

    void puts(const char* str) const noexcept {
        std::size_t n = 0;
        while (str[n] != '\0') ++n;
        write_raw(str, n);
    }

    // On the silicon boards these two skip the semihosting mirror. Here there
    // is only one channel, so they are the same thing -- kept so the shared
    // test sources compile without a branch.
    void putc_uart(char c) const noexcept { putc(c); }
    void puts_uart(const char* str) const noexcept { puts(str); }

private:
    void write_raw(const char* p, std::size_t n) const noexcept {
        while (n > 0) {
            ssize_t w = ::write(1, p, n);
            if (w <= 0) {
                // EINTR cannot reach here (SA_RESTART), and a closed stdout
                // is not something a test can recover from.
                return;
            }
            p += w;
            n -= static_cast<std::size_t>(w);
        }
    }

public:
    void print_dec(std::uint32_t value) const noexcept {
        char digits[10];
        int pos = 0;
        if (value == 0) { digits[pos++] = '0'; }
        else { while (value > 0) { digits[pos++] = static_cast<char>('0' + (value % 10)); value /= 10; } }
        char out[11];
        int n = 0;
        while (pos > 0) out[n++] = digits[--pos];
        out[n] = '\0';
        puts(out);
    }

    void print_hex(std::uint32_t value, int width = 8) const noexcept {
        static constexpr char hex_chars[] = "0123456789ABCDEF";
        if (width < 1) width = 1;
        if (width > 8) width = 8;
        char out[9];
        int n = 0;
        for (int i = width - 1; i >= 0; --i)
            out[n++] = hex_chars[(value >> (i * 4)) & 0xF];
        out[n] = '\0';
        puts(out);
    }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    const Uart1& operator<<(const char* str) const noexcept { puts(str); return *this; }
    const Uart1& operator<<(char c) const noexcept { putc(c); return *this; }
    const Uart1& operator<<(std::uint32_t value) const noexcept { print_dec(value); return *this; }
    const Uart1& operator<<(int value) const noexcept {
        std::uint32_t mag = (value < 0)
            ? (0u - static_cast<std::uint32_t>(value))
            : static_cast<std::uint32_t>(value);
        if (value < 0) putc('-');
        print_dec(mag);
        return *this;
    }

private:
    bool initialized_ = false;
};

inline Uart1 uart1;
inline constexpr const char* endl = "\n";

} // namespace uart
