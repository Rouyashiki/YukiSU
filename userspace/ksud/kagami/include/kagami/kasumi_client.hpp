#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kagami::kasumi {

enum class Status {
    Available = 0,
    NotPresent = 1,
    KernelTooOld = 2,
    ClientTooOld = 3,
};

struct VersionInfo {
    int expected_protocol = 0;
    int kernel_protocol = 0;
    int last_errno = 0;
    int process_uid = -1;
    int process_euid = -1;
    Status status = Status::NotPresent;
};

struct FeatureCapabilities {
    bool ok = false;
    int last_errno = 0;
    int bitmask = 0;
};

struct ControlCapabilities {
    bool ok = false;
    int last_errno = 0;
    int protocol = 0;
    std::uint64_t capabilities = 0;
    std::uint64_t generation = 0;
    std::uint64_t operations = 0;
    std::uint32_t max_stage = 0;
    std::uint32_t max_operations = 0;
    std::uint32_t max_bytes = 0;
};

enum class PathOperation : std::uint32_t {
    Redirect = 1,
    Merge = 2,
    Hide = 3,
    Delete = 4,
    HideXattrs = 5,
};

struct PathRule {
    PathOperation operation;
    std::string path;
    std::string source;
    std::uint32_t flags = 0;
};

struct TransactionMapsRule {
    std::uint64_t target_ino = 0;
    std::uint64_t target_dev = 0;
    std::uint64_t spoofed_ino = 0;
    std::uint64_t spoofed_dev = 0;
    std::string spoofed_path;
    std::uint32_t fields = 7;
    std::uint32_t match_flags = 0;
};

struct TransactionKstatRule {
    std::uint32_t fields = 0;
    std::uint32_t match_flags = 0;
    std::uint64_t target_ino = 0;
    std::uint64_t target_dev = 0;
    std::uint64_t spoofed_ino = 0;
    std::uint64_t spoofed_dev = 0;
    std::uint64_t spoofed_nlink = 0;
    std::uint64_t spoofed_size = 0;
    std::uint64_t spoofed_atime_sec = 0;
    std::uint64_t spoofed_atime_nsec = 0;
    std::uint64_t spoofed_mtime_sec = 0;
    std::uint64_t spoofed_mtime_nsec = 0;
    std::uint64_t spoofed_ctime_sec = 0;
    std::uint64_t spoofed_ctime_nsec = 0;
    std::uint64_t spoofed_blksize = 0;
    std::uint64_t spoofed_blocks = 0;
    std::string target_path;
};

struct TransactionConfiguration {
    std::uint32_t fields = 0;
    bool enabled = false;
    std::uint32_t features = 0;
    bool stealth = false;
    bool debug = false;
    std::uint32_t policy_owner = 0;
    std::uint32_t policy_flags = 0;
    std::vector<std::uint32_t> allow_uids;
    std::vector<std::uint32_t> deny_uids;
    std::vector<TransactionMapsRule> maps;
    std::vector<TransactionKstatRule> kstat;
};

bool commit_transaction(const std::vector<PathRule>& rules,
                        const TransactionConfiguration& configuration, std::uint32_t flags);

ControlCapabilities control_capabilities();
bool replace_path_rules(const std::vector<PathRule>& rules);

struct UserHideRule {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint32_t management = 0;
    std::uint32_t binding = 0;
    int error = 0;
    std::string path;
    bool bypass = false;
};

bool user_hide_rules(std::vector<UserHideRule>& rules);
bool upsert_user_hide(const std::string& path, UserHideRule* result = nullptr, bool bypass = false);
bool delete_user_hide(const std::string& path, std::uint64_t id = 0);
bool retry_user_hide(const std::string& path);
// A retained managed definition prevents switching to legacy HIDE after an error.
bool managed_hide_mode(bool& managed);

VersionInfo version_info();
bool is_available();
std::string active_rules();
std::string hooks();
FeatureCapabilities feature_capabilities();
int features();
std::vector<std::string> feature_names(int bitmask);
std::vector<std::string> active_modules_from_rules(const std::string& rules);

bool set_enabled(bool enable);
bool set_debug(bool enable);
bool set_stealth(bool enable);
bool fix_mounts();
bool hide_overlay_xattrs(const std::string& path);
bool clear_overlay_xattr_hiding();
bool set_mount_hide(bool enable);
bool set_maps_spoof(bool enable);
bool set_statfs_spoof(bool enable);
bool set_selinux_fix(bool enable);
bool clear_rules();
bool add_rule(const std::string& target, const std::string& source, int type);
bool add_merge_rule(const std::string& target, const std::string& source, int flags = 0);
bool hide_path(const std::string& path, int flags = 0);
bool rule_flags_supported(int flags);
bool delete_rule(const std::string& path);
bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino,
                   unsigned long spoofed_dev, const std::string& spoofed_path);
bool clear_maps_rules();
int enabled_state();
int process_uid();
int process_euid();

}  // namespace kagami::kasumi
