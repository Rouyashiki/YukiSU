#include "log.hpp"
#include <unistd.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "terminal.hpp"
#ifdef __ANDROID__
#include <android/log.h>
#endif  // #ifdef __ANDROID__

namespace ksud {

namespace {

constexpr size_t kLogTagSize = 32U;

LogLevel g_log_level = LogLevel::INFO;
bool g_log_stderr_enabled = true;
bool g_log_cli_mode = false;
bool g_log_verbose = false;
std::array<char, kLogTagSize> g_log_tag = {"KernelSU"};

void log_write(LogLevel level, const char* fmt, va_list args) {
    if (level < g_log_level) {
        return;
    }

    const char* level_str = "?";
    int android_level = 4;
    switch (level) {
    case LogLevel::VERBOSE:
        level_str = "V";
        android_level = 2;
        break;
    case LogLevel::DEBUG:
        level_str = "D";
        android_level = 3;
        break;
    case LogLevel::INFO:
        level_str = "I";
        android_level = 4;
        break;
    case LogLevel::WARN:
        level_str = "W";
        android_level = 5;
        break;
    case LogLevel::ERROR:
        level_str = "E";
        android_level = 6;
        break;
    default:
        break;
    }

    std::array<char, 1024> msg{};
    const int vsn_ret = vsnprintf(msg.data(), msg.size(), fmt, args);
    if (vsn_ret < 0 || static_cast<size_t>(vsn_ret) >= msg.size()) {
        msg[0] = '\0';
    }

#ifdef __ANDROID__
    (void)__android_log_write(android_level, g_log_tag.data(), msg.data());
#else
    (void)android_level;
#endif

    if (!g_log_stderr_enabled)
        return;

    if (g_log_cli_mode && !g_log_verbose && isatty(STDERR_FILENO) == 1) {
        if (level >= LogLevel::WARN)
            terminal::message(stderr, level == LogLevel::ERROR ? "error" : "warning", msg.data());
        return;
    }

    const time_t now = time(nullptr);
    const struct tm* tm_info = localtime(&now);
    std::array<char, 32> time_buf{};
    const size_t time_len = strftime(time_buf.data(), time_buf.size(), "%m-%d %H:%M:%S", tm_info);
    if (time_len == 0U) {
        time_buf[0] = '\0';
    }

    // Use stderr so stdout stays clean for CLI output (manager parses stdout for module list etc.)
    (void)fprintf(stderr, "%s %s/%s: %s\n", time_buf.data(), level_str, g_log_tag.data(),
                  msg.data());
    (void)fflush(stderr);
}

}  // namespace

void log_init(const char* tag) {
    strncpy(g_log_tag.data(), tag, g_log_tag.size() - 1);
    g_log_tag[g_log_tag.size() - 1] = '\0';
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

void log_set_stderr_enabled(bool enabled) {
    g_log_stderr_enabled = enabled;
}

void log_set_cli_mode(bool verbose) {
    g_log_cli_mode = isatty(STDERR_FILENO) == 1;
    g_log_verbose = verbose;
    if (verbose)
        log_set_level(LogLevel::DEBUG);
}

void log_flush() {
    (void)fflush(stderr);
}

// C-style variadic required for LOGx("fmt", ...) call sites; implementation delegates to
// log_write(va_list). NOLINT: cert-dcl50-cpp
void log_v(const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::VERBOSE, fmt, args);
    va_end(args);
}

void log_d(const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::DEBUG, fmt, args);
    va_end(args);
}

void log_i(const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::INFO, fmt, args);
    va_end(args);
}

void log_w(const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::WARN, fmt, args);
    va_end(args);
}

void log_e(const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list args;
    va_start(args, fmt);
    log_write(LogLevel::ERROR, fmt, args);
    va_end(args);
}

}  // namespace ksud
