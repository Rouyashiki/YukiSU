#pragma once

// POSIX headers are needed by the streaming line reader below, which has to stay
// a template (no std::function indirection) and therefore lives in this header.
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ksud {

// File system utilities
bool ensure_dir_exists(const std::string& path);
bool ensure_clean_dir(const std::string& path);
bool ensure_file_exists(const std::string& path);
bool ensure_binary(const std::string& path, const uint8_t* data, size_t size,
                   bool ignore_if_exist = false);

// Property utilities
std::optional<std::string> getprop(const std::string& prop);
bool is_safe_mode();

// Process utilities
enum class ProcessDaemonizeResult : std::uint8_t {
    Parent,
    Daemon,
    Error,
};

bool switch_mnt_ns(pid_t pid);
void detach_process_group(bool use_init_pgrp);
void switch_cgroups();
bool reset_stdio_to_devnull();
ProcessDaemonizeResult daemonize_process(bool use_init_pgrp);
void umask(mode_t mask);

// Magisk detection
bool has_magisk();

// Install/Uninstall
int install(const std::optional<std::string>& magiskboot_path,
            const std::optional<std::string>& libadbroot_path = std::nullopt);
int uninstall(const std::optional<std::string>& magiskboot_path);

// String utilities
std::string trim(const std::string& str);
/** Allocation-free trim: returns a view into `str`, not a copy. Prefer this over
 *  trim() wherever the result is only inspected or compared. Distinct name
 *  because an overload on string_view would be ambiguous for std::string args. */
std::string_view trim_view(std::string_view str);
std::vector<std::string> split(const std::string& str, char delim);
bool starts_with(std::string_view str, std::string_view prefix);
bool ends_with(std::string_view str, std::string_view suffix);
/** Append a lowercase hexadecimal integer without locale or stream state.
 *  `min_digits` pads the digits (not the optional 0x prefix) with zeroes. */
void append_hex(std::string* out, uint64_t value, bool prefix = true, size_t min_digits = 0);
void append_uint(std::string* out, uint64_t value);
void append_int(std::string* out, int64_t value);
/** Parse `<major>.<minor>...android<level>` from a kernel release string and
 *  return the canonical `android<level>-<major>.<minor>` KMI. */
std::optional<std::string> parse_kmi_string(std::string_view text);

/** Pop the next whitespace-delimited token from `rest`, advancing it past that
 *  token. Returns an empty view once exhausted, so it stands in for a failed
 *  `stream >> token`. Replaces `std::istringstream` for /proc line parsing. */
std::string_view next_token(std::string_view* rest);

/** Visit each '\n'-separated line of `text` as a view into it.
 *
 *  Replaces the `std::istringstream` + `std::getline` pattern: no stream, no
 *  per-line allocation, and no indirect call (a template, not std::function).
 *  Yields the same line set as getline -- a trailing newline does not produce a
 *  final empty line -- and additionally drops a trailing '\r' so CRLF config
 *  files parse the same as LF ones. */
template <typename Fn>
void for_each_line(std::string_view text, Fn&& fn) {
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        const bool last = (end == std::string_view::npos);
        if (last) {
            end = text.size();
        }
        std::string_view line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // A bool-returning callable can stop early, standing in for a `break`.
        if constexpr (std::is_same_v<decltype(fn(line)), bool>) {
            if (!fn(line)) {
                return;
            }
        } else {
            fn(line);
        }
        begin = last ? text.size() : end + 1;
    }
}

/** Like for_each_line but splits on an arbitrary delimiter and does not touch
 *  '\r'. Replaces `std::getline(stream, field, delim)` and matches its EOF
 *  behavior: a trailing delimiter does not produce a final empty field. */
template <typename Fn>
void for_each_field(std::string_view text, char delim, Fn&& fn) {
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find(delim, begin);
        const bool last = (end == std::string_view::npos);
        if (last) {
            end = text.size();
        }
        const std::string_view field = text.substr(begin, end - begin);
        if constexpr (std::is_same_v<decltype(fn(field)), bool>) {
            if (!fn(field)) {
                return;
            }
        } else {
            fn(field);
        }
        if (last) {
            return;
        }
        begin = end + 1;
    }
}

/** Stream lines from a file without loading it whole. `fn` returns false to stop
 *  reading. Use this instead of read_file()+for_each_line() for files that are
 *  large or scanned with an early exit -- /proc/kallsyms is several MB and its
 *  callers stop at the first module symbol. Returns false only if the open
 *  failed. */
template <typename Fn>
bool for_each_file_line(const char* path, Fn&& fn) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::string pending;
    pending.reserve(65536);
    char buf[65536];
    bool stop = false;
    while (!stop) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        pending.append(buf, static_cast<size_t>(n));
        size_t begin = 0;
        for (;;) {
            const size_t newline = pending.find('\n', begin);
            if (newline == std::string::npos) {
                break;
            }
            std::string_view line(pending.data() + begin, newline - begin);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!fn(line)) {
                stop = true;
                break;
            }
            begin = newline + 1;
        }
        pending.erase(0, begin);
    }
    if (!stop && !pending.empty()) {
        std::string_view line(pending);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        (void)fn(line);
    }
    close(fd);
    return true;
}

// File I/O
std::optional<std::string> read_file(const std::string& path);
bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>* data);
bool write_file(const std::filesystem::path& path, const std::string& content);
// Atomically replace a private state file; the parent directory must exist.
bool write_file_atomic(const std::filesystem::path& path, const std::string& content);
bool write_file_bytes(const std::filesystem::path& path, const uint8_t* data, size_t size,
                      mode_t mode = 0644);
bool copy_file_data(const std::filesystem::path& source, const std::filesystem::path& target,
                    mode_t mode = 0644, bool overwrite = true);
bool append_file(const std::filesystem::path& path, const std::string& content);

// Ask init for an orderly reboot. Falls back to spawning `reboot` only if the
// property write is refused.
void request_reboot();

// Create the file if it is missing and bump its mtime to now, which is all the
// `touch` this codebase used it for did.
bool touch_file(const std::filesystem::path& path);

// Use the installer's BusyBox decoder for compressed ZIP metadata.
std::optional<std::string> read_zip_entry(const std::string& zip_path, const char* entry_name);

// Command execution
struct ExecResult {
    int exit_code;
    std::string stdout_str;
    std::string stderr_str;
    int error_number = 0;
};
ExecResult exec_command(const std::vector<std::string>& args);
ExecResult exec_command(const std::vector<std::string>& args, const std::string& workdir);
ExecResult exec_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout);
/** Run built-in magiskboot in a forked child. argv[0] is set to "magiskboot";
 *  magiskboot_path is ignored (magiskboot is linked into this binary). */
ExecResult exec_command_magiskboot(const std::string& magiskboot_path,
                                   const std::vector<std::string>& sub_args,
                                   const std::string& workdir = "");
/** Run a read-only magiskboot subcommand in this process (no fork, no pipes) and
 *  return its exit status. Only for subcommands that report solely through the
 *  exit status and print nothing on success -- currently cpio exists/test/ls.
 *  Paths must be absolute: this does not chdir. */
int magiskboot_query(const std::vector<std::string>& sub_args);
int exec_command_async(const std::vector<std::string>& args);

// Exception-free number parsers. Return true on success.
bool parse_uint32(std::string_view s, uint32_t* out);
bool parse_uint64(std::string_view s, uint64_t* out);

}  // namespace ksud
