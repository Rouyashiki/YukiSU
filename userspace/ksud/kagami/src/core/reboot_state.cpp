#include "core/reboot_state.hpp"
#include "core/ksucalls.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "uapi/feature.h"
#include "uapi/kasumi.h"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace kagami::reboot {
namespace {
int owner_fd = -1;
pid_t owner_pid = -1;
std::string request_token;

std::filesystem::path lock_file() {
    return runtime_data_dir() / "run" / "soft_reboot.lock";
}
std::filesystem::path state_file() {
    return runtime_data_dir() / "run" / "soft_reboot.json";
}
std::filesystem::path cycle_file() {
    return runtime_data_dir() / "run" / "mount_cycle";
}

JsonValue read_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    JsonValue result;
    std::string error;
    if (text.size() > 65536 || !parse_json(text, result, error) || !result.is_object())
        return JsonValue::object();
    return result;
}

bool saved_rules_readable(std::string& error) {
    const auto absolute_path = [](const JsonValue& value) {
        return value.is_string() && !value.s.empty() && value.s.front() == '/' &&
               value.s.find('\0') == std::string::npos;
    };
    for (const auto* name : {"user_hide_rules.json", "hide_bypass.json", "module_bypass.json",
                             "module_mode.json", "module_rules.json"}) {
        const auto path = runtime_data_dir() / name;
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (!ec && !exists)
            continue;
        std::ifstream in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        JsonValue saved;
        bool valid = !ec && in && parse_json(text, saved, error);
        if (std::strcmp(name, "user_hide_rules.json") == 0) {
            valid = valid && saved.is_array() &&
                    std::all_of(saved.a.begin(), saved.a.end(), absolute_path);
        } else {
            valid = valid && saved.is_object();
            for (const auto& [id, value] : saved.o) {
                if (std::strcmp(name, "module_rules.json") != 0) {
                    valid = valid && value.is_string();
                    continue;
                }
                valid = valid && value.is_array();
                for (const auto& rule : value.a) {
                    const auto* bypass = rule.find("bypass");
                    valid = valid && rule.is_object() && absolute_path(rule.at("path")) &&
                            rule.at("mode").is_string() && (!bypass || bypass->is_bool());
                }
            }
        }
        if (!valid) {
            error = "cannot restore invalid or unreadable saved rules: " + path.string();
            return false;
        }
    }
    return true;
}

bool cycle_number(unsigned int& number) {
    number = 0;
    const auto saved = read_json(cycle_file());
    std::error_code ec;
    if (saved.o.empty() && std::filesystem::exists(cycle_file(), ec))
        return false;
    if (ec)
        return false;
    if (saved.at("boot_id").string_or("") != runtime_boot_id())
        return true;
    const auto& value = saved.at("sequence");
    if (!value.is_number() || !std::isfinite(value.n) || value.n < 0 ||
        value.n >= std::numeric_limits<unsigned int>::max() ||
        value.n != static_cast<unsigned int>(value.n))
        return false;
    number = static_cast<unsigned int>(value.n);
    return true;
}
}  // namespace

JsonValue state() {
    return read_json(state_file());
}

bool recover_abandoned(
    const std::function<bool(const std::string&, std::string&)>& recover_android) {
    const auto saved = state();
    if (saved.at("boot_id").string_or("") != runtime_boot_id() ||
        saved.at("result").string_or("") != "accepted" || active())
        return false;
    const int fd = open(lock_file().c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        close(fd);
        return false;
    }
    auto current = state();
    if (current.at("token").string_or("") != saved.at("token").string_or("") ||
        current.at("result").string_or("") != "accepted") {
        close(fd);
        return false;
    }
    const auto phase = current.at("phase").string_or("");
    const auto token = current.at("token").string_or("");
    const auto previous_token = client_token();
    const auto previous_request = request_token;
    const bool has_context = !token.empty() && setenv(token_env, token.c_str(), 1) == 0;
    owner_fd = fd;
    owner_pid = getpid();
    request_token = has_context ? token : "";
    const bool needs_start = phase != "preflight" && phase != "prepare_waiter";
    std::string recovery_error;
    const bool recovered = !needs_start || (has_context && recover_android(phase, recovery_error));
    if (!has_context && needs_start)
        recovery_error = "cannot authorize soft reboot recovery";
    current = state();
    current["result"] = "failed";
    current["error"] = "coordinator exited during " + phase +
                       (recovered ? "" : "; recovery failed: " + recovery_error);
    const bool written = ksud::write_file_atomic(state_file(), stringify_json(current) + "\n");
    release();
    request_token = previous_request;
    if (!previous_token.empty())
        setenv(token_env, previous_token.c_str(), 1);
    return written;
}

bool check_support(std::string& error) {
    error.clear();
    if (!ksud::ensure_uapi_version_matched(&error))
        return false;
    if (ksud::is_safe_mode() || ksud::has_magisk()) {
        error = "soft reboot rule restoration is unavailable in safe mode or with Magisk boot "
                "ownership";
        return false;
    }
    Config config;
    if (!read_config_file(config, error, MissingConfig::UseDefaults))
        return false;
    if (!saved_rules_readable(error))
        return false;
    ksud::GetFeatureCmd feature = {KSU_FEATURE_KASUMI, 0, 0};
    std::error_code ec;
    if (std::filesystem::exists(runtime_data_dir() / "run" / "mount_disabled", ec) || ec) {
        error = "module mounting is disabled by bootloop protection or its state is unreadable";
        return false;
    }
    if (ksud::ksuctl(KSU_IOCTL_GET_FEATURE, &feature) < 0) {
        error = "cannot query the kernel Kasumi feature";
        return false;
    }
    const auto version = kasumi::version_info();
    const bool available = version.status == kasumi::Status::Available;
    const bool present = version.status != kasumi::Status::NotPresent;
    const int runtime = kasumi::enabled_state();
    const bool involved = runtime > 0 || feature.value != 0 || (present && config.kasumi_enabled);
    const bool absent = version.status == kasumi::Status::NotPresent &&
                        (version.last_errno == ENODEV || version.last_errno == ENOTTY ||
                         version.last_errno == EOPNOTSUPP || version.last_errno == EINVAL);
    if ((present && runtime < 0) || (!present && !absent)) {
        error = "cannot determine Kasumi runtime state";
        return false;
    }
    if (involved) {
        const auto caps = kasumi::feature_capabilities();
        if (!available || !caps.ok || !(caps.bitmask & KSM_FEATURE_SAFE_RULE_RETIRE)) {
            error = "update the running kernel/LKM: safe_rule_retire is required for Kasumi soft "
                    "reboot";
            return false;
        }
        if (!load_user_hide_rules().empty() && !(caps.bitmask & KSM_FEATURE_MANAGED_HIDE)) {
            error = "managed hide support is required to restore saved hide rules";
            return false;
        }
    }
    if (cycle_id().empty()) {
        error = "cannot determine the current mount cycle";
        return false;
    }
    return true;
}

bool active() {
    if (owner_fd >= 0 && owner_pid == getpid())
        return true;
    const int fd = open(lock_file().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno != ENOENT;
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    const bool busy = fcntl(fd, F_GETLK, &lock) < 0 || lock.l_type != F_UNLCK;
    close(fd);
    return busy;
}

bool acquire(std::string& error) {
    if (owner_fd >= 0 && owner_pid == getpid()) {
        error = "soft reboot already running";
        return false;
    }
    if (!prepare_runtime(error))
        return false;
    const int fd = open(lock_file().c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        error = "soft reboot already running or its lock is unavailable";
        close(fd);
        return false;
    }
    // POSIX locks stay with this process, not daemons forked by module stages.
    owner_fd = fd;
    owner_pid = getpid();
    std::ifstream random("/proc/sys/kernel/random/uuid");
    std::string token;
    std::getline(random, token);
    if (token.empty() || setenv(token_env, token.c_str(), 1) != 0) {
        error = "cannot create soft reboot transaction token";
        release();
        return false;
    }
    JsonValue initial = JsonValue::object();
    initial["boot_id"] = runtime_boot_id();
    initial["token"] = token;
    initial["pid"] = static_cast<int>(getpid());
    initial["phase"] = "preflight";
    initial["result"] = "accepted";
    initial["error"] = "";
    initial["pending"] = 0;
    if (!ksud::write_file_atomic(state_file(), stringify_json(initial) + "\n")) {
        error = "cannot record soft reboot transaction";
        release();
        return false;
    }
    return true;
}

void release() {
    if (owner_fd >= 0)
        close(owner_fd);
    owner_fd = -1;
    owner_pid = -1;
    unsetenv(token_env);
}

std::string client_token() {
    const char* token = std::getenv(token_env);
    return token ? token : "";
}

void set_request_token(const std::string& token) {
    request_token = token;
}

bool authorized() {
    const auto token = request_token.empty() ? client_token() : request_token;
    if (token.empty() || !active())
        return false;
    const auto current = state();
    return current.at("boot_id").string_or("") == runtime_boot_id() &&
           current.at("token").string_or("") == token;
}

int claim_services(std::string& error) {
    if (!authorized()) {
        error = "module service recovery requires the soft reboot owner";
        return -1;
    }
    auto current = state();
    if (current.at("services_started").bool_or(false))
        return 0;
    current["services_started"] = true;
    if (!ksud::write_file_atomic(state_file(), stringify_json(current) + "\n")) {
        error = "cannot record module service launch";
        return -1;
    }
    return 1;
}

bool record(const std::string& phase, const std::string& result, const std::string& error,
            int pending) {
    if (!authorized())
        return false;
    auto current = state();
    current["phase"] = phase;
    current["result"] = result;
    current["error"] = error;
    current["pending"] = pending;
    return ksud::write_file_atomic(state_file(), stringify_json(current) + "\n");
}

std::string cycle_id() {
    unsigned int sequence;
    const auto boot = runtime_boot_id();
    if (boot.empty() || !cycle_number(sequence))
        return {};
    return sequence == 0 ? boot : boot + ":" + std::to_string(sequence);
}

bool advance_cycle(std::string& error) {
    unsigned int sequence;
    if (!authorized() || !cycle_number(sequence) ||
        sequence >= std::numeric_limits<unsigned int>::max() - 1) {
        error = "invalid soft reboot mount cycle";
        return false;
    }
    JsonValue next = JsonValue::object();
    next["boot_id"] = runtime_boot_id();
    next["sequence"] = static_cast<double>(sequence) + 1;
    if (!ksud::write_file_atomic(cycle_file(), stringify_json(next) + "\n")) {
        error = "cannot advance mount cycle";
        return false;
    }
    return true;
}

bool marker_matches_cycle(const std::filesystem::path& path) {
    const auto cycle = cycle_id();
    std::ifstream in(path);
    std::string recorded;
    return !cycle.empty() && std::getline(in, recorded) && recorded == cycle;
}

bool write_cycle_marker(const std::filesystem::path& path, const std::string& detail) {
    const auto cycle = cycle_id();
    return !cycle.empty() && ksud::write_file_atomic(path, cycle + "\n" + detail);
}
}  // namespace kagami::reboot
