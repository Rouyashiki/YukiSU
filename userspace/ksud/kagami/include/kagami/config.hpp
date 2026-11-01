#pragma once

#include <string>
#include <vector>

namespace json {
struct Value;
}

namespace kagami {

struct Config {
    std::string mount_source = "KSU";
    std::string fs_type = "auto";
    std::string work_dir = "/dev/kagami";
    std::string mirror_dir;
    int mirror_img_size_mb = 2048;
    bool overlay_writable = false;
    std::vector<std::string> partitions;
    bool debug = false;
    bool verbose = false;
    bool builtin_mount_enabled = true;
    bool kasumi_enabled = true;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;
    bool enable_overlay_xattr_hide = false;
    bool enable_mount_hide = false;
    bool enable_maps_spoof = false;
    bool enable_statfs_spoof = false;
    bool enable_selinux_fix = false;
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    std::string mount_backend = "auto";
};

std::vector<std::string> load_user_hide_rules();
bool save_user_hide_rules(const std::vector<std::string>& rules);
std::string default_config_json();
void prune_config_fields(json::Value& config);
bool validate_config_patch(const json::Value& patch, std::string& error);
bool write_default_config(std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(Config& config, std::string& error);
bool merge_config_json(const std::string& updates, std::string& error);

}  // namespace kagami
