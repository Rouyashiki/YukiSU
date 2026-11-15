#include "kagami/kasumi_client.hpp"

#include "uapi/kasumi.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kagami::kasumi {

namespace {
bool protocol_supported(int version) {
    return version == KSM_PROTOCOL_VERSION;
}
}  // namespace

static_assert(sizeof(kasumi_user_hide_arg) == 4160);
static_assert(offsetof(kasumi_user_hide_arg, path) == 64);

extern "C" int ksu_kasumi_ioctl(unsigned long cmd, void* arg);
extern "C" int ksu_kasumi_open_fd();

namespace {
int execute(unsigned long cmd, void* arg) {
    if (cmd != KSM_IOC_GET_VERSION) {
        int version = 0;
        if (ksu_kasumi_ioctl(KSM_IOC_GET_VERSION, &version) != 0)
            return -1;
        if (!protocol_supported(version)) {
            errno = EPROTO;
            return -1;
        }
    }
    return ksu_kasumi_ioctl(cmd, arg);
}

bool ioctl_arg_ok(int rc, int arg_err) {
    if (rc != 0) {
        return false;
    }
    if (arg_err != 0) {
        errno = arg_err < 0 ? -arg_err : arg_err;
        return false;
    }
    return true;
}
}  // namespace

static_assert(sizeof(kasumi_capabilities) == 64);
static_assert(sizeof(kasumi_transaction) == 64);
static_assert(sizeof(kasumi_transaction_rule) == 24);
static_assert(sizeof(kasumi_transaction_stage) == 24);
static_assert(sizeof(kasumi_transaction_configuration) == 112);
static_assert(sizeof(kasumi_transaction_maps_rule) == 296);

namespace {
int control_request(int fd, unsigned long command, void* argument) {
#if defined(__linux__)
    if (fd >= 0)
        return ioctl(fd, command, argument);
#else
    (void)fd;
#endif
    return execute(command, argument);
}

ControlCapabilities read_control_capabilities(int fd) {
    ControlCapabilities result;
    if (control_request(fd, KSM_IOC_GET_VERSION, &result.protocol) != 0) {
        result.last_errno = errno != 0 ? errno : EIO;
        return result;
    }
    if (!protocol_supported(result.protocol)) {
        result.last_errno = EPROTO;
        return result;
    }
    kasumi_capabilities capabilities{};
    capabilities.size = sizeof(capabilities);
    capabilities.version = KSM_CAPABILITIES_VERSION;
    if (control_request(fd, KSM_IOC_GET_CAPABILITIES, &capabilities) != 0) {
        result.last_errno = errno != 0 ? errno : EIO;
        return result;
    }
    if (capabilities.size != sizeof(capabilities) ||
        capabilities.version != KSM_CAPABILITIES_VERSION ||
        ((capabilities.capabilities & KSM_CAP_ATOMIC_PATH_RULES) &&
         (!capabilities.max_stage || !capabilities.max_operations || !capabilities.max_bytes))) {
        result.last_errno = EPROTO;
        return result;
    }
    result.ok = true;
    result.capabilities = capabilities.capabilities;
    result.generation = capabilities.generation;
    result.operations = capabilities.operations;
    result.max_stage = capabilities.max_stage;
    result.max_operations = capabilities.max_operations;
    result.max_bytes = capabilities.max_bytes;
    return result;
}

int open_transaction_fd() {
    return ksu_kasumi_open_fd();
}

class RuleTransaction {
public:
    explicit RuleTransaction(int fd) : fd_(fd) {}
    RuleTransaction(const RuleTransaction&) = delete;
    RuleTransaction& operator=(const RuleTransaction&) = delete;
    RuleTransaction(RuleTransaction&&) = delete;
    RuleTransaction& operator=(RuleTransaction&&) = delete;
    ~RuleTransaction() {
        const int saved_errno = errno;
        if (started_ && !committed_) {
            if (!status_.id)
                (void)query();
            if (status_.id)
                (void)control_request(fd_, KSM_IOC_ABORT_TRANSACTION, &status_);
        }
#if defined(__linux__)
        close(fd_);
#endif
        errno = saved_errno;
    }

    bool begin(std::uint64_t generation, std::uint32_t flags) {
        status_.size = sizeof(status_);
        status_.flags = flags;
        status_.expected_generation = generation;
        started_ = true;
        const int result = control_request(fd_, KSM_IOC_BEGIN_TRANSACTION, &status_);
        const int error = errno != 0 ? errno : EIO;
        if (result != 0 && !query(true)) {
            errno = error;
            return false;
        }
        if (!status_.id || status_.state != KSM_TX_STATE_STAGING) {
            errno = status_.error ? -status_.error : EPROTO;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool stage(const std::vector<PathRule>& rules, std::uint32_t maximum) const {
        const auto chunk_size = std::min(maximum, KSM_TX_MAX_STAGE);
        std::vector<kasumi_transaction_rule> chunk;
        chunk.reserve(chunk_size);
        for (std::size_t offset = 0; offset < rules.size(); offset += chunk.size()) {
            chunk.clear();
            const auto count = std::min<std::size_t>(chunk_size, rules.size() - offset);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& rule = rules[offset + i];
                chunk.push_back({static_cast<__u32>(rule.operation), rule.flags,
                                 reinterpret_cast<std::uintptr_t>(rule.path.c_str()),
                                 rule.source.empty()
                                     ? 0
                                     : reinterpret_cast<std::uintptr_t>(rule.source.c_str())});
            }
            kasumi_transaction_stage request{};
            request.size = sizeof(request);
            request.count = static_cast<__u32>(chunk.size());
            request.id = status_.id;
            request.rules = reinterpret_cast<std::uintptr_t>(chunk.data());
            if (control_request(fd_, KSM_IOC_STAGE_TRANSACTION, &request) != 0)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool configure(const TransactionConfiguration& configuration) const {
        if (!configuration.fields)
            return true;
        std::vector<kasumi_transaction_maps_rule> maps;
        maps.reserve(configuration.maps.size());
        for (const auto& rule : configuration.maps) {
            kasumi_transaction_maps_rule entry{};
            entry.fields = rule.fields;
            entry.match_flags = rule.match_flags;
            entry.target_ino = rule.target_ino;
            entry.target_dev = rule.target_dev;
            entry.spoofed_ino = rule.spoofed_ino;
            entry.spoofed_dev = rule.spoofed_dev;
            std::memcpy(entry.spoofed_pathname, rule.spoofed_path.c_str(),
                        rule.spoofed_path.size() + 1);
            maps.push_back(entry);
        }
        std::vector<kasumi_transaction_kstat_rule> kstat;
        kstat.reserve(configuration.kstat.size());
        for (const auto& rule : configuration.kstat) {
            kasumi_transaction_kstat_rule entry{};
            entry.fields = rule.fields;
            entry.match_flags = rule.match_flags;
            entry.target_ino = rule.target_ino;
            entry.target_dev = rule.target_dev;
            entry.spoofed_ino = rule.spoofed_ino;
            entry.spoofed_dev = rule.spoofed_dev;
            entry.spoofed_nlink = rule.spoofed_nlink;
            entry.spoofed_size = rule.spoofed_size;
            entry.spoofed_atime_sec = rule.spoofed_atime_sec;
            entry.spoofed_atime_nsec = rule.spoofed_atime_nsec;
            entry.spoofed_mtime_sec = rule.spoofed_mtime_sec;
            entry.spoofed_mtime_nsec = rule.spoofed_mtime_nsec;
            entry.spoofed_ctime_sec = rule.spoofed_ctime_sec;
            entry.spoofed_ctime_nsec = rule.spoofed_ctime_nsec;
            entry.spoofed_blksize = rule.spoofed_blksize;
            entry.spoofed_blocks = rule.spoofed_blocks;
            std::memcpy(entry.target_pathname, rule.target_path.c_str(),
                        rule.target_path.size() + 1);
            kstat.push_back(entry);
        }
        kasumi_transaction_configuration request{};
        request.size = sizeof(request);
        request.id = status_.id;
        request.fields = configuration.fields;
        request.enabled = configuration.enabled;
        request.features = configuration.features;
        request.stealth = configuration.stealth;
        request.debug = configuration.debug;
        request.policy_owner = configuration.policy_owner;
        request.policy_flags = configuration.policy_flags;
        request.allow_count = static_cast<__u32>(configuration.allow_uids.size());
        request.deny_count = static_cast<__u32>(configuration.deny_uids.size());
        request.allow_uids =
            configuration.allow_uids.empty()
                ? 0
                : reinterpret_cast<std::uintptr_t>(configuration.allow_uids.data());
        request.deny_uids = configuration.deny_uids.empty()
                                ? 0
                                : reinterpret_cast<std::uintptr_t>(configuration.deny_uids.data());
        request.maps_count = static_cast<__u32>(maps.size());
        request.maps_rules = maps.empty() ? 0 : reinterpret_cast<std::uintptr_t>(maps.data());
        request.kstat_count = static_cast<__u32>(kstat.size());
        request.kstat_rules = kstat.empty() ? 0 : reinterpret_cast<std::uintptr_t>(kstat.data());
        return control_request(fd_, KSM_IOC_STAGE_CONFIGURATION, &request) == 0;
    }

    bool commit() {
        const int result = control_request(fd_, KSM_IOC_COMMIT_TRANSACTION, &status_);
        const int error = errno != 0 ? errno : EIO;
        if (result != 0 && !query(true)) {
            errno = error;
            return false;
        }
        committed_ = status_.state == KSM_TX_STATE_COMMITTED;
        if (!committed_) {
            const int fallback = result != 0 ? error : EPROTO;
            errno = status_.error ? -status_.error : fallback;
        }
        return committed_;
    }

private:
    bool query(bool latest = false) {
        kasumi_transaction request{};
        request.size = sizeof(request);
        request.id = latest ? 0 : status_.id;
        if (control_request(fd_, KSM_IOC_QUERY_TRANSACTION, &request) != 0)
            return false;
        if (request.size != sizeof(request) || !request.id) {
            errno = EPROTO;
            return false;
        }
        status_ = request;
        return true;
    }

    int fd_;
    kasumi_transaction status_{};
    bool started_ = false;
    bool committed_ = false;
};
}  // namespace

ControlCapabilities control_capabilities() {
    return read_control_capabilities(-1);
}

namespace {
bool commit_transaction_once(const std::vector<PathRule>& rules,
                             const TransactionConfiguration& configuration, std::uint32_t flags) {
    const int fd = open_transaction_fd();
    if (fd < 0)
        return false;
    RuleTransaction transaction(fd);
    const auto capabilities = read_control_capabilities(fd);
    std::uint64_t required = 0;
    if (!rules.empty() || flags)
        required |= KSM_CAP_ATOMIC_PATH_RULES;
    if (configuration.fields & KSM_TX_CONFIG_SETTINGS)
        required |= KSM_CAP_ATOMIC_SETTINGS;
    if (configuration.fields & (KSM_TX_CONFIG_POLICY | KSM_TX_CONFIG_RESET_POLICY))
        required |= KSM_CAP_ATOMIC_POLICY;
    if (configuration.fields & KSM_TX_CONFIG_MAPS)
        required |= KSM_CAP_ATOMIC_MAPS;
    if (configuration.fields & KSM_TX_CONFIG_KSTAT)
        required |= KSM_CAP_ATOMIC_KSTAT;
    if (!capabilities.ok || capabilities.protocol != KSM_PROTOCOL_VERSION ||
        (capabilities.capabilities & required) != required) {
        errno = capabilities.ok ? EOPNOTSUPP : capabilities.last_errno;
        return false;
    }
    if ((flags & ~(KSM_TX_REPLACE_PATH_RULES | KSM_TX_REPLACE_XATTR_RULES)) ||
        (configuration.fields &
         ~(KSM_TX_CONFIG_SETTINGS | KSM_TX_CONFIG_POLICY | KSM_TX_CONFIG_MAPS |
           KSM_TX_CONFIG_KSTAT | KSM_TX_CONFIG_RESET_POLICY))) {
        errno = EINVAL;
        return false;
    }
    if (configuration.allow_uids.size() > UINT32_MAX ||
        configuration.deny_uids.size() > UINT32_MAX || configuration.maps.size() > UINT32_MAX ||
        configuration.kstat.size() > UINT32_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    for (const auto& rule : configuration.maps) {
        if ((rule.fields & ~KSM_MAPS_FIELDS_ALL) || (rule.match_flags & ~KSM_MATCH_ANY_DEVICE) ||
            rule.spoofed_path.size() >= KSM_MAX_LEN_PATHNAME ||
            rule.spoofed_path.find('\0') != std::string::npos) {
            errno = EINVAL;
            return false;
        }
    }
    if (!configuration.kstat.empty() && !(configuration.fields & KSM_TX_CONFIG_KSTAT)) {
        errno = EINVAL;
        return false;
    }
    for (const auto& rule : configuration.kstat) {
        if ((rule.target_path.empty() && !rule.target_ino) ||
            (!rule.target_path.empty() && rule.target_path.front() != '/') ||
            rule.target_path.size() >= KSM_MAX_LEN_PATHNAME ||
            rule.target_path.find('\0') != std::string::npos ||
            (rule.fields & ~KSM_KSTAT_FIELDS_ALL) || (rule.match_flags & ~KSM_MATCH_ANY_DEVICE) ||
            rule.target_dev > UINT32_MAX || rule.spoofed_dev > UINT32_MAX ||
            rule.spoofed_nlink > UINT32_MAX || rule.spoofed_blksize > UINT32_MAX ||
            ((rule.fields & KSM_KSTAT_FIELD_ATIME) && rule.spoofed_atime_nsec >= 1000000000) ||
            ((rule.fields & KSM_KSTAT_FIELD_MTIME) && rule.spoofed_mtime_nsec >= 1000000000) ||
            ((rule.fields & KSM_KSTAT_FIELD_CTIME) && rule.spoofed_ctime_nsec >= 1000000000)) {
            errno = EINVAL;
            return false;
        }
    }
    if (configuration.maps.size() > capabilities.max_operations ||
        configuration.kstat.size() > capabilities.max_operations ||
        configuration.allow_uids.size() > 1024 || configuration.deny_uids.size() > 1024) {
        errno = E2BIG;
        return false;
    }
    std::uint64_t bytes = 0;
    if (configuration.fields & KSM_TX_CONFIG_POLICY)
        bytes += (configuration.allow_uids.size() + configuration.deny_uids.size()) * sizeof(__u32);
    if (configuration.fields & KSM_TX_CONFIG_MAPS)
        bytes += configuration.maps.size() * sizeof(kasumi_transaction_maps_rule);
    if (configuration.fields & KSM_TX_CONFIG_KSTAT)
        bytes += configuration.kstat.size() * sizeof(kasumi_transaction_kstat_rule);
    if (bytes > capabilities.max_bytes) {
        errno = E2BIG;
        return false;
    }
    if (rules.size() > capabilities.max_operations) {
        errno = E2BIG;
        return false;
    }
    for (const auto& rule : rules) {
        const bool source_required =
            rule.operation == PathOperation::Redirect || rule.operation == PathOperation::Merge;
        if (rule.path.empty() || rule.path.front() != '/' ||
            rule.path.find('\0') != std::string::npos || source_required != !rule.source.empty() ||
            (!rule.source.empty() &&
             (rule.source.front() != '/' || rule.source.find('\0') != std::string::npos)) ||
            (rule.flags & ~KSM_RULE_FLAGS_MASK) ||
            (rule.flags && (rule.operation == PathOperation::Delete ||
                            rule.operation == PathOperation::HideXattrs))) {
            errno = EINVAL;
            return false;
        }
        const auto operation = static_cast<std::uint32_t>(rule.operation);
        if (operation >= 64 || !(capabilities.operations & (1ULL << operation))) {
            errno = EOPNOTSUPP;
            return false;
        }
    }
    return transaction.begin(capabilities.generation, flags) &&
           transaction.configure(configuration) &&
           transaction.stage(rules, capabilities.max_stage) && transaction.commit();
}

}  // namespace

bool commit_transaction(const std::vector<PathRule>& rules,
                        const TransactionConfiguration& configuration, std::uint32_t flags) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (commit_transaction_once(rules, configuration, flags))
            return true;
        if (errno != EAGAIN)
            return false;
    }
    return false;
}

bool replace_path_rules(const std::vector<PathRule>& rules) {
    return commit_transaction(rules, {}, KSM_TX_REPLACE_PATH_RULES);
}

VersionInfo version_info() {
    VersionInfo info;
    info.expected_protocol = KSM_PROTOCOL_VERSION;
    info.process_uid = process_uid();
    info.process_euid = process_euid();

    int version = 0;
    if (execute(KSM_IOC_GET_VERSION, &version) != 0) {
        info.last_errno = errno;
        info.status = Status::NotPresent;
        return info;
    }

    info.kernel_protocol = version;
    if (version < KSM_PROTOCOL_VERSION) {
        info.status = Status::KernelTooOld;
    } else if (version > KSM_PROTOCOL_VERSION) {
        info.status = Status::ClientTooOld;
    } else {
        info.status = Status::Available;
    }
    return info;
}

int process_uid() {
#if defined(__linux__)
    return static_cast<int>(getuid());
#else
    return -1;
#endif
}

int process_euid() {
#if defined(__linux__)
    return static_cast<int>(geteuid());
#else
    return -1;
#endif
}

bool is_available() {
    return version_info().status == Status::Available;
}

std::string active_rules() {
    std::vector<char> buffer(64UL * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_LIST_RULES, &arg) != 0) {
        return "";
    }
    return {buffer.data()};
}

std::string hooks() {
    std::vector<char> buffer(8UL * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_GET_HOOKS, &arg) != 0) {
        return "";
    }
    return {buffer.data()};
}

FeatureCapabilities feature_capabilities() {
    FeatureCapabilities capabilities;
    int bitmask = 0;
    if (execute(KSM_IOC_GET_FEATURES, &bitmask) != 0) {
        capabilities.last_errno = errno != 0 ? errno : EIO;
        return capabilities;
    }
    capabilities.ok = true;
    capabilities.bitmask = bitmask;
    return capabilities;
}

int features() {
    return feature_capabilities().bitmask;
}

std::vector<std::string> feature_names(int bitmask) {
    std::vector<std::string> names;
    if (bitmask & KSM_FEATURE_SAFE_RULE_RETIRE)
        names.emplace_back("safe_rule_retire");
    if (bitmask & KSM_FEATURE_RULE_BYPASS)
        names.emplace_back("rule_bypass");
    if (bitmask & KSM_FEATURE_MANAGED_HIDE)
        names.emplace_back("managed_hide");
    if (bitmask & KSM_FEATURE_MOUNT_HIDE)
        names.emplace_back("mount_hide");
    if (bitmask & KSM_FEATURE_MAPS_SPOOF)
        names.emplace_back("maps_spoof");
    if (bitmask & KSM_FEATURE_STATFS_SPOOF)
        names.emplace_back("statfs_spoof");
    if (bitmask & KSM_FEATURE_SELINUX_FIX)
        names.emplace_back("selinux_fix");
    if (bitmask & KSM_FEATURE_KSTAT_SPOOF)
        names.emplace_back("kstat_spoof");
    if (bitmask & KSM_FEATURE_MERGE_DIR)
        names.emplace_back("merge_dir");
    if (bitmask & KSM_FEATURE_OVERLAY_XATTR_HIDE)
        names.emplace_back("overlay_xattr_hide");
    if (bitmask & KSM_FEATURE_FAKE_MOUNTINFO)
        names.emplace_back("fake_mountinfo");
    return names;
}

namespace {
bool hide_request_path(kasumi_user_hide_arg& arg, const std::string& path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    if (path.size() >= sizeof(arg.path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    arg.size = sizeof(arg);
    arg.path_len = static_cast<std::uint32_t>(path.size());
    std::memcpy(arg.path, path.c_str(), path.size() + 1);
    return true;
}

bool hide_reply(const kasumi_user_hide_arg& arg, UserHideRule& result) {
    if (arg.size != sizeof(arg) || (arg.flags & ~KSM_RULE_FLAGS_MASK) ||
        arg.path_len >= sizeof(arg.path) || arg.path[arg.path_len] ||
        std::strlen(arg.path) != arg.path_len) {
        errno = EPROTO;
        return false;
    }
    result = {arg.rule_id,
              arg.generation,
              arg.management,
              arg.binding,
              arg.error,
              std::string(arg.path, arg.path_len),
              (arg.flags & KSM_RULE_BYPASS_POLICY) != 0};
    return true;
}
}  // namespace

bool user_hide_rules(std::vector<UserHideRule>& rules) {
    std::vector<UserHideRule> snapshot;
    std::uint64_t cursor = 0;
    for (;;) {
        kasumi_user_hide_arg arg = {};
        arg.size = sizeof(arg);
        arg.rule_id = cursor;
        if (execute(KSM_IOC_USER_HIDE_QUERY, &arg))
            return false;
        UserHideRule rule;
        if (!hide_reply(arg, rule))
            return false;
        if (!rule.id) {
            rules = std::move(snapshot);
            return true;
        }
        if (rule.id <= cursor || snapshot.size() >= 4096) {
            errno = EOVERFLOW;
            return false;
        }
        cursor = rule.id;
        snapshot.push_back(std::move(rule));
    }
}

bool upsert_user_hide(const std::string& path, UserHideRule* result, bool bypass) {
    const int flags = bypass ? KSM_RULE_BYPASS_POLICY : 0;
    if (!rule_flags_supported(flags))
        return false;
    kasumi_user_hide_arg arg = {};
    arg.flags = flags;
    if (!hide_request_path(arg, path) || execute(KSM_IOC_USER_HIDE_UPSERT, &arg))
        return false;
    UserHideRule reply;
    if (!hide_reply(arg, reply))
        return false;
    if (result)
        *result = std::move(reply);
    return true;
}

bool delete_user_hide(const std::string& path, std::uint64_t id) {
    kasumi_user_hide_arg arg = {};
    arg.rule_id = id;
    return hide_request_path(arg, path) && !execute(KSM_IOC_USER_HIDE_DELETE, &arg);
}

bool managed_hide_mode(bool& managed) {
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (caps.bitmask & KSM_FEATURE_MANAGED_HIDE) {
        managed = true;
        return true;
    }
    std::vector<UserHideRule> rules;
    if (user_hide_rules(rules)) {
        managed = !rules.empty();
        return true;
    }
    if (errno != EINVAL && errno != ENOTTY && errno != EOPNOTSUPP)
        return false;
    managed = false;
    return true;
}

bool retry_user_hide(const std::string& path) {
    std::vector<UserHideRule> rules;
    if (!user_hide_rules(rules))
        return false;
    for (const auto& rule : rules) {
        if (rule.path != path)
            continue;
        kasumi_user_hide_arg arg = {};
        arg.rule_id = rule.id;
        return hide_request_path(arg, path) && !execute(KSM_IOC_USER_HIDE_RETRY, &arg);
    }
    errno = ENOENT;
    return false;
}

std::vector<std::string> active_modules_from_rules(const std::string& rules) {
    std::set<std::string> modules;
    std::istringstream lines(rules);
    std::string line;
    while (std::getline(lines, line)) {
        const std::vector<std::string> prefixes = {
            "/data/adb/modules/",
        };
        for (const auto& prefix : prefixes) {
            std::size_t pos = line.find(prefix);
            while (pos != std::string::npos) {
                const std::size_t start = pos + prefix.size();
                const std::size_t end = line.find('/', start);
                if (end != std::string::npos && end > start) {
                    modules.insert(line.substr(start, end - start));
                }
                pos = line.find(prefix, start);
            }
        }
    }
    return {modules.begin(), modules.end()};
}

bool set_enabled(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_ENABLED, &value) == 0;
}

bool set_debug(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_DEBUG, &value) == 0;
}

bool set_stealth(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_STEALTH, &value) == 0;
}

bool fix_mounts() {
    return execute(KSM_IOC_REORDER_MNT_ID, nullptr) == 0;
}

bool hide_overlay_xattrs(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
}

bool clear_overlay_xattr_hiding() {
    if (!(features() & KSM_FEATURE_OVERLAY_XATTR_HIDE))
        return true;
    return hide_overlay_xattrs("");
}

bool set_mount_hide(bool enable) {
    kasumi_mount_hide_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(KSM_IOC_SET_MOUNT_HIDE, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool set_maps_spoof(bool enable) {
    kasumi_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(KSM_IOC_SET_MAPS_SPOOF, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool set_statfs_spoof(bool enable) {
    kasumi_statfs_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(KSM_IOC_SET_STATFS_SPOOF, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool set_selinux_fix(bool enable) {
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & KSM_FEATURE_SELINUX_FIX)) {
        if (!enable)
            return true;
        errno = EOPNOTSUPP;
        return false;
    }
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SELINUX_FIX, &value) == 0;
}

bool clear_rules() {
    return execute(KSM_IOC_CLEAR_ALL, nullptr) == 0;
}

bool rule_flags_supported(int flags) {
    if (flags & ~KSM_RULE_FLAGS_MASK) {
        errno = EINVAL;
        return false;
    }
    if (!flags)
        return true;
    const auto caps = feature_capabilities();
    if (!caps.ok || !(caps.bitmask & KSM_FEATURE_RULE_BYPASS)) {
        errno = caps.ok ? EOPNOTSUPP : caps.last_errno;
        return false;
    }
    return true;
}

bool add_rule(const std::string& target, const std::string& source, int type) {
    if (!rule_flags_supported(type))
        return false;
    kasumi_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    arg.type = type;
    return execute(KSM_IOC_ADD_RULE, &arg) == 0;
}

bool add_merge_rule(const std::string& target, const std::string& source, int flags) {
    if (!rule_flags_supported(flags))
        return false;
    kasumi_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    arg.type = flags;
    return execute(KSM_IOC_ADD_MERGE_RULE, &arg) == 0;
}

bool hide_path(const std::string& path, int flags) {
    if (!rule_flags_supported(flags))
        return false;
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    arg.type = flags;
    return execute(KSM_IOC_HIDE_RULE, &arg) == 0;
}

bool delete_rule(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_DEL_RULE, &arg) == 0;
}

bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino,
                   unsigned long spoofed_dev, const std::string& spoofed_path) {
    kasumi_maps_rule arg = {};
    arg.target_ino = target_ino;
    arg.target_dev = target_dev;
    arg.spoofed_ino = spoofed_ino;
    arg.spoofed_dev = spoofed_dev;
    std::strncpy(arg.spoofed_pathname, spoofed_path.c_str(), KSM_MAX_LEN_PATHNAME - 1);
    const int rc = execute(KSM_IOC_ADD_MAPS_RULE, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool clear_maps_rules() {
    return execute(KSM_IOC_CLEAR_MAPS_RULES, nullptr) == 0;
}

int enabled_state() {
    int enabled = 0;
    return execute(KSM_IOC_GET_ENABLED, &enabled) == 0 ? enabled : -1;
}

}  // namespace kagami::kasumi
