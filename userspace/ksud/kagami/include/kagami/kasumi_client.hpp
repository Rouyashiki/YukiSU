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
