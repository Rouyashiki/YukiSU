#include "lua_engine.hpp"

#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "json.hpp"
#include "plugin.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <fcntl.h>
#include <linux/close_range.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...

namespace ksud {

namespace fs = std::filesystem;

PluginCodeSnapshot::PluginCodeSnapshot(int directory_fd)
    : directory_fd_(directory_fd), directory_("/proc/self/fd/" + std::to_string(directory_fd)) {}

PluginCodeSnapshot::~PluginCodeSnapshot() {
    if (directory_fd_ >= 0)
        close(directory_fd_);
}

namespace {

constexpr size_t kMaxCommandOutput = size_t{4} * 1024 * 1024;
constexpr size_t kMaxFileInput = size_t{4} * 1024 * 1024;
constexpr size_t kMaxDirectoryEntries = 10000;
constexpr size_t kMaxDirectoryEntryBytes = size_t{4} * 1024 * 1024;
constexpr size_t kMaxLogMessage = size_t{64} * 1024;
constexpr size_t kMaxCommandArguments = 256;
constexpr size_t kMaxCommandArgumentBytes = size_t{1024} * 1024;
constexpr size_t kMaxPropertyNameBytes = 256;
constexpr size_t kMaxCallbackBytes = 64;
constexpr size_t kMaxPathBytes = 4096;
constexpr size_t kMaxJsonInput = size_t{4} * 1024 * 1024;
constexpr size_t kMaxJsonDepth = 32;
constexpr size_t kMaxJsonNodes = 10000;
constexpr size_t kMaxLuaMemory = size_t{64} * 1024 * 1024;
constexpr int kStageCallbackTimeoutSeconds = 120;
constexpr int kManualCallbackTimeoutSeconds = 300;
constexpr int kDaemonReadyTimeoutMilliseconds = 10000;
constexpr uint32_t kDaemonStateMagic = 0x4b504c47;

struct DaemonState {
    uint32_t magic = kDaemonStateMagic;
    int32_t pid = -1;
    int32_t process_group = -1;
    uint8_t ready = 0;
    uint8_t reserved[3]{};
};

void print_error(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    (void)std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void print_output(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    (void)std::vfprintf(stdout, format, arguments);
    va_end(arguments);
}

struct PluginApiContext {
    std::string id;
    std::string directory;
};

struct CapturedCommand {
    int exit_code = -1;
    std::string standard_output;
    std::string standard_error;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::string error;
};

struct JsonEncodeContext {
    std::set<const void*> tables;
    size_t nodes = 0;
    size_t output_bytes = 0;
    std::string error;
};

struct LuaMemoryContext {
    size_t used = 0;
};

struct LuaStateCloser {
    void operator()(lua_State* state) const {
        if (state)
            lua_close(state);
    }
};

using ScopedLuaState = std::unique_ptr<lua_State, LuaStateCloser>;

void* limited_lua_allocator(void* opaque, void* pointer, size_t old_size, size_t new_size) {
    auto* context = static_cast<LuaMemoryContext*>(opaque);
    if (pointer == nullptr)
        old_size = 0;
    if (new_size == 0) {
        std::free(pointer);  // NOLINT(cppcoreguidelines-no-malloc): required by Lua allocator ABI.
        context->used = old_size <= context->used ? context->used - old_size : 0;
        return nullptr;
    }
    if (new_size > old_size && new_size - old_size > kMaxLuaMemory - context->used)
        return nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): required by Lua allocator ABI.
    void* resized = std::realloc(pointer, new_size);
    if (resized != nullptr)
        context->used = context->used - old_size + new_size;
    return resized;
}

void log_line(PluginApiContext* context, const char* level, const std::string& message) {
    if (std::strcmp(level, "W") == 0) {
        LOGW("plugin %s: %s", context->id.c_str(), message.c_str());
        print_error("[Lua] %s\n", message.c_str());
    } else {
        LOGI("plugin %s: %s", context->id.c_str(), message.c_str());
        print_output("[Lua] %s\n", message.c_str());
    }
    plugin_append_log(context->directory, level, message);
}

PluginApiContext* api_context(lua_State* state) {
    return static_cast<PluginApiContext*>(lua_touserdata(state, lua_upvalueindex(1)));
}

bool require_string(lua_State* state, int index) {
    return lua_isstring(state, index) != 0;
}

bool copy_lua_string(lua_State* state, int index, size_t maximum, std::string* output) {
    if (!require_string(state, index))
        return false;
    size_t size = 0;
    const char* value = lua_tolstring(state, index, &size);
    if (size > maximum || std::memchr(value, '\0', size) != nullptr)
        return false;
    output->assign(value, size);
    return true;
}

std::string bounded_lua_message(lua_State* state, int index, std::string_view fallback) {
    size_t size = 0;
    const char* value = lua_tolstring(state, index, &size);
    if (!value)
        return std::string(fallback);
    return {value, std::min(size, kMaxLogMessage)};
}

int traceback_handler(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (message)
        luaL_traceback(state, state, message, 1);
    else
        lua_pushliteral(state, "Lua callback failed without an error message");
    return 1;
}

int table_lookup_handler(lua_State* state) {
    lua_settop(state, 2);
    lua_gettable(state, 1);
    return 1;
}

bool push_table_value(lua_State* state, int table, std::string_view key, std::string* error) {
    table = lua_absindex(state, table);
    lua_pushcfunction(state, traceback_handler);
    const int error_handler = lua_gettop(state);
    lua_pushcfunction(state, table_lookup_handler);
    lua_pushvalue(state, table);
    lua_pushlstring(state, key.data(), key.size());
    const int status = lua_pcall(state, 2, 1, error_handler);
    lua_remove(state, error_handler);
    if (status == LUA_OK)
        return true;
    *error = bounded_lua_message(state, -1, "table lookup failed without an error message");
    return false;
}

bool redirect_standard_streams() {
    const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0)
        return false;
    bool success = true;
    for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
        if (dup2(devnull, descriptor) < 0)
            success = false;
    }
    if (devnull > STDERR_FILENO)
        close(devnull);
    return success;
}

bool create_internal_pipe(int descriptors[2]) {
    if (pipe2(descriptors, O_CLOEXEC) != 0)
        return false;
    for (int index = 0; index < 2; ++index) {
        if (descriptors[index] > STDERR_FILENO)
            continue;
        const int promoted = fcntl(descriptors[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (promoted < 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            descriptors[0] = -1;
            descriptors[1] = -1;
            return false;
        }
        close(descriptors[index]);
        descriptors[index] = promoted;
    }
    return true;
}

void mark_nonstandard_fds_cloexec() {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1),
                std::numeric_limits<unsigned int>::max(), CLOSE_RANGE_CLOEXEC) == 0) {
        return;
    }
#endif
    rlimit limit{};
    const rlim_t maximum = getrlimit(RLIMIT_NOFILE, &limit) == 0
                               ? std::min<rlim_t>(limit.rlim_cur, static_cast<rlim_t>(1U << 20U))
                               : static_cast<rlim_t>(65536);
    for (int descriptor = STDERR_FILENO + 1; static_cast<rlim_t>(descriptor) < maximum;
         ++descriptor) {
        const int flags = fcntl(descriptor, F_GETFD);
        if (flags >= 0)
            (void)fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
}

int lua_cloexec_process_call(lua_State* state) {
    const int arguments = lua_gettop(state);
    mark_nonstandard_fds_cloexec();
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, arguments, LUA_MULTRET);
    return lua_gettop(state);
}

int lua_cloexec_file_call(lua_State* state) {
    const int results = lua_cloexec_process_call(state);
    if (results > 0) {
        auto* stream = static_cast<luaL_Stream*>(luaL_testudata(state, 1, LUA_FILEHANDLE));
        if (stream && stream->f) {
            const int descriptor = fileno(stream->f);
            const int flags = descriptor >= 0 ? fcntl(descriptor, F_GETFD) : -1;
            if (flags >= 0)
                (void)fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    return results;
}

void wrap_library_function(lua_State* state, const char* library, const char* function,
                           lua_CFunction wrapper) {
    lua_getglobal(state, library);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_getfield(state, -1, function);
    if (lua_isfunction(state, -1)) {
        lua_pushcclosure(state, wrapper, 1);
        lua_setfield(state, -2, function);
    } else {
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

void protect_library_descriptors(lua_State* state) {
    wrap_library_function(state, "io", "open", lua_cloexec_file_call);
    wrap_library_function(state, "io", "tmpfile", lua_cloexec_file_call);
    wrap_library_function(state, "io", "popen", lua_cloexec_file_call);
    wrap_library_function(state, "os", "execute", lua_cloexec_process_call);
}

void report_start_status(int descriptor, int status) {
    const char* data = reinterpret_cast<const char*>(&status);
    size_t written = 0;
    while (written < sizeof(status)) {
        const ssize_t count = write(descriptor, data + written, sizeof(status) - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
}

void report_daemon_ready(int* descriptor, bool ready) {
    if (*descriptor < 0)
        return;
    report_start_status(*descriptor, ready ? 0 : -1);
    close(*descriptor);
    *descriptor = -1;
}

bool write_daemon_marker(int descriptor, bool ready) {
    const DaemonState state{kDaemonStateMagic, static_cast<int32_t>(getpid()),
                            static_cast<int32_t>(getpgrp()), static_cast<uint8_t>(ready)};
    const char* data = reinterpret_cast<const char*>(&state);
    size_t written = 0;
    while (written < sizeof(state)) {
        const ssize_t count = pwrite(descriptor, data + written, sizeof(state) - written,
                                     static_cast<off_t>(written));
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count >= 0)
            errno = EIO;
        return false;
    }
    return ftruncate(descriptor, sizeof(state)) == 0;
}

bool read_daemon_state(int descriptor, DaemonState* state) {
    char* data = reinterpret_cast<char*>(state);
    size_t received = 0;
    while (received < sizeof(*state)) {
        const ssize_t count = pread(descriptor, data + received, sizeof(*state) - received,
                                    static_cast<off_t>(received));
        if (count > 0) {
            received += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return state->magic == kDaemonStateMagic && state->pid > 0 && state->process_group > 0;
}

bool daemon_marker_is_ready(int descriptor) {
    DaemonState state;
    return read_daemon_state(descriptor, &state) && state.ready != 0;
}

void stop_spawned_daemon(pid_t launcher) {
    (void)kill(-launcher, SIGKILL);
    (void)kill(launcher, SIGKILL);
}

bool daemon_lock_belongs_to(const std::string& filename, const std::string& plugin_id) {
    constexpr std::string_view suffix = ".daemon.lock";
    if (filename.size() <= suffix.size() ||
        filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    const std::string_view identity(filename.data(), filename.size() - suffix.size());
    const size_t separator = identity.rfind('.');
    if (separator == std::string_view::npos || identity.substr(0, separator) != plugin_id)
        return false;
    const auto callback = identity.substr(separator + 1);
    return !callback.empty() && callback.size() <= kMaxCallbackBytes &&
           std::all_of(callback.begin(), callback.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_';
           });
}

std::string encode_lock_component(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const unsigned char c : value) {
        encoded.push_back(digits[c >> 4U]);
        encoded.push_back(digits[c & 15U]);
    }
    return encoded;
}

fs::path daemon_lock_directory(const std::string& id) {
    return fs::path(PLUGIN_LOCK_DIR) / "daemons" / encode_lock_component(id);
}

class DaemonLock {
public:
    explicit DaemonLock(const fs::path& path)
        : fd_(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        if (fd_ >= 0) {
            owned_ = flock(fd_, LOCK_EX | LOCK_NB) == 0;
            const int error = errno;
            ready_ =
                !owned_ && (error == EWOULDBLOCK || error == EAGAIN) && daemon_marker_is_ready(fd_);
        }
    }
    ~DaemonLock() {
        if (fd_ >= 0)
            close(fd_);
    }
    DaemonLock(const DaemonLock&) = delete;
    DaemonLock& operator=(const DaemonLock&) = delete;
    DaemonLock(DaemonLock&&) = delete;
    DaemonLock& operator=(DaemonLock&&) = delete;
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] bool owned() const { return owned_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    int fd_ = -1;
    bool owned_ = false;
    bool ready_ = false;
};

bool publish_daemon_state(int fd, int legacy_fd, bool ready) {
    return write_daemon_marker(fd, ready) &&
           (legacy_fd < 0 || write_daemon_marker(legacy_fd, ready));
}

bool wait_for_pidfd(int descriptor, int timeout_milliseconds) {
    pollfd event{descriptor, POLLIN, 0};
    int result;
    do {
        result = poll(&event, 1, timeout_milliseconds);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (event.revents & POLLIN) != 0;
}

bool pidfd_lifecycle_supported() {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int descriptor = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (descriptor < 0)
        return false;
    const bool supported = syscall(SYS_pidfd_send_signal, descriptor, 0, nullptr, 0) == 0;
    close(descriptor);
    return supported;
#else
    return false;
#endif
}

bool wait_for_process_stop(pid_t process, int pid_fd) {
    const std::string stat_path = "/proc/" + std::to_string(process) + "/stat";
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (wait_for_pidfd(pid_fd, 0))
            return false;
        const int stat_fd = open(stat_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (stat_fd >= 0) {
            std::array<char, 1024> buffer{};
            ssize_t count;
            do {
                count = read(stat_fd, buffer.data(), buffer.size() - 1);
            } while (count < 0 && errno == EINTR);
            close(stat_fd);
            if (count > 0) {
                const std::string_view stat(buffer.data(), static_cast<size_t>(count));
                const size_t command_end = stat.rfind(')');
                if (command_end != std::string_view::npos && command_end + 2 < stat.size() &&
                    (stat[command_end + 2] == 'T' || stat[command_end + 2] == 't')) {
                    return true;
                }
            }
        }
        (void)poll(nullptr, 0, 25);
    }
    return false;
}

bool stop_daemon_from_lock(const fs::path& path, std::string* error) {
    const int lock_fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lock_fd < 0) {
        if (errno == ENOENT)
            return true;
        *error = "Cannot open daemon lock '" + path.string() + "': " + strerror(errno);
        return false;
    }
    int lock_result = flock(lock_fd, LOCK_EX | LOCK_NB);
    if (lock_result == 0) {
        close(lock_fd);
        return true;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        *error = "Cannot inspect daemon lock '" + path.string() + "': " + strerror(errno);
        close(lock_fd);
        return false;
    }

    DaemonState state;
    if (!read_daemon_state(lock_fd, &state)) {
        *error = "Daemon lock has invalid owner metadata: " + path.string();
        close(lock_fd);
        return false;
    }
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int pid_fd = static_cast<int>(syscall(SYS_pidfd_open, state.pid, 0));
    if (pid_fd < 0) {
        *error = "Cannot open daemon pidfd: " + std::string(strerror(errno));
        close(lock_fd);
        return false;
    }
    DaemonState confirmed;
    if (!read_daemon_state(lock_fd, &confirmed) || confirmed.pid != state.pid ||
        confirmed.process_group != state.process_group ||
        getpgid(state.pid) != state.process_group) {
        close(pid_fd);
        close(lock_fd);
        *error = "Daemon owner changed while stopping it";
        return false;
    }
    if (getpgrp() == state.process_group) {
        close(pid_fd);
        close(lock_fd);
        *error = "A plugin daemon cannot stop itself through the plugin CLI";
        return false;
    }
    lock_result = flock(lock_fd, LOCK_EX | LOCK_NB);
    if (lock_result == 0) {
        close(pid_fd);
        close(lock_fd);
        return true;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        *error = "Cannot recheck daemon lock: " + std::string(strerror(errno));
        close(pid_fd);
        close(lock_fd);
        return false;
    }

    if (syscall(SYS_pidfd_send_signal, pid_fd, SIGSTOP, nullptr, 0) != 0 ||
        !wait_for_process_stop(state.pid, pid_fd)) {
        (void)syscall(SYS_pidfd_send_signal, pid_fd, SIGCONT, nullptr, 0);
        *error = "Cannot freeze plugin daemon before stopping it";
        close(pid_fd);
        close(lock_fd);
        return false;
    }
    (void)kill(-state.process_group, SIGTERM);
    (void)poll(nullptr, 0, 250);
    (void)kill(-state.process_group, SIGKILL);
    if (syscall(SYS_pidfd_send_signal, pid_fd, SIGKILL, nullptr, 0) != 0 && errno != ESRCH) {
        *error = "Cannot kill plugin daemon: " + std::string(strerror(errno));
        close(pid_fd);
        close(lock_fd);
        return false;
    }
    if (!wait_for_pidfd(pid_fd, 2000)) {
        *error = "Plugin daemon did not exit";
        close(pid_fd);
        close(lock_fd);
        return false;
    }
    close(pid_fd);
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) {
            close(lock_fd);
            return true;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN)
            break;
        (void)poll(nullptr, 0, 50);
    }
    *error = "Plugin daemon exited but its lock is still held";
#else
    *error = "This kernel cannot safely stop plugin daemons";
#endif
    close(lock_fd);
    return false;
}

void append_capped(std::string* output, const char* data, size_t size, bool* truncated) {
    if (output->size() >= kMaxCommandOutput) {
        *truncated = true;
        return;
    }
    const size_t available = kMaxCommandOutput - output->size();
    const size_t accepted = std::min(size, available);
    output->append(data, accepted);
    if (accepted != size)
        *truncated = true;
}

int drain_pipe(int* fd, std::string* output, bool* truncated) {
    std::array<char, 4096> buffer{};
    for (int batch = 0; batch < 32 && *fd >= 0; ++batch) {
        const ssize_t count = read(*fd, buffer.data(), buffer.size());
        if (count > 0) {
            append_capped(output, buffer.data(), static_cast<size_t>(count), truncated);
            continue;
        }
        if (count == 0) {
            close(*fd);
            *fd = -1;
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        const int error = errno;
        close(*fd);
        *fd = -1;
        return error;
    }
    return 0;
}

CapturedCommand run_command(const std::vector<std::string>& arguments);

bool set_prop_builtin(const std::string& name, const std::string& value) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 5> argv{
        const_cast<char*>("resetprop"),
        const_cast<char*>("-n"),
        const_cast<char*>(name.c_str()),
        const_cast<char*>(value.c_str()),
        nullptr,
    };
    return resetprop_main(4, argv.data()) == 0;
#else
    return run_command({RESETPROP_PATH, "-n", name, value}).exit_code == 0;
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
}

class CommandPipes {
public:
    CommandPipes() = default;
    CommandPipes(const CommandPipes&) = delete;
    CommandPipes& operator=(const CommandPipes&) = delete;
    CommandPipes(CommandPipes&&) = delete;
    CommandPipes& operator=(CommandPipes&&) = delete;
    std::array<int, 2>& output() { return output_; }
    std::array<int, 2>& error() { return error_; }
    std::array<int, 2>& status() { return status_; }

    static void close_fd(int& fd) {
        if (fd >= 0)
            close(fd);
        fd = -1;
    }

    ~CommandPipes() {
        for (auto* pipe : {&output_, &error_, &status_}) {
            for (auto& fd : *pipe)
                close_fd(fd);
        }
    }

private:
    std::array<int, 2> output_{-1, -1};
    std::array<int, 2> error_{-1, -1};
    std::array<int, 2> status_{-1, -1};
};

class CommandChild {
public:
    explicit CommandChild(pid_t pid) : pid_(pid) {}
    ~CommandChild() {
        if (pid_ > 0) {
            (void)kill(pid_, SIGKILL);
            int status = 0;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    CommandChild(const CommandChild&) = delete;
    CommandChild& operator=(const CommandChild&) = delete;
    CommandChild(CommandChild&&) = delete;
    CommandChild& operator=(CommandChild&&) = delete;

    pid_t wait(int* status) {
        pid_t result;
        do {
            result = waitpid(pid_, status, 0);
        } while (result < 0 && errno == EINTR);
        if (result > 0 || errno == ECHILD)
            pid_ = -1;
        return result;
    }

private:
    pid_t pid_;
};

CapturedCommand run_command(const std::vector<std::string>& arguments) {
    CapturedCommand result;
    const auto fail = [&](const char* operation, int error) {
        result.error = std::string(operation) + ": " + strerror(error);
        return result;
    };
    if (arguments.empty())
        return fail("missing program", EINVAL);

    CommandPipes pipes;
    if (!create_internal_pipe(pipes.output().data()) ||
        !create_internal_pipe(pipes.error().data()) || !create_internal_pipe(pipes.status().data()))
        return fail("create command pipe", errno);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0)
        return fail("fork command", errno);
    if (child == 0) {
        CommandPipes::close_fd(pipes.output()[0]);
        CommandPipes::close_fd(pipes.error()[0]);
        CommandPipes::close_fd(pipes.status()[0]);
        const auto reject = [&](int error) {
            report_start_status(pipes.status()[1], error);
            _exit(127);
        };
        const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (input < 0)
            reject(errno);
        if (dup2(input, STDIN_FILENO) < 0 || fcntl(STDIN_FILENO, F_SETFD, 0) < 0 ||
            dup2(pipes.output()[1], STDOUT_FILENO) < 0 || dup2(pipes.error()[1], STDERR_FILENO) < 0)
            reject(errno);
        if (input > STDERR_FILENO)
            close(input);
        CommandPipes::close_fd(pipes.output()[1]);
        CommandPipes::close_fd(pipes.error()[1]);
        mark_nonstandard_fds_cloexec();
        execvp(argv[0], argv.data());
        reject(errno);
    }

    CommandChild owned_child(child);
    CommandPipes::close_fd(pipes.output()[1]);
    CommandPipes::close_fd(pipes.error()[1]);
    CommandPipes::close_fd(pipes.status()[1]);
    int child_error = 0;
    size_t received = 0;
    while (received < sizeof(child_error)) {
        const ssize_t count =
            read(pipes.status()[0], reinterpret_cast<char*>(&child_error) + received,
                 sizeof(child_error) - received);
        if (count > 0)
            received += static_cast<size_t>(count);
        else if (count == 0)
            break;
        else if (errno != EINTR)
            return fail("read command exec status", errno);
    }
    CommandPipes::close_fd(pipes.status()[0]);
    if (received != 0)
        return fail("start command", received == sizeof(child_error) ? child_error : EIO);

    for (const int fd : {pipes.output()[0], pipes.error()[0]}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return fail("configure command output", errno);
    }
    while (pipes.output()[0] >= 0 || pipes.error()[0] >= 0) {
        std::array<pollfd, 2> descriptors{{
            {pipes.output()[0], static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
            {pipes.error()[0], static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
        }};
        int polled;
        do {
            polled = poll(descriptors.data(), descriptors.size(), -1);
        } while (polled < 0 && errno == EINTR);
        if (polled < 0)
            return fail("poll command output", errno);
        for (size_t index = 0; index < descriptors.size(); ++index) {
            if ((descriptors[index].revents & POLLNVAL) != 0)
                return fail("invalid command output descriptor", EBADF);
            if (descriptors[index].fd < 0 || descriptors[index].revents == 0)
                continue;
            const int error = index == 0
                                  ? drain_pipe(pipes.output().data(), &result.standard_output,
                                               &result.stdout_truncated)
                                  : drain_pipe(pipes.error().data(), &result.standard_error,
                                               &result.stderr_truncated);
            if (error != 0)
                return fail("read command output", error);
        }
    }
    int status = 0;
    if (owned_child.wait(&status) < 0)
        return fail("wait for command", errno);
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
    if (result.stdout_truncated)
        result.standard_output += "\n[output truncated]\n";
    if (result.stderr_truncated)
        result.standard_error += "\n[output truncated]\n";
    return result;
}

std::optional<std::string> safe_sysctl_path(std::string key) {
    if (key.empty() || key.front() == '/')
        return std::nullopt;
    std::replace(key.begin(), key.end(), '.', '/');
    size_t offset = 0;
    while (offset <= key.size()) {
        const size_t separator = key.find('/', offset);
        const size_t end = separator == std::string::npos ? key.size() : separator;
        const std::string_view component(key.data() + offset, end - offset);
        if (component.empty() || component == "." || component == ".." ||
            !std::all_of(component.begin(), component.end(), [](char character) {
                return (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '_' ||
                       character == '-';
            })) {
            return std::nullopt;
        }
        if (separator == std::string::npos)
            break;
        offset = separator + 1;
    }
    return "/proc/sys/" + key;
}

bool write_plugin_file(const fs::path& path, const std::string& content) {
    std::error_code error;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), error);
    if (error)
        return false;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = write(fd, content.data() + written, content.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    const bool success = written == content.size() && close(fd) == 0;
    if (!success && written != content.size())
        (void)close(fd);
    return success;
}

std::optional<std::string> read_plugin_file(const std::string& path, std::string* error) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = strerror(errno);
        return std::nullopt;
    }
    std::string output;
    std::array<char, 8192> buffer{};
    while (true) {
        ssize_t count;
        do {
            count = read(descriptor, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count == 0)
            break;
        if (count < 0) {
            *error = strerror(errno);
            close(descriptor);
            return std::nullopt;
        }
        if (static_cast<size_t>(count) > kMaxFileInput - output.size()) {
            *error = "file exceeds the size limit";
            close(descriptor);
            return std::nullopt;
        }
        output.append(buffer.data(), static_cast<size_t>(count));
    }
    if (close(descriptor) != 0) {
        *error = strerror(errno);
        return std::nullopt;
    }
    return output;
}

size_t escaped_json_string_size(std::string_view value) {
    size_t size = 2;
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\' || character == '\b' || character == '\f' ||
            character == '\n' || character == '\r' || character == '\t') {
            size += 2;
        } else if (character < 0x20U) {
            size += 6;
        } else {
            ++size;
        }
    }
    return size;
}

bool account_json_output(JsonEncodeContext* context, size_t bytes) {
    if (bytes > kMaxJsonInput - context->output_bytes) {
        context->error = "JSON output exceeds the size limit";
        return false;
    }
    context->output_bytes += bytes;
    return true;
}

bool account_json_string(JsonEncodeContext* context, std::string_view value) {
    if (value.size() > kMaxJsonInput) {
        context->error = "JSON output exceeds the size limit";
        return false;
    }
    return account_json_output(context, escaped_json_string_size(value));
}

std::optional<plugin_json::Value> lua_to_json(lua_State* state, int index, size_t depth,
                                              JsonEncodeContext* context) {
    if (depth > kMaxJsonDepth || ++context->nodes > kMaxJsonNodes) {
        context->error = "JSON value exceeds the depth or node limit";
        return std::nullopt;
    }
    if (!account_json_output(context, 32))
        return std::nullopt;
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        return plugin_json::Value();
    case LUA_TBOOLEAN:
        return plugin_json::Value(lua_toboolean(state, index) != 0);
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
            return plugin_json::Value(static_cast<int64_t>(lua_tointeger(state, index)));
        if (!std::isfinite(lua_tonumber(state, index))) {
            context->error = "Cannot encode a non-finite JSON number";
            return std::nullopt;
        }
        return plugin_json::Value(lua_tonumber(state, index));
    case LUA_TSTRING: {
        size_t size = 0;
        const char* value = lua_tolstring(state, index, &size);
        if (!account_json_string(context, {value, size})) {
            return std::nullopt;
        }
        return plugin_json::Value(std::string(value, size));
    }
    case LUA_TTABLE: {
        const void* identity = lua_topointer(state, index);
        if (!context->tables.insert(identity).second) {
            context->error = "Cannot encode a cyclic Lua table";
            return std::nullopt;
        }

        bool array = true;
        size_t entries = 0;
        lua_Integer maximum = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            if (++entries > kMaxJsonNodes) {
                lua_pop(state, 2);
                context->tables.erase(identity);
                context->error = "JSON table has too many entries";
                return std::nullopt;
            }
            if (!lua_isinteger(state, -2)) {
                array = false;
            } else {
                const lua_Integer key = lua_tointeger(state, -2);
                if (key <= 0)
                    array = false;
                else
                    maximum = std::max(maximum, key);
            }
            lua_pop(state, 1);
        }
        array = array && entries > 0 && maximum == static_cast<lua_Integer>(entries);

        if (array) {
            plugin_json::Array output;
            output.reserve(entries);
            for (size_t item = 1; item <= entries; ++item) {
                lua_geti(state, index, static_cast<lua_Integer>(item));
                auto value = lua_to_json(state, -1, depth + 1, context);
                lua_pop(state, 1);
                if (!value) {
                    context->tables.erase(identity);
                    return std::nullopt;
                }
                output.push_back(std::move(*value));
            }
            context->tables.erase(identity);
            return plugin_json::Value(std::move(output));
        }

        plugin_json::Object output;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            std::string key;
            if (lua_type(state, -2) == LUA_TSTRING) {
                size_t size = 0;
                const char* text = lua_tolstring(state, -2, &size);
                if (!account_json_string(context, {text, size})) {
                    lua_pop(state, 2);
                    context->tables.erase(identity);
                    return std::nullopt;
                }
                key.assign(text, size);
            } else if (lua_isinteger(state, -2)) {
                key = std::to_string(lua_tointeger(state, -2));
                if (!account_json_string(context, key)) {
                    lua_pop(state, 2);
                    context->tables.erase(identity);
                    return std::nullopt;
                }
            } else {
                lua_pop(state, 2);
                context->tables.erase(identity);
                context->error = "JSON object keys must be strings or integers";
                return std::nullopt;
            }
            auto value = lua_to_json(state, -1, depth + 1, context);
            if (!value) {
                lua_pop(state, 2);
                context->tables.erase(identity);
                return std::nullopt;
            }
            lua_pop(state, 1);
            if (!output.emplace(std::move(key), std::move(*value)).second) {
                lua_pop(state, 1);
                context->tables.erase(identity);
                context->error = "JSON object keys collide after normalization";
                return std::nullopt;
            }
        }
        context->tables.erase(identity);
        return plugin_json::Value(std::move(output));
    }
    default:
        break;
    }
    context->error = "Unsupported Lua value for JSON encoding";
    return std::nullopt;
}

void json_to_lua(lua_State* state, const plugin_json::Value& value) {
    switch (value.type) {
    case plugin_json::Type::Null:
        lua_pushnil(state);
        break;
    case plugin_json::Type::Bool:
        lua_pushboolean(state, value.b);
        break;
    case plugin_json::Type::Integer:
        lua_pushinteger(state, static_cast<lua_Integer>(value.i));
        break;
    case plugin_json::Type::Number:
        lua_pushnumber(state, value.n);
        break;
    case plugin_json::Type::String:
        lua_pushlstring(state, value.s.data(), value.s.size());
        break;
    case plugin_json::Type::Array:
        lua_createtable(state, static_cast<int>(value.a.size()), 0);
        for (size_t index = 0; index < value.a.size(); ++index) {
            json_to_lua(state, value.a[index]);
            lua_seti(state, -2, static_cast<lua_Integer>(index) + 1);
        }
        break;
    case plugin_json::Type::Object:
        lua_createtable(state, 0, static_cast<int>(value.o.size()));
        for (const auto& [key, item] : value.o) {
            lua_pushlstring(state, key.data(), key.size());
            json_to_lua(state, item);
            lua_settable(state, -3);
        }
        break;
    }
}

bool spawn_daemon_process(PluginApiContext* context, const std::string& callback,
                          int64_t interval_seconds) {
    if (!plugin_callback_is_valid(callback) || interval_seconds < 1)
        return false;
    if (!pidfd_lifecycle_supported()) {
        log_line(context, "W", "plugin daemons require pidfd support");
        return false;
    }

    const std::string interval = std::to_string(interval_seconds);
    int status_pipe[2] = {-1, -1};
    if (!create_internal_pipe(status_pipe))
        return false;
    const std::string ready_descriptor = std::to_string(status_pipe[1]);

    const pid_t launcher = fork();
    if (launcher < 0) {
        close(status_pipe[0]);
        close(status_pipe[1]);
        return false;
    }
    if (launcher == 0) {
        close(status_pipe[0]);
        if (setsid() < 0) {
            report_start_status(status_pipe[1], errno);
            _exit(1);
        }
        const pid_t daemon = fork();
        if (daemon < 0) {
            report_start_status(status_pipe[1], errno);
            _exit(1);
        }
        if (daemon > 0)
            _exit(0);
        if (!redirect_standard_streams()) {
            report_start_status(status_pipe[1], errno);
            _exit(1);
        }
        mark_nonstandard_fds_cloexec();
        const int descriptor_flags = fcntl(status_pipe[1], F_GETFD);
        if (descriptor_flags < 0 ||
            fcntl(status_pipe[1], F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0) {
            report_start_status(status_pipe[1], errno);
            _exit(1);
        }
        execl(DAEMON_PATH, "ksud", "plugin", "daemon", "--ready-fd", ready_descriptor.c_str(), "--",
              context->id.c_str(), callback.c_str(), interval.c_str(), nullptr);
        report_start_status(status_pipe[1], errno);
        _exit(127);
    }

    close(status_pipe[1]);
    pollfd status_event{status_pipe[0], POLLIN | POLLHUP, 0};
    int poll_result = 0;
    do {
        poll_result = poll(&status_event, 1, kDaemonReadyTimeoutMilliseconds);
    } while (poll_result < 0 && errno == EINTR);

    int start_status = -1;
    size_t received = 0;
    if (poll_result > 0) {
        while (received < sizeof(start_status)) {
            const ssize_t count =
                read(status_pipe[0], reinterpret_cast<char*>(&start_status) + received,
                     sizeof(start_status) - received);
            if (count > 0) {
                received += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
    } else {
        stop_spawned_daemon(launcher);
    }
    const bool start_rejected =
        poll_result > 0 && (received != sizeof(start_status) || start_status != 0);
    if (start_rejected)
        stop_spawned_daemon(launcher);
    close(status_pipe[0]);
    int launcher_status = 0;
    pid_t waited;
    do {
        waited = waitpid(launcher, &launcher_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (poll_result == 0) {
        log_line(context, "W", "daemon start timed out");
        return false;
    }
    if (poll_result < 0 || received != sizeof(start_status) || waited < 0 ||
        !WIFEXITED(launcher_status) || WEXITSTATUS(launcher_status) != 0) {
        log_line(context, "W", "cannot confirm daemon start");
        return false;
    }
    if (start_status > 0) {
        log_line(context, "W", "cannot start daemon: " + std::string(strerror(start_status)));
        return false;
    }
    if (start_status < 0) {
        log_line(context, "W", "daemon rejected callback '" + callback + "'");
        return false;
    }
    log_line(context, "I", "daemon started: " + callback + " (" + interval + "s)");
    return true;
}

int lua_api_info(lua_State* state) {
    if (!require_string(state, 1))
        return luaL_error(state, "info expects a string");
    size_t size = 0;
    const char* message = lua_tolstring(state, 1, &size);
    log_line(api_context(state), "I", std::string(message, std::min(size, kMaxLogMessage)));
    return 0;
}

int lua_api_warn(lua_State* state) {
    if (!require_string(state, 1))
        return luaL_error(state, "warn expects a string");
    size_t size = 0;
    const char* message = lua_tolstring(state, 1, &size);
    log_line(api_context(state), "W", std::string(message, std::min(size, kMaxLogMessage)));
    return 0;
}

int lua_api_getprop(lua_State* state) {
    std::string name;
    if (!copy_lua_string(state, 1, kMaxPropertyNameBytes, &name))
        return luaL_error(state, "getprop expects a valid property name");
    const auto value = getprop(name);
    const std::string output = value.value_or(std::string());
    lua_pushlstring(state, output.data(), output.size());
    return 1;
}

int lua_api_setprop(lua_State* state) {
    if (!require_string(state, 1) || !require_string(state, 2))
        return luaL_error(state, "setprop expects name and value strings");
    size_t name_size = 0;
    size_t value_size = 0;
    const char* name = lua_tolstring(state, 1, &name_size);
    const char* value = lua_tolstring(state, 2, &value_size);
    if (std::memchr(name, '\0', name_size) != nullptr ||
        std::memchr(value, '\0', value_size) != nullptr ||
        name_size + value_size > kMaxCommandArgumentBytes) {
        return luaL_error(state, "setprop arguments are invalid or too large");
    }
    lua_pushboolean(state,
                    set_prop_builtin(std::string(name, name_size), std::string(value, value_size)));
    return 1;
}

int lua_api_exec(lua_State* state) {
    const int count = lua_gettop(state);
    if (count < 1 || count > static_cast<int>(kMaxCommandArguments))
        return luaL_error(state, "exec expects at least one string argument");
    for (int index = 1; index <= count; ++index) {
        if (!require_string(state, index))
            return luaL_error(state, "exec arguments must be strings");
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(count));
    size_t argument_bytes = 0;
    for (int index = 1; index <= count; ++index) {
        size_t size = 0;
        const char* argument = lua_tolstring(state, index, &size);
        if (std::memchr(argument, '\0', size) != nullptr ||
            size > kMaxCommandArgumentBytes - argument_bytes) {
            return luaL_error(state, "exec arguments contain NUL or exceed the size limit");
        }
        argument_bytes += size;
        arguments.emplace_back(argument, size);
    }
    const CapturedCommand result = run_command(arguments);
    if (!result.error.empty())
        return luaL_error(state, "exec failed: %s", result.error.c_str());
    lua_createtable(state, 0, 4);
    lua_pushboolean(state, result.exit_code == 0);
    lua_setfield(state, -2, "ok");
    lua_pushinteger(state, result.exit_code);
    lua_setfield(state, -2, "code");
    lua_pushlstring(state, result.standard_output.data(), result.standard_output.size());
    lua_setfield(state, -2, "stdout");
    lua_pushlstring(state, result.standard_error.data(), result.standard_error.size());
    lua_setfield(state, -2, "stderr");
    return 1;
}

int lua_api_read_file(lua_State* state) {
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "read_file expects a valid path string");
    std::string error;
    const auto content = read_plugin_file(path, &error);
    if (!content)
        return luaL_error(state, "read_file failed: %s", error.c_str());
    lua_pushlstring(state, content->data(), content->size());
    return 1;
}

int lua_api_write_file(lua_State* state) {
    if (!require_string(state, 1) || !require_string(state, 2))
        return luaL_error(state, "write_file expects path and content strings");
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "write_file expects a valid path string");
    size_t size = 0;
    const char* content = lua_tolstring(state, 2, &size);
    if (size > kMaxFileInput)
        return luaL_error(state, "write_file content exceeds the size limit");
    const bool success = write_plugin_file(path, std::string(content, size));
    lua_pushboolean(state, success);
    return 1;
}

int lua_api_sysctl(lua_State* state) {
    if (!require_string(state, 1) || !require_string(state, 2))
        return luaL_error(state, "sysctl expects key and value strings");
    std::string key;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &key))
        return luaL_error(state, "sysctl expects a valid key string");
    const auto path = safe_sysctl_path(key);
    bool success = false;
    if (path) {
        size_t size = 0;
        const char* value = lua_tolstring(state, 2, &size);
        const int fd = open(path->c_str(), O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            size_t written = 0;
            while (written < size) {
                const ssize_t count = write(fd, value + written, size - written);
                if (count > 0) {
                    written += static_cast<size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                break;
            }
            success = written == size && close(fd) == 0;
            if (!success && written != size)
                (void)close(fd);
        }
    }
    lua_pushboolean(state, success);
    return 1;
}

int lua_api_chmod(lua_State* state) {
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "chmod expects a valid path string");
    mode_t mode = 0;
    if (lua_isinteger(state, 2)) {
        const lua_Integer integer = lua_tointeger(state, 2);
        if (integer < 0 || integer > 07777)
            return luaL_error(state, "chmod numeric mode is out of range");
        mode = static_cast<mode_t>(integer);
    } else if (lua_type(state, 2) == LUA_TSTRING) {
        std::string text;
        if (!copy_lua_string(state, 2, kMaxPathBytes, &text))
            return luaL_error(state, "chmod mode must be an octal string");
        const auto first = text.find_first_not_of(" \t\r\n\f\v");
        const auto last = text.find_last_not_of(" \t\r\n\f\v");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        if (text.empty() || text.size() > 16 ||
            !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '7'; }))
            return luaL_error(state, "chmod mode must be an octal string");
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, 8);
        if (errno != 0 || end == text.c_str() || *end != '\0' || parsed > 07777)
            return luaL_error(state, "chmod mode must be an octal string");
        mode = static_cast<mode_t>(parsed);
    } else {
        return luaL_error(state, "chmod mode must be an integer or octal string");
    }
    lua_pushboolean(state, chmod(path.c_str(), mode) == 0);
    return 1;
}

int lua_api_mkdir(lua_State* state) {
    std::string path_string;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path_string))
        return luaL_error(state, "mkdir expects a valid path string");
    std::error_code error;
    const fs::path path(path_string);
    if (lua_toboolean(state, 2) != 0)
        fs::create_directories(path, error);
    else if (!fs::create_directory(path, error) && !error && !fs::is_directory(path))
        error = std::make_error_code(std::errc::file_exists);
    lua_pushboolean(state, !error);
    return 1;
}

int lua_api_rm(lua_State* state) {
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "rm expects a valid path string");
    std::error_code error;
    const bool removed = fs::remove(path, error);
    lua_pushboolean(state, removed && !error);
    return 1;
}

int lua_api_list_dir(lua_State* state) {
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "list_dir expects a valid path string");
    std::error_code error;
    const fs::directory_iterator iterator(path, error);
    if (error)
        return luaL_error(state, "list_dir failed: %s", error.message().c_str());
    std::vector<std::string> entries;
    size_t entry_bytes = 0;
    for (fs::directory_iterator current = iterator, end; current != end && !error;
         current.increment(error)) {
        std::string entry = current->path().filename().string();
        if (entries.size() >= kMaxDirectoryEntries ||
            entry.size() > kMaxDirectoryEntryBytes - entry_bytes) {
            return luaL_error(state, "list_dir result exceeds the size limit");
        }
        entry_bytes += entry.size();
        entries.push_back(std::move(entry));
    }
    if (error)
        return luaL_error(state, "list_dir failed: %s", error.message().c_str());
    std::sort(entries.begin(), entries.end());
    lua_createtable(state, static_cast<int>(entries.size()), 0);
    for (size_t index = 0; index < entries.size(); ++index) {
        lua_pushlstring(state, entries[index].data(), entries[index].size());
        lua_seti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

int lua_api_file_exists(lua_State* state) {
    std::string path;
    if (!copy_lua_string(state, 1, kMaxPathBytes, &path))
        return luaL_error(state, "file_exists expects a valid path string");
    std::error_code error;
    const bool exists = fs::exists(path, error);
    lua_pushboolean(state, exists && !error);
    return 1;
}

int lua_api_get_config(lua_State* state) {
    if (!require_string(state, 1))
        return luaL_error(state, "get_config expects a key string");
    size_t key_size = 0;
    const char* key = lua_tolstring(state, 1, &key_size);
    if (key_size > 64)
        return luaL_error(state, "get_config key exceeds the size limit");
    std::string value;
    std::string error;
    if (!plugin_get_config_value(api_context(state)->id, std::string(key, key_size), &value,
                                 &error))
        return luaL_error(state, "get_config failed: %s", error.c_str());
    lua_pushlstring(state, value.data(), value.size());
    return 1;
}

int lua_api_set_config(lua_State* state) {
    if (!require_string(state, 1) || !require_string(state, 2))
        return luaL_error(state, "set_config expects key and value strings");
    size_t key_size = 0;
    size_t value_size = 0;
    const char* key = lua_tolstring(state, 1, &key_size);
    const char* value = lua_tolstring(state, 2, &value_size);
    if (key_size > 64 || value_size > kMaxJsonInput)
        return luaL_error(state, "set_config key or value exceeds the size limit");
    if (!plugin_config_key_is_valid(std::string(key, key_size)))
        return luaL_error(state, "set_config key is invalid");
    if (!plugin_json::is_valid_utf8(std::string(value, value_size)))
        return luaL_error(state, "set_config value must be valid UTF-8");
    std::string error;
    const bool success = plugin_set_config_value(api_context(state)->id, std::string(key, key_size),
                                                 std::string(value, value_size), &error);
    if (!success)
        log_line(api_context(state), "W", "set_config failed: " + error);
    lua_pushboolean(state, success);
    return 1;
}

int lua_api_json_decode(lua_State* state) {
    if (!require_string(state, 1))
        return luaL_error(state, "json_decode expects a string");
    size_t size = 0;
    const char* text = lua_tolstring(state, 1, &size);
    if (size > kMaxJsonInput)
        return luaL_error(state, "json_decode input exceeds the size limit");
    try {
        std::string error;
        const auto value = plugin_json::parse(std::string(text, size), &error);
        if (!value)
            return luaL_error(state, "json_decode failed: %s", error.c_str());
        json_to_lua(state, *value);
        return 1;
    } catch (const std::exception& exception) {
        return luaL_error(state, "json_decode failed: %s", exception.what());
    }
}

int lua_api_json_encode(lua_State* state) {
    JsonEncodeContext context;
    auto value = lua_to_json(state, 1, 0, &context);
    if (!value)
        return luaL_error(state, "json_encode failed: %s", context.error.c_str());
    if (!plugin_json::has_valid_utf8(*value))
        return luaL_error(state, "json_encode failed: strings must be valid UTF-8");
    const std::string output = plugin_json::dump(*value);
    if (output.size() > kMaxJsonInput)
        return luaL_error(state, "json_encode output exceeds the size limit");
    lua_pushlstring(state, output.data(), output.size());
    return 1;
}

int lua_api_start_daemon(lua_State* state) {
    int converted = 0;
    const lua_Integer integer = lua_tointegerx(state, 2, &converted);
    int64_t interval = 0;
    if (converted) {
        interval = static_cast<int64_t>(integer);
    } else {
        const lua_Number number = lua_tonumberx(state, 2, &converted);
        if (!converted || !std::isfinite(number) || number < -0x1p63 || number >= 0x1p63)
            return luaL_error(state, "start_daemon interval is out of range");
        interval = static_cast<int64_t>(number);
    }
    interval = std::max<int64_t>(1, interval);
    std::string callback;
    if (!copy_lua_string(state, 1, kMaxCallbackBytes, &callback) ||
        !plugin_callback_is_valid(callback))
        return luaL_error(state, "start_daemon callback is invalid or too long");
    const bool success = spawn_daemon_process(api_context(state), callback, interval);
    lua_pushboolean(state, success);
    return 1;
}

void register_closure(lua_State* state, PluginApiContext* context, const char* name,
                      lua_CFunction function) {
    lua_pushlightuserdata(state, context);
    lua_pushcclosure(state, function, 1);
    lua_setglobal(state, name);
}

template <int (*Function)(lua_State*)>
int guarded_lua_api(lua_State* state) {
    try {
        return Function(state);
    } catch (const std::exception& error) {
        return luaL_error(state, "plugin API failed: %s", error.what());
    }
}

void register_api(lua_State* state, PluginApiContext* context) {
    register_closure(state, context, "info", guarded_lua_api<lua_api_info>);
    register_closure(state, context, "warn", guarded_lua_api<lua_api_warn>);
    register_closure(state, context, "getprop", guarded_lua_api<lua_api_getprop>);
    register_closure(state, context, "setprop", guarded_lua_api<lua_api_setprop>);
    register_closure(state, context, "exec", guarded_lua_api<lua_api_exec>);
    register_closure(state, context, "read_file", guarded_lua_api<lua_api_read_file>);
    register_closure(state, context, "write_file", guarded_lua_api<lua_api_write_file>);
    register_closure(state, context, "sysctl", guarded_lua_api<lua_api_sysctl>);
    register_closure(state, context, "chmod", guarded_lua_api<lua_api_chmod>);
    register_closure(state, context, "mkdir", guarded_lua_api<lua_api_mkdir>);
    register_closure(state, context, "rm", guarded_lua_api<lua_api_rm>);
    register_closure(state, context, "list_dir", guarded_lua_api<lua_api_list_dir>);
    register_closure(state, context, "file_exists", guarded_lua_api<lua_api_file_exists>);
    register_closure(state, context, "get_config", guarded_lua_api<lua_api_get_config>);
    register_closure(state, context, "set_config", guarded_lua_api<lua_api_set_config>);
    register_closure(state, context, "json_decode", guarded_lua_api<lua_api_json_decode>);
    register_closure(state, context, "json_encode", guarded_lua_api<lua_api_json_encode>);
    register_closure(state, context, "start_daemon", guarded_lua_api<lua_api_start_daemon>);
}

void prepend_package_path(lua_State* state, const std::string& directory) {
    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    const int package = lua_absindex(state, -1);
    lua_getfield(state, package, "path");
    const char* old_path = lua_tostring(state, -1);
    const std::string path =
        directory + "/?.lua;" + directory + "/?/init.lua;" + (old_path ? old_path : "");
    lua_pop(state, 1);
    lua_pushlstring(state, path.data(), path.size());
    lua_setfield(state, package, "path");

    lua_getfield(state, package, "cpath");
    const char* old_cpath = lua_tostring(state, -1);
    const std::string cpath = directory + "/?.so;" + (old_cpath ? old_cpath : "");
    lua_pop(state, 1);
    lua_pushlstring(state, cpath.data(), cpath.size());
    lua_setfield(state, package, "cpath");
    lua_pop(state, 1);
}

std::shared_ptr<PluginCodeSnapshot> open_plugin_snapshot(const std::string& directory,
                                                         std::string* error) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int directory_fd =
            open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_fd < 0) {
            *error = "Cannot open plugin directory: " + std::string(strerror(errno));
            return nullptr;
        }

        struct stat directory_status{};
        if (fstat(directory_fd, &directory_status) != 0 || !S_ISDIR(directory_status.st_mode)) {
            *error = "Plugin directory has an invalid type";
            close(directory_fd);
            return nullptr;
        }
        int lock_result;
        do {
            lock_result = flock(directory_fd, LOCK_SH);
        } while (lock_result != 0 && errno == EINTR);
        if (lock_result != 0) {
            *error = "Cannot lock plugin runtime: " + std::string(strerror(errno));
            close(directory_fd);
            return nullptr;
        }

        const int current_fd =
            open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat current_status{};
        const bool current_matches = current_fd >= 0 && fstat(current_fd, &current_status) == 0 &&
                                     current_status.st_dev == directory_status.st_dev &&
                                     current_status.st_ino == directory_status.st_ino;
        if (current_fd >= 0)
            close(current_fd);
        if (current_matches) {
            return std::make_shared<PluginCodeSnapshot>(directory_fd);
        }
        close(directory_fd);
    }
    *error = "Plugin changed repeatedly while acquiring a runtime snapshot";
    return nullptr;
}

std::optional<PluginRecord> load_plugin_record(const std::string& id, bool require_enabled,
                                               std::string* error) {
    if (!plugin_id_is_valid(id)) {
        *error = "Invalid plugin id";
        return std::nullopt;
    }
    PluginRecord record;
    record.id = id;
    record.directory = std::string(PLUGIN_DIR) + id;
    std::error_code directory_error;
    if (!fs::is_directory(record.directory, directory_error) || directory_error) {
        *error = "Plugin is not installed";
        return std::nullopt;
    }
    record.code_snapshot = open_plugin_snapshot(record.directory, error);
    if (!record.code_snapshot)
        return std::nullopt;
    std::error_code disable_error;
    record.enabled =
        !fs::exists(fs::path(record.code_snapshot->directory()) / DISABLE_FILE_NAME, disable_error);
    if (disable_error) {
        *error = "Cannot read plugin state: " + disable_error.message();
        return std::nullopt;
    }
    if (require_enabled && !record.enabled) {
        *error = "Plugin is disabled";
        return std::nullopt;
    }
    auto manifest = plugin_read_manifest(record.code_snapshot->directory(), id, error);
    if (!manifest)
        return std::nullopt;
    if (!plugin_dependencies_available(*manifest, error))
        return std::nullopt;
    record.manifest = std::move(*manifest);
    record.manifest_valid = true;
    return record;
}

PluginRunResult call_plugin(const PluginRecord& plugin, const std::string& callback,
                            bool callback_optional, bool auto_start_main, int ready_fd = -1,
                            bool* callback_ready = nullptr, int ready_marker_fd = -1,
                            int legacy_marker_fd = -1) {
    if (callback_ready)
        *callback_ready = false;
    const std::string& code_directory =
        plugin.code_snapshot ? plugin.code_snapshot->directory() : plugin.directory;
    PluginApiContext context{plugin.id, plugin.directory};
    LuaMemoryContext memory;
    const ScopedLuaState state(
        lua_newstate(limited_lua_allocator, &memory, luaL_makeseed(nullptr)));
    if (!state) {
        report_daemon_ready(&ready_fd, false);
        plugin_append_log(plugin.directory, "W", "cannot allocate a Lua state");
        return PluginRunResult::Failed;
    }
    luaL_openlibs(state.get());
    protect_library_descriptors(state.get());

    register_api(state.get(), &context);
    lua_pushlstring(state.get(), plugin.id.data(), plugin.id.size());
    lua_setglobal(state.get(), "PLUGIN_ID");
    lua_pushlstring(state.get(), plugin.directory.data(), plugin.directory.size());
    lua_setglobal(state.get(), "PLUGIN_DIR");
    lua_pushlstring(state.get(), code_directory.data(), code_directory.size());
    lua_setglobal(state.get(), "PLUGIN_CODE_DIR");
    prepend_package_path(state.get(), code_directory);

    lua_pushcfunction(state.get(), traceback_handler);
    const int load_handler = lua_gettop(state.get());
    const fs::path entry = fs::path(code_directory) / plugin.manifest.entry;
    int status = luaL_loadfilex(state.get(), entry.c_str(), "t");
    if (status == LUA_OK)
        status = lua_pcall(state.get(), 0, 1, load_handler);
    if (status != LUA_OK) {
        report_daemon_ready(&ready_fd, false);
        log_line(&context, "W",
                 "entry load failed: " + bounded_lua_message(state.get(), -1, "unknown"));
        return PluginRunResult::Failed;
    }
    lua_remove(state.get(), load_handler);
    if (!lua_istable(state.get(), -1)) {
        report_daemon_ready(&ready_fd, false);
        log_line(&context, "W", "entry script must return a table");
        return PluginRunResult::Failed;
    }
    const int plugin_table = lua_absindex(state.get(), -1);

    PluginRunResult result = PluginRunResult::Success;
    std::string lookup_error;
    const bool callback_found =
        push_table_value(state.get(), plugin_table, callback, &lookup_error);
    if (!callback_found) {
        report_daemon_ready(&ready_fd, false);
        lua_pop(state.get(), 1);
        log_line(&context, "W", "callback lookup failed: " + lookup_error);
        result = PluginRunResult::Failed;
    } else if (lua_isnil(state.get(), -1)) {
        report_daemon_ready(&ready_fd, false);
        lua_pop(state.get(), 1);
        result = PluginRunResult::MissingCallback;
    } else if (!lua_isfunction(state.get(), -1)) {
        report_daemon_ready(&ready_fd, false);
        lua_pop(state.get(), 1);
        log_line(&context, "W", "callback '" + callback + "' is not a function");
        result = PluginRunResult::Failed;
    } else {
        if (ready_marker_fd >= 0 &&
            !publish_daemon_state(ready_marker_fd, legacy_marker_fd, true)) {
            report_daemon_ready(&ready_fd, false);
            lua_pop(state.get(), 1);
            log_line(&context, "W", "cannot publish daemon readiness");
            return PluginRunResult::Failed;
        }
        if (callback_ready)
            *callback_ready = true;
        report_daemon_ready(&ready_fd, true);
        lua_pushcfunction(state.get(), traceback_handler);
        lua_insert(state.get(), -2);
        const int callback_handler = lua_gettop(state.get()) - 1;
        status = lua_pcall(state.get(), 0, 0, callback_handler);
        if (status != LUA_OK) {
            log_line(&context, "W",
                     "callback '" + callback +
                         "' failed: " + bounded_lua_message(state.get(), -1, "unknown"));
            lua_pop(state.get(), 1);
            lua_remove(state.get(), callback_handler);
            result = PluginRunResult::Failed;
        } else {
            lua_remove(state.get(), callback_handler);
        }
    }

    if (auto_start_main) {
        lookup_error.clear();
        const bool main_found = push_table_value(state.get(), plugin_table, "main", &lookup_error);
        if (!main_found) {
            log_line(&context, "W", "main callback lookup failed: " + lookup_error);
            result = PluginRunResult::Failed;
        } else if (lua_isfunction(state.get(), -1) && !spawn_daemon_process(&context, "main", 1)) {
            result = PluginRunResult::Failed;
        }
        lua_pop(state.get(), 1);
    }
    if (result == PluginRunResult::MissingCallback && callback_optional)
        return PluginRunResult::Success;
    return result;
}

int result_exit_code(PluginRunResult result) {
    if (result == PluginRunResult::Success)
        return 0;
    if (result == PluginRunResult::MissingCallback)
        return 2;
    return 1;
}

PluginRunResult exit_code_result(int code) {
    if (code == 0)
        return PluginRunResult::Success;
    if (code == 2)
        return PluginRunResult::MissingCallback;
    return PluginRunResult::Failed;
}

PluginRunResult run_blocking_worker(const PluginRecord& plugin, const std::string& callback,
                                    bool optional, bool auto_start_main, int timeout_seconds) {
    const pid_t child = fork();
    if (child < 0)
        return PluginRunResult::Failed;
    if (child == 0) {
        (void)setpgid(0, 0);
        switch_cgroups();
        const PluginRunResult result = call_plugin(plugin, callback, optional, auto_start_main);
        (void)std::fflush(nullptr);
        _exit(result_exit_code(result));
    }
    (void)setpgid(child, child);
    int status = 0;
    pid_t waited = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (true) {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno == EINTR)
            continue;
        if (waited < 0)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(-child, SIGKILL);
            (void)kill(child, SIGKILL);
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            plugin_append_log(plugin.directory, "W",
                              "callback '" + callback + "' exceeded its time limit");
            return PluginRunResult::Failed;
        }
        (void)poll(nullptr, 0, 100);
    }
    if (waited < 0 || !WIFEXITED(status))
        return PluginRunResult::Failed;
    return exit_code_result(WEXITSTATUS(status));
}

bool run_detached_worker(const PluginRecord& plugin, const std::string& callback,
                         bool auto_start_main) {
    const pid_t launcher = fork();
    if (launcher < 0)
        return false;
    if (launcher == 0) {
        if (setsid() < 0)
            _exit(1);
        switch_cgroups();
        if (!redirect_standard_streams())
            _exit(1);
        const pid_t worker = fork();
        if (worker < 0)
            _exit(1);
        if (worker > 0)
            _exit(0);
        const PluginRunResult result = run_blocking_worker(plugin, callback, true, auto_start_main,
                                                           kStageCallbackTimeoutSeconds);
        _exit(result_exit_code(result));
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(launcher, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

PluginRunResult run_plugin_callback_isolated(const std::string& plugin_id,
                                             const std::string& callback) {
    if (!plugin_callback_is_valid(callback))
        return PluginRunResult::Failed;
    std::string error;
    const auto plugin = load_plugin_record(plugin_id, true, &error);
    if (!plugin) {
        if (plugin_id_is_valid(plugin_id))
            plugin_append_log(std::string(PLUGIN_DIR) + plugin_id, "W", error);
        print_error("%s\n", error.c_str());
        return PluginRunResult::Failed;
    }
    return run_blocking_worker(*plugin, callback, false, false, kManualCallbackTimeoutSeconds);
}

bool exec_plugin_stage(const std::string& stage, bool block) {
    std::string callback = stage;
    std::replace(callback.begin(), callback.end(), '-', '_');
    if (!plugin_callback_is_valid(callback)) {
        LOGW("Invalid plugin stage callback: %s", callback.c_str());
        return false;
    }

    std::vector<std::string> errors;
    const auto plugins = plugin_resolve_enabled(&errors);
    for (const auto& error : errors)
        LOGW("plugin discovery: %s", error.c_str());

    bool success = true;
    for (const auto& discovered_plugin : plugins) {
        std::string load_error;
        const auto loaded_plugin = load_plugin_record(discovered_plugin.id, true, &load_error);
        if (!loaded_plugin) {
            LOGW("plugin %s stage %s skipped: %s", discovered_plugin.id.c_str(), callback.c_str(),
                 load_error.c_str());
            success = false;
            continue;
        }
        const auto& plugin = *loaded_plugin;
        const bool auto_start_main =
            callback == "service" &&
            (!plugin.manifest.callbacks ||
             std::find(plugin.manifest.callbacks->begin(), plugin.manifest.callbacks->end(),
                       "main") != plugin.manifest.callbacks->end());
        if (block) {
            const PluginRunResult result = run_blocking_worker(
                plugin, callback, true, auto_start_main, kStageCallbackTimeoutSeconds);
            if (result != PluginRunResult::Success) {
                LOGW("plugin %s stage %s failed", plugin.id.c_str(), callback.c_str());
                success = false;
            }
        } else if (!run_detached_worker(plugin, callback, auto_start_main)) {
            LOGW("plugin %s stage %s could not be launched", plugin.id.c_str(), callback.c_str());
            success = false;
        }
    }
    return success;
}

bool stop_plugin_daemons(const std::string& plugin_id, std::string* error) {
    if (!plugin_id_is_valid(plugin_id)) {
        *error = "Invalid plugin id";
        return false;
    }
    bool success = true;
    for (const auto& directory : {fs::path(PLUGIN_LOCK_DIR), daemon_lock_directory(plugin_id)}) {
        const bool legacy = directory == fs::path(PLUGIN_LOCK_DIR);
        std::error_code iterator_error;
        fs::directory_iterator it(directory, iterator_error);
        if (iterator_error == std::errc::no_such_file_or_directory)
            continue;
        for (; !iterator_error && it != fs::directory_iterator(); it.increment(iterator_error)) {
            const std::string name = it->path().filename().string();
            bool matches = daemon_lock_belongs_to(name, plugin_id);
            if (!legacy) {
                constexpr std::string_view suffix = ".lock";
                const size_t length = name.size() > suffix.size() ? name.size() - suffix.size() : 0;
                matches =
                    length > 0 && length <= kMaxCallbackBytes * 2 && length % 2 == 0 &&
                    name.substr(length) == suffix &&
                    std::all_of(
                        name.begin(), name.begin() + static_cast<std::ptrdiff_t>(length),
                        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
            }
            if (!matches)
                continue;
            std::string stop_error;
            if (!stop_daemon_from_lock(it->path(), &stop_error)) {
                success = false;
                if (error->empty())
                    *error = std::move(stop_error);
            }
        }
        if (iterator_error) {
            success = false;
            if (error->empty())
                *error = "Cannot enumerate plugin daemons: " + iterator_error.message();
        }
    }
    return success;
}

bool launch_plugin_daemon(const std::string& plugin_id, const std::string& callback,
                          int64_t interval_seconds) {
    std::string error;
    const auto plugin = load_plugin_record(plugin_id, true, &error);
    if (!plugin) {
        print_error("%s\n", error.c_str());
        return false;
    }
    PluginApiContext context{plugin->id, plugin->directory};
    return spawn_daemon_process(&context, callback, std::max<int64_t>(1, interval_seconds));
}

bool start_plugin_daemon(const std::string& plugin_id, const std::string& callback,
                         int64_t interval_seconds, int ready_fd) {
    if (!plugin_id_is_valid(plugin_id) || !plugin_callback_is_valid(callback) ||
        interval_seconds < 1 || ready_fd <= STDERR_FILENO) {
        report_daemon_ready(&ready_fd, false);
        print_error("Invalid plugin daemon arguments\n");
        return false;
    }
    if (!pidfd_lifecycle_supported()) {
        report_daemon_ready(&ready_fd, false);
        print_error("Plugin daemons require pidfd support\n");
        return false;
    }
    const pid_t session = getsid(0);
    if (session <= 0 || getpgrp() != session || getpid() == session) {
        report_daemon_ready(&ready_fd, false);
        print_error("Plugin daemon is not in a dedicated launcher session\n");
        return false;
    }
    if (ready_fd >= 0) {
        const int descriptor_flags = fcntl(ready_fd, F_GETFD);
        if (descriptor_flags < 0 || fcntl(ready_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
            report_daemon_ready(&ready_fd, false);
            return false;
        }
    }
    switch_cgroups();

    std::error_code directory_error;
    fs::create_directories(daemon_lock_directory(plugin_id), directory_error);
    if (directory_error) {
        report_daemon_ready(&ready_fd, false);
        print_error("Cannot create plugin lock directory: %s\n", directory_error.message().c_str());
        return false;
    }
    const DaemonLock lock(daemon_lock_directory(plugin_id) /
                          (encode_lock_component(callback) + ".lock"));
    if (!lock.owned()) {
        report_daemon_ready(&ready_fd, lock.ready());
        return lock.ready();
    }
    const int lock_fd = lock.fd();
    std::unique_ptr<DaemonLock> legacy_lock;
    const std::string legacy_name = plugin_id + "." + callback + ".daemon.lock";
    if (daemon_lock_belongs_to(legacy_name, plugin_id)) {
        legacy_lock = std::make_unique<DaemonLock>(fs::path(PLUGIN_LOCK_DIR) / legacy_name);
        if (!legacy_lock->owned()) {
            report_daemon_ready(&ready_fd, legacy_lock->ready());
            return legacy_lock->ready();
        }
    }
    const int legacy_fd = legacy_lock ? legacy_lock->fd() : -1;
    if (!publish_daemon_state(lock_fd, legacy_fd, false)) {
        report_daemon_ready(&ready_fd, false);
        print_error("Cannot initialize daemon readiness: %s\n", strerror(errno));
        return false;
    }

    LOGI("plugin daemon %s::%s started with interval %s", plugin_id.c_str(), callback.c_str(),
         std::to_string(interval_seconds).c_str());
    bool success = true;
    while (true) {
        if (!publish_daemon_state(lock_fd, legacy_fd, false)) {
            report_daemon_ready(&ready_fd, false);
            success = false;
            break;
        }
        {
            std::string error;
            const auto plugin = load_plugin_record(plugin_id, true, &error);
            if (!plugin) {
                report_daemon_ready(&ready_fd, false);
                LOGI("plugin daemon %s::%s stopped: %s", plugin_id.c_str(), callback.c_str(),
                     error.c_str());
                success = false;
                break;
            }
            const bool awaiting_ready = ready_fd >= 0;
            bool callback_ready = false;
            const PluginRunResult result = call_plugin(*plugin, callback, false, false, ready_fd,
                                                       &callback_ready, lock_fd, legacy_fd);
            ready_fd = -1;
            if (awaiting_ready && !callback_ready) {
                success = false;
                break;
            }
            if (result == PluginRunResult::MissingCallback) {
                plugin_append_log(plugin->directory, "W",
                                  "daemon callback is missing: " + callback);
                success = false;
                break;
            }
            if (result == PluginRunResult::Failed)
                success = false;
        }
        int64_t remaining = interval_seconds;
        while (remaining > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < deadline)
                (void)poll(nullptr, 0, 100);
            --remaining;
            std::error_code error;
            const fs::path directory = fs::path(PLUGIN_DIR) / plugin_id;
            if (!fs::is_directory(directory, error) || error ||
                fs::exists(directory / DISABLE_FILE_NAME, error) || error) {
                (void)publish_daemon_state(lock_fd, legacy_fd, false);
                return success;
            }
        }
    }
    (void)publish_daemon_state(lock_fd, legacy_fd, false);
    return success;
}

}  // namespace ksud
