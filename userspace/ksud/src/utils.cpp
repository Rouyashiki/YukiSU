#include "utils.hpp"
#include "boot/boot_patch.hpp"
#include "core/assets.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif  // #ifdef __ANDROID__

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
extern int magiskboot_main(int argc, char** argv);
#endif  // #if defined(MAGISKBOOT_ALONE_AVAILABLE)...

namespace ksud {

namespace {

// magiskboot is welded into this binary as a multi-call entry. Keep the
// availability check in one place so the callers below read the same either way.
int run_magiskboot_main(int argc, char** argv) {
#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
    return magiskboot_main(argc, argv);
#else
    (void)argc;
    (void)argv;
    LOGE("magiskboot is not built into this ksud");
    return 127;
#endif  // #if defined(MAGISKBOOT_ALONE_AVAILABLE)...
}

// Byte-for-byte file copy. The stream version (`dst << src.rdbuf()`) went
// through streambuf one character at a time; this is a plain bulk read/write.
bool copy_file_contents_impl(const char* src_path, const char* dst_path, mode_t mode,
                             bool overwrite) {
    const int in_fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0)
        return false;
    const int out_flags = O_WRONLY | O_CREAT | O_CLOEXEC | (overwrite ? O_TRUNC : O_EXCL);
    const int out_fd = open(dst_path, out_flags, mode);
    if (out_fd < 0) {
        close(in_fd);
        return false;
    }

    bool ok = true;
    std::array<char, 65536> buf{};
    for (;;) {
        const ssize_t n = read(in_fd, buf.data(), buf.size());
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            const ssize_t w = write(out_fd, buf.data() + written, static_cast<size_t>(n) - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            if (w == 0) {
                // Cannot happen for a non-zero count per POSIX; guard anyway so a
                // misbehaving fd cannot spin here forever.
                ok = false;
                break;
            }
            written += static_cast<size_t>(w);
        }
        if (!ok)
            break;
    }
    if (close(out_fd) != 0)
        ok = false;
    close(in_fd);
    return ok;
}

}  // namespace

bool copy_file_data(const std::filesystem::path& source, const std::filesystem::path& target,
                    mode_t mode, bool overwrite) {
    return copy_file_contents_impl(source.c_str(), target.c_str(), mode, overwrite);
}

bool ensure_dir_exists(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Create directory recursively
    std::string current;
    current.reserve(path.size());
    for (const char c : path) {
        current += c;
        if (c == '/' && !current.empty()) {
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                LOGE("Failed to create directory %s: %s", current.c_str(), strerror(errno));
                return false;
            }
        }
    }

    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("Failed to create directory %s: %s", path.c_str(), strerror(errno));
        return false;  // NOLINT(readability-simplify-boolean-expr)
    }
    return true;
}

bool ensure_clean_dir(const std::string& path) {
    LOGD("ensure_clean_dir: %s", path.c_str());

    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (ec) {
            LOGE("Failed to remove existing directory %s: %s", path.c_str(), ec.message().c_str());
            return false;
        }
    }

    return ensure_dir_exists(path);
}

bool ensure_file_exists(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            struct stat st{};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                return true;
            }
        }
        return false;
    }
    close(fd);
    return true;
}

bool ensure_binary(const std::string& path, const uint8_t* data, size_t size,
                   bool ignore_if_exist) {
    if (ignore_if_exist) {
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) {
            return true;
        }
    }

    // Ensure parent directory exists
    const size_t pos = path.rfind('/');
    if (pos != std::string::npos) {
        const std::string parent = path.substr(0, pos);
        if (!ensure_dir_exists(parent)) {
            return false;
        }
    }

    // Remove existing file
    unlink(path.c_str());

    // Write file
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        LOGE("Failed to create %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    const ssize_t written = write(fd, data, size);
    close(fd);

    if (written != static_cast<ssize_t>(size)) {
        LOGE("Failed to write %s: %s", path.c_str(), strerror(errno));
        return false;  // NOLINT(readability-simplify-boolean-expr)
    }
    return true;
}

std::optional<std::string> getprop(const std::string& prop) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {0};
    int const len = __system_property_get(prop.c_str(), value);
    if (len > 0) {
        return std::string(value);
    }
    return std::nullopt;
#else
    // On non-Android, try to read from environment or /proc
    // This is a stub for testing purposes
    (void)prop;
    return std::nullopt;
#endif  // #ifdef __ANDROID__
}

bool is_safe_mode() {
    auto persist_safemode = getprop("persist.sys.safemode");
    if (persist_safemode && *persist_safemode == "1") {
        LOGI("safemode: true (persist.sys.safemode)");
        return true;
    }

    auto ro_safemode = getprop("ro.sys.safemode");
    if (ro_safemode && *ro_safemode == "1") {
        LOGI("safemode: true (ro.sys.safemode)");
        return true;
    }

    // Check kernel safemode via ksucalls (volume down key detection)
    const bool kernel_safemode = check_kernel_safemode();
    if (kernel_safemode) {
        LOGI("safemode: true (kernel volume down)");
        return true;
    }

    return false;
}

bool switch_mnt_ns(pid_t pid) {
    std::array<char, 64> path{};
    const int snp_ret = snprintf(path.data(), path.size(), "/proc/%d/ns/mnt", pid);
    if (snp_ret < 0 || static_cast<size_t>(snp_ret) >= path.size()) {
        return false;
    }

    const int fd = open(path.data(), O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open %s: %s", path.data(), strerror(errno));
        return false;
    }

    // Save current directory
    std::array<char, PATH_MAX> cwd{};
    const char* cwd_result = getcwd(cwd.data(), cwd.size());

    // Switch namespace
    if (setns(fd, CLONE_NEWNS) != 0) {
        LOGE("Failed to setns: %s", strerror(errno));
        close(fd);
        return false;
    }
    close(fd);

    // Restore current directory
    if (cwd_result != nullptr) {
        chdir(cwd.data());
    }

    return true;
}

void detach_process_group(bool use_init_pgrp) {
    if (use_init_pgrp) {
        if (set_init_pgrp() == 0) {
            return;
        }
        LOGW("Failed to switch to init process group, falling back to a private group");
    }

    if (setpgid(0, 0) != 0) {
        LOGW("Failed to detach process group: %s", strerror(errno));
    }
}

namespace {
void switch_cgroup(const char* grp, pid_t pid) {
    const std::string path = std::string(grp) + "/cgroup.procs";

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return;
    }

    (void)append_file(path, std::to_string(pid));
}
}  // namespace

void switch_cgroups() {
    const pid_t pid = getpid();
    switch_cgroup("/acct", pid);
    switch_cgroup("/dev/cg2_bpf", pid);
    switch_cgroup("/sys/fs/cgroup", pid);

    auto per_app_memcg = getprop("ro.config.per_app_memcg");
    if (!per_app_memcg || *per_app_memcg != "false") {
        switch_cgroup("/dev/memcg/apps", pid);
    }
}

bool reset_stdio_to_devnull() {
    const int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        LOGE("Failed to open /dev/null: %s", strerror(errno));
        return false;
    }

    bool success = true;
    for (const int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (devnull != target && dup2(devnull, target) < 0) {
            LOGE("Failed to redirect fd %d to /dev/null: %s", target, strerror(errno));
            success = false;
            break;
        }
    }
    if (devnull > STDERR_FILENO)
        close(devnull);
    return success;
}

ProcessDaemonizeResult daemonize_process(bool use_init_pgrp) {
    const pid_t child = fork();
    if (child < 0) {
        LOGE("Failed to fork daemon launcher: %s", strerror(errno));
        return ProcessDaemonizeResult::Error;
    }

    if (child > 0) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            LOGE("Daemon launcher failed to initialize");
            return ProcessDaemonizeResult::Error;
        }
        return ProcessDaemonizeResult::Parent;
    }

    detach_process_group(use_init_pgrp);
    switch_cgroups();
    if (!reset_stdio_to_devnull())
        _exit(1);

    const pid_t daemon = fork();
    if (daemon < 0)
        _exit(1);
    if (daemon > 0)
        _exit(0);

    return ProcessDaemonizeResult::Daemon;
}

void umask(mode_t mask) {  // NOLINT(misc-unused-parameters) forwarded to ::umask
    ::umask(mask);
}

bool has_magisk() {
    // Check if magisk binary exists in PATH
    const char* path_env = getenv("PATH");
    if (!path_env)
        return false;

    const std::string_view path_str(path_env);
    std::string candidate;
    for (size_t begin = 0; begin <= path_str.size();) {
        const size_t end = std::min(path_str.find(':', begin), path_str.size());
        const std::string_view dir = path_str.substr(begin, end - begin);
        begin = end + 1;
        if (dir.empty())
            continue;
        candidate.assign(dir);
        candidate += "/magisk";
        if (access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
    }

    return false;
}

std::string_view trim_view(std::string_view str) {
    const size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos)
        return {};
    return str.substr(start, str.find_last_not_of(" \t\n\r") - start + 1);
}

std::string trim(const std::string& str) {
    return std::string(trim_view(str));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> result;
    // Reproduce the getline loop this replaced exactly: an empty input yields no
    // fields at all, and a trailing delimiter terminates the last field rather
    // than starting an empty one after it. Interior and leading empty fields are
    // kept. Current callers skip empty lines anyway, but this is a public helper.
    if (str.empty()) {
        return result;
    }
    result.reserve(1 + static_cast<size_t>(std::count(str.begin(), str.end(), delim)));
    for (size_t begin = 0; begin <= str.size();) {
        const size_t end = std::min(str.find(delim, begin), str.size());
        result.emplace_back(str, begin, end - begin);
        begin = end + 1;
    }
    if (str.back() == delim) {
        result.pop_back();
    }
    return result;
}

bool starts_with(std::string_view str, std::string_view prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void append_hex(std::string* out, uint64_t value, bool prefix, size_t min_digits) {
    std::array<char, 16> digits{};
    const auto [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (error != std::errc{})
        return;
    if (prefix)
        out->append("0x");
    const size_t count = static_cast<size_t>(end - digits.data());
    if (count < min_digits)
        out->append(min_digits - count, '0');
    out->append(digits.data(), end);
}

void append_uint(std::string* out, uint64_t value) {
    std::array<char, 20> digits{};
    const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (error == std::errc{})
        out->append(digits.data(), end);
}

void append_int(std::string* out, int64_t value) {
    std::array<char, 20> digits{};
    const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (error == std::errc{})
        out->append(digits.data(), end);
}

std::optional<std::string> parse_kmi_string(std::string_view text) {
    const auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    const auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
    };
    constexpr std::string_view kAndroid = "android";

    for (size_t begin = 0; begin < text.size(); ++begin) {
        if (!is_digit(text[begin]))
            continue;
        size_t cursor = begin;
        while (cursor < text.size() && is_digit(text[cursor]))
            ++cursor;
        if (cursor >= text.size() || text[cursor] != '.')
            continue;
        ++cursor;
        const size_t minor_begin = cursor;
        while (cursor < text.size() && is_digit(text[cursor]))
            ++cursor;
        if (cursor == minor_begin)
            continue;
        const size_t version_end = cursor;

        size_t android_begin = std::string_view::npos;
        size_t android_end = 0;
        while (cursor < text.size() && !is_space(text[cursor])) {
            if (cursor + kAndroid.size() < text.size() &&
                text.compare(cursor, kAndroid.size(), kAndroid) == 0 &&
                is_digit(text[cursor + kAndroid.size()])) {
                size_t end = cursor + kAndroid.size() + 1;
                while (end < text.size() && is_digit(text[end]))
                    ++end;
                // The old greedy pattern selected the last satisfiable android
                // tag before whitespace, so keep scanning.
                android_begin = cursor;
                android_end = end;
            }
            ++cursor;
        }
        if (android_begin == std::string_view::npos)
            continue;

        std::string kmi(text.substr(android_begin, android_end - android_begin));
        kmi += '-';
        kmi.append(text.substr(begin, version_end - begin));
        return kmi;
    }
    return std::nullopt;
}

std::string_view next_token(std::string_view* rest) {
    constexpr std::string_view kSpace = " \t\r\n\f\v";
    const size_t begin = rest->find_first_not_of(kSpace);
    if (begin == std::string_view::npos) {
        *rest = {};
        return {};
    }
    const size_t end = rest->find_first_of(kSpace, begin);
    const std::string_view token = rest->substr(begin, end - begin);
    *rest = (end == std::string_view::npos) ? std::string_view{} : rest->substr(end);
    return token;
}

std::optional<std::string> read_file(const std::string& path) {
    // The stream version copied the contents three times (filebuf -> stringbuf
    // -> returned string) and had no size hint, so the stringbuf grew
    // geometrically. One fstat plus one reserve gets it down to a single
    // allocation for regular files; /proc entries report st_size 0 and just
    // grow from the first chunk.
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;

    std::string out;
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        out.reserve(static_cast<size_t>(st.st_size));
    } else {
        // procfs/sysfs report st_size=0 despite having content.
        out.reserve(8192);
    }

    std::array<char, 8192> buf{};
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        close(fd);
        return std::nullopt;
    }
    close(fd);
    return out;
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>* data) {
    if (data == nullptr)
        return false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat status{};
    if (fstat(fd, &status) != 0 || status.st_size < 0 ||
        static_cast<uintmax_t>(status.st_size) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
        close(fd);
        return false;
    }
    data->resize(static_cast<size_t>(status.st_size));
    size_t filled = 0;
    while (filled < data->size()) {
        const ssize_t count = read(fd, data->data() + filled, data->size() - filled);
        if (count > 0) {
            filled += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        close(fd);
        data->clear();
        return false;
    }
    close(fd);
    return true;
}

namespace {

bool write_bytes_impl(const char* path, const void* data, size_t size, int extra_flags,
                      mode_t mode) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | extra_flags, mode);
    if (fd < 0)
        return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, bytes + written, size - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        close(fd);
        return false;
    }
    return close(fd) == 0;
}

// Shared by write_file/append_file: O_TRUNC vs O_APPEND is the only difference.
bool write_file_impl(const char* path, const std::string& content, int extra_flags) {
    return write_bytes_impl(path, content.data(), content.size(), extra_flags, 0644);
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool write_file(const std::filesystem::path& path, const std::string& content) {
    return write_file_impl(path.c_str(), content, O_TRUNC);
}

bool write_file_atomic(const std::filesystem::path& path, const std::string& content) {
    std::string temporary = path.string() + ".tmp.XXXXXX";
    const int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0)
        return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t n = write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            ok = false;
            break;
        }
        offset += static_cast<size_t>(n);
    }
    if (ok)
        ok = fsync(fd) == 0;
    int error = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        error = errno;
    }
    if (ok && rename(temporary.c_str(), path.c_str()) != 0) {
        ok = false;
        error = errno;
    }
    if (!ok) {
        unlink(temporary.c_str());
        errno = error;
    }
    return ok;
}

bool write_file_bytes(const std::filesystem::path& path, const uint8_t* data, size_t size,
                      mode_t mode) {
    if (size != 0 && data == nullptr)
        return false;
    return write_bytes_impl(path.c_str(), data, size, O_TRUNC, mode);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void request_reboot() {
#ifdef __ANDROID__
    // /system/bin/reboot is itself just a property write; spawning it bought no
    // privilege, because the child inherits this process's SELinux domain. Do the
    // same write directly. Deliberately not the reboot() syscall: that skips
    // init's orderly shutdown, which is how /data gets corrupted.
    if (__system_property_set("sys.powerctl", "reboot") == 0) {
        return;
    }
    LOGW("sys.powerctl was refused; falling back to the reboot binary");
#endif  // #ifdef __ANDROID__
    (void)exec_command({"reboot"});
}

bool touch_file(const std::filesystem::path& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    // futimens with a null times argument sets both stamps to now: the mtime half
    // of touch. Creating the file above covers the other half.
    const bool ok = futimens(fd, nullptr) == 0;
    return close(fd) == 0 && ok;
}

std::optional<std::string> read_zip_entry(const std::string& zip_path, const char* entry_name) {
    struct stat status{};
    if (stat(zip_path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) {
        LOGE("zip: %s is not a non-empty regular file", zip_path.c_str());
        return std::nullopt;
    }

    const std::string absolute_path = std::filesystem::absolute(zip_path).string();
    auto result = exec_command({BUSYBOX_PATH, "unzip", "-p", absolute_path, entry_name},
                               std::chrono::seconds(30));
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        LOGE("zip: cannot read %s from %s: %s", entry_name, zip_path.c_str(),
             result.stderr_str.c_str());
        return std::nullopt;
    }
    return std::move(result.stdout_str);
}

bool append_file(const std::filesystem::path& path, const std::string& content) {
    return write_file_impl(path.c_str(), content, O_APPEND);
}

ExecResult exec_command_magiskboot(const std::string& magiskboot_path,
                                   const std::vector<std::string>& sub_args,
                                   const std::string& workdir) {
    // magiskboot_path is accepted for API compatibility only: magiskboot is
    // linked into this binary, so there is nothing to locate or exec.
    (void)magiskboot_path;

    std::vector<std::string> args;
    args.reserve(1 + sub_args.size());
    args.emplace_back("magiskboot");
    for (const auto& a : sub_args)
        args.push_back(a);

    ExecResult result{-1, "", ""};

    std::array<int, 2> stdout_pipe{};
    std::array<int, 2> stderr_pipe{};
    if (pipe(stdout_pipe.data()) != 0 || pipe(stderr_pipe.data()) != 0)
        return result;
    // Drain our own stdio before forking. The child inherits a copy of these
    // buffers, and it flushes them below; anything still pending here would be
    // replayed into the pipe and read back as magiskboot output. The old execv
    // discarded the inherited buffers, so this had no equivalent before. A failure
    // here leaves the data stuck in the buffer rather than in the pipe, so there is
    // nothing to recover and nothing useful to report.
    (void)fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return result;
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        if (!workdir.empty() && chdir(workdir.c_str()) != 0) {
            _exit(127);
        }
        // Call magiskboot_main directly instead of re-exec'ing /proc/self/exe:
        // it is already in this image, so an exec would only repeat dynamic
        // linking, relocation processing and .init_array for no benefit. The
        // fork stays, so magiskboot keeps its own cwd, its global state, and
        // containment for the std::abort() paths in boot_crypto.
        std::vector<char*> c_args;
        c_args.reserve(args.size() + 1U);
        for (auto& arg : args)
            c_args.push_back(arg.data());
        c_args.push_back(nullptr);
        int rc = run_magiskboot_main(static_cast<int>(args.size()), c_args.data());
        // magiskboot writes its output with buffered stdio, and stdout here is a
        // pipe, so it is fully buffered. execv used to end in exit(), which
        // flushes; _exit does not. Without this, callers that parse stdout --
        // "cpio ls -r", the unpack format probe -- silently see nothing. If the
        // flush itself fails the output is gone, so do not report success and let
        // the caller act on an empty parse.
        if (fflush(nullptr) != 0 && rc == 0) {
            rc = 1;
        }
        _exit(rc);
    }
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    // Cap capture at 1MB per stream to prevent cache/memory explosion if magiskboot
    // enters an infinite loop writing output.
    constexpr size_t kMaxCapture = 1024ULL * 1024;
    // Single-threaded drain of both pipes. Two std::threads per call cost a
    // pthread create/join and a guard page + stack mapping each, for work that
    // is pure I/O waiting.
    std::array<pollfd, 2> fds{};
    fds[0] = {stdout_pipe[0], POLLIN, 0};
    fds[1] = {stderr_pipe[0], POLLIN, 0};
    std::array<std::string*, 2> sinks{&result.stdout_str, &result.stderr_str};
    constexpr std::array<bool, 2> kTee{false, true};  // stderr is mirrored live
    int open_fds = 2;
    std::array<char, 4096> buf{};
    while (open_fds > 0) {
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].fd < 0 || (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                continue;
            const ssize_t n = read(fds[i].fd, buf.data(), buf.size());
            if (n > 0) {
                std::string* out = sinks[i];
                if (out->size() < kMaxCapture) {
                    out->append(buf.data(),
                                std::min(static_cast<size_t>(n), kMaxCapture - out->size()));
                }
                if (kTee[i]) {
                    (void)fwrite(buf.data(), 1, static_cast<size_t>(n), stdout);
                    (void)fflush(stdout);
                }
                continue;
            }
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }
    }
    for (auto& pfd : fds) {
        if (pfd.fd >= 0)
            close(pfd.fd);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        result.exit_code = 128 + sig;
        result.stderr_str.append("magiskboot terminated by signal ");
        result.stderr_str.append(std::to_string(sig));
        const char* sig_name = strsignal(sig);
        if (sig_name && sig_name[0]) {
            result.stderr_str.append(" (");
            result.stderr_str.append(sig_name);
            result.stderr_str.append(")");
        }
        result.stderr_str.push_back('\n');
    }
    return result;
}

int magiskboot_query(const std::vector<std::string>& sub_args) {
    // Read-only magiskboot subcommands (cpio exists/test/ls) report through the
    // exit status and print nothing on the success path, so they need neither a
    // pipe nor a child: run them in this process. Callers must pass absolute
    // paths -- there is no chdir here, by design.
    std::vector<std::string> args;
    args.reserve(1 + sub_args.size());
    args.emplace_back("magiskboot");
    for (const auto& a : sub_args)
        args.push_back(a);

    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1U);
    for (auto& arg : args)
        c_args.push_back(arg.data());
    c_args.push_back(nullptr);
    return run_magiskboot_main(static_cast<int>(args.size()), c_args.data());
}

int exec_command_async(const std::vector<std::string>& args) {
    if (args.empty())
        return -1;

    const pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        // Child process
        std::vector<char*> c_args;
        c_args.reserve(args.size() + 1U);
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    return 0;
}

namespace {

bool install_daemon_atomically() {
    const int source = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (source < 0) {
        LOGE("Failed to open /proc/self/exe: %s", strerror(errno));
        return false;
    }
    struct stat source_stat{};
    if (fstat(source, &source_stat) != 0 || !S_ISREG(source_stat.st_mode) ||
        source_stat.st_size <= 0) {
        LOGE("Invalid /proc/self/exe source");
        close(source);
        return false;
    }

    std::string temporary = std::string(DAEMON_PATH) + ".tmp.XXXXXX";
    std::vector<char> temporary_path(temporary.begin(), temporary.end());
    temporary_path.push_back('\0');
    const int target = mkstemp(temporary_path.data());
    if (target < 0) {
        LOGE("Failed to create temporary ksud: %s", strerror(errno));
        close(source);
        return false;
    }

    bool success = true;
    std::array<char, 65536> buffer{};
    while (success) {
        const ssize_t count = read(source, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            LOGE("Failed to read /proc/self/exe: %s", strerror(errno));
            success = false;
            break;
        }

        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            const ssize_t written =
                write(target, buffer.data() + offset, static_cast<size_t>(count) - offset);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                LOGE("Failed to write temporary ksud: %s", strerror(errno));
                success = false;
                break;
            }
            if (written == 0) {
                LOGE("Failed to write temporary ksud: zero-length write");
                success = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
    }

    if (success && fchmod(target, 0755) != 0) {
        LOGE("Failed to chmod temporary ksud: %s", strerror(errno));
        success = false;
    }
    if (success && !lsetfilecon(temporary_path.data(), KSU_CON))
        success = false;
    if (success && fsync(target) != 0) {
        LOGE("Failed to sync temporary ksud: %s", strerror(errno));
        success = false;
    }

    close(source);
    if (close(target) != 0 && success) {
        LOGE("Failed to close temporary ksud: %s", strerror(errno));
        success = false;
    }

    if (!success) {
        unlink(temporary_path.data());
        return false;
    }

    if (rename(temporary_path.data(), DAEMON_PATH) != 0) {
        LOGE("Failed to replace %s: %s", DAEMON_PATH, strerror(errno));
        unlink(temporary_path.data());
        return false;
    }

    const int directory = open(ADB_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        if (fsync(directory) != 0)
            LOGW("Failed to sync %s after ksud update: %s", ADB_DIR, strerror(errno));
        close(directory);
    }
    return true;
}

bool copy_optional_file(const std::optional<std::string>& src_path, const char* dst_path,
                        mode_t mode) {
    if (!src_path)
        return true;
    const int source = open(src_path->c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (source < 0) {
        LOGE("Failed to open %s: %s", src_path->c_str(), strerror(errno));
        return false;
    }
    struct stat status{};
    if (fstat(source, &status) != 0 || !S_ISREG(status.st_mode)) {
        LOGE("Input is not a readable regular file: %s", src_path->c_str());
        close(source);
        return false;
    }
    std::string temporary = std::string(dst_path) + ".tmp.XXXXXX";
    const int target = mkostemp(temporary.data(), O_CLOEXEC);
    if (target < 0) {
        LOGE("Failed to stage %s: %s", dst_path, strerror(errno));
        close(source);
        return false;
    }
    bool ok = true;
    std::array<char, 65536> buffer{};
    while (ok) {
        const ssize_t count = read(source, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = count == 0;
            break;
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            const ssize_t written =
                write(target, buffer.data() + offset, static_cast<size_t>(count) - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                if (written == 0)
                    errno = EIO;
                ok = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
    }
    if (ok)
        ok = fchmod(target, mode) == 0 && fsync(target) == 0;
    int error = errno;
    close(source);
    if (close(target) != 0 && ok) {
        ok = false;
        error = errno;
    }
    // Replace the directory entry, never the target of an existing tool symlink.
    if (ok && rename(temporary.c_str(), dst_path) != 0) {
        ok = false;
        error = errno;
    }
    if (!ok) {
        unlink(temporary.c_str());
        LOGE("Failed to copy %s from %s: %s", dst_path, src_path->c_str(), strerror(error));
        return false;
    }
    (void)restorecon(std::filesystem::path(dst_path), false);
    return true;
}

}  // namespace

int install(const std::optional<std::string>& magiskboot_path,
            const std::optional<std::string>& libadbroot_path) {
    if (!ensure_dir_exists(ADB_DIR)) {
        LOGE("Failed to create %s", ADB_DIR);
        return 1;
    }

    if (!install_daemon_atomically()) {
        LOGE("Failed to copy ksud");
        return 1;
    }

    // Restore SELinux contexts
    if (!restorecon()) {
        LOGW("Failed to restore SELinux contexts");
    }

    // Ensure BINARY_DIR and multi-call symlinks exist
    if (ensure_binaries(false) != 0) {
        LOGW("Failed to ensure binaries");
    }

    // Create symlink
    if (!ensure_dir_exists(BINARY_DIR)) {
        LOGE("Failed to create %s", BINARY_DIR);
        return 1;
    }

    unlink(DAEMON_LINK_PATH);
    if (symlink(DAEMON_PATH, DAEMON_LINK_PATH) != 0) {
        LOGW("Failed to create symlink: %s", strerror(errno));
    }

    // Copy magiskboot if provided
    if (magiskboot_path) {
        if (!copy_optional_file(magiskboot_path, MAGISKBOOT_PATH, 0755)) {
            return 1;
        }
    }

    if (libadbroot_path) {
        if (!ensure_dir_exists(LIBRARY_DIR)) {
            LOGE("Failed to create %s", LIBRARY_DIR);
            return 1;
        }

        if (!copy_optional_file(libadbroot_path, LIBADBROOT_PATH, 0644)) {
            return 1;
        }

        (void)restorecon(std::filesystem::path(LIBRARY_DIR), false);
    }

    return 0;
}

int uninstall(const std::optional<std::string>& magiskboot_path) {
    // Uninstall modules
    std::error_code fs_error;
    if (std::filesystem::exists(MODULE_DIR, fs_error) && !fs_error) {
        printf("- Uninstall modules..\n");
        // Disable all modules
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(MODULE_DIR, ec);
             it != std::filesystem::directory_iterator() && !ec; it.increment(ec)) {
            if (it->is_directory()) {
                const std::string disable_file = it->path().string() + "/disable";
                const int marker = open(disable_file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
                if (marker >= 0)
                    close(marker);
            }
        }
        if (ec) {
            LOGW("Error disabling modules: %s", ec.message().c_str());
        }
    }

    printf("- Removing directories..\n");
    fs_error.clear();
    std::filesystem::remove_all(WORKING_DIR, fs_error);
    if (fs_error)
        LOGW("Failed to remove %s: %s", WORKING_DIR, fs_error.message().c_str());
    fs_error.clear();
    std::filesystem::remove(DAEMON_PATH, fs_error);
    if (fs_error)
        LOGW("Failed to remove %s: %s", DAEMON_PATH, fs_error.message().c_str());
    fs_error.clear();
    std::filesystem::remove_all(MODULE_DIR, fs_error);
    if (fs_error)
        LOGW("Failed to remove %s: %s", MODULE_DIR, fs_error.message().c_str());

    printf("- Restore boot image..\n");
    std::vector<std::string> restore_args;
    if (magiskboot_path) {
        restore_args.push_back("--magiskboot");
        restore_args.push_back(*magiskboot_path);
    }
    restore_args.push_back("--flash");

    const int ret = boot_restore(restore_args);
    if (ret != 0) {
        LOGE("Boot image restoration failed");
        printf("Warning: Failed to restore boot image, you may need to manually restore\n");
    }

    printf("- Uninstall YukiSU manager..\n");
    const auto uninstall_result = exec_command({"pm", "uninstall", "com.anatdx.yukisu"});
    if (uninstall_result.exit_code != 0) {
        LOGW("Manager uninstall failed: %s", uninstall_result.stderr_str.c_str());
    }

    printf("- Rebooting in 5 seconds..\n");
    sleep(5);
    request_reboot();

    return 0;
}

bool parse_uint32(std::string_view s, uint32_t* out) {
    if (s.empty() || out == nullptr)
        return false;
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, 10);
    if (error != std::errc{} || end != s.data() + s.size())
        return false;
    *out = value;
    return true;
}

bool parse_uint64(std::string_view s, uint64_t* out) {
    if (s.empty() || out == nullptr)
        return false;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, 10);
    if (error != std::errc{} || end != s.data() + s.size())
        return false;
    *out = value;
    return true;
}
}  // namespace ksud
