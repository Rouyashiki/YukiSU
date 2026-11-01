#include "core/command.hpp"
#include "kagami/embedded.hpp"

#include <chrono>
#include "core/daemon.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "defs.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/backend.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"
#include "uapi/kasumi.h"
#include "utils.hpp"

#include <sys/statvfs.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace kagami {

namespace fs = std::filesystem;

using mount::fsutil::managed_partitions;
using mount::fsutil::partition_mount_point;

namespace {
fs::path data_dir() {
    return runtime_data_dir();
}

fs::path modules_dir() {
    return runtime_modules_dir();
}

fs::path config_file() {
    return runtime_config_file();
}

void print_usage() {
    std::cout << "Kagami " << ksud::VERSION_NAME << "\n"
              << "usage:\n"
              << "  ksud kagami version\n"
              << "  ksud kagami config show\n"
              << "  ksud kagami config gen\n"
              << "  ksud kagami config merge-json JSON\n"
              << "  ksud kagami config apply\n"
              << "  ksud kagami daemon status|serve|call|ping|stop\n"
              << "  ksud kagami api system|storage|kasumi|features|hooks|backends|meta\n"
              << "  ksud kagami module "
                 "list|add|delete|set-mode|set-bypass|apply|add-rule|remove-rule|hot-mount|hot-"
                 "unmount|check-"
                 "conflicts|mount-all|unmount|normalize\n"
              << "  ksud kagami recovery status|reset\n"
              << "  ksud kagami hide list|apply|add|remove\n"
              << "  ksud kagami kasumi "
                 "version|list|enable|disable|clear|fix-mounts|hide-overlay-xattrs|mount-hide "
                 "off|normal|maps\n"
              << "  ksud kagami kasumi selinux-fix on|off\n";
}

std::string arg_or_default(const std::vector<std::string>& args, std::size_t index,
                           const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}

// Magic/Overlay mounts are boot-only; post-boot controls replay Kasumi mappings only.
bool system_boot_completed() {
    return ksud::getprop("sys.boot_completed") == "1";
}

void print_string_array(const std::vector<std::string>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << json_quote(values[i]);
    }
    std::cout << "]";
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) {
        return line;
    }
    return "";
}

std::string read_file(const fs::path& path) {
    return ksud::read_file(path.string()).value_or("");
}

bool is_builtin_partition(const std::string& name) {
    return std::find(managed_partitions().begin(), managed_partitions().end(), name) !=
           managed_partitions().end();
}

bool valid_module_id(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool valid_module_mode(const std::string& mode) {
    return mode == "auto" || mode == "kasumi" || mode == "overlay" || mode == "magic" ||
           mode == "none" || mode == "hide";
}

bool parse_unsigned_long(const std::string& value, unsigned long& out) {
    try {
        std::size_t parsed = 0;
        const unsigned long result = std::stoul(value, &parsed, 0);
        if (parsed != value.size()) {
            return false;
        }
        out = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool module_has_partition_content(const fs::path& module_path, const std::string& partition) {
    const fs::path root = module_path / partition;
    if (!fs::is_directory(root)) {
        return false;
    }
    try {
        return fs::recursive_directory_iterator(root) != fs::recursive_directory_iterator();
    } catch (...) {
        return false;
    }
}

std::map<std::string, std::string> read_prop_file(const fs::path& path) {
    std::map<std::string, std::string> props;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        props[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return props;
}

std::string kernel_release() {
    const std::string version = read_first_line("/proc/version");
    const std::string marker = "Linux version ";
    const auto start = version.find(marker);
    if (start == std::string::npos) {
        return version.empty() ? "Unknown" : version;
    }
    const auto value_start = start + marker.size();
    const auto value_end = version.find(' ', value_start);
    return version.substr(value_start, value_end - value_start);
}

std::string format_bytes(unsigned long long bytes) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << static_cast<unsigned long long>(value) << units[unit];
    } else {
        out.setf(std::ios::fixed);
        out.precision(value < 10.0 ? 1 : 0);
        out << value << units[unit];
    }
    return out.str();
}

bool path_is_read_only_mount(const std::string& mount_point) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream parts(line);
        std::string dev;
        std::string mp;
        std::string fs_type;
        std::string options;
        parts >> dev >> mp >> fs_type >> options;
        if (mp == mount_point) {
            return options.find("ro") != std::string::npos;
        }
    }
    return false;
}
}  // namespace

struct MountEntry {
    std::string mount_point;
    std::string fstype;
    std::string source;
};

// Parse /proc/self/mountinfo into (mount_point, fstype, source) tuples.
namespace {
std::vector<MountEntry> read_mountinfo() {
    std::vector<MountEntry> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string tok;
        while (pre >> tok) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        MountEntry e;
        e.mount_point = mount::fsutil::decode_mount_path(f[4]);
        post >> e.fstype >> e.source;
        e.source = mount::fsutil::decode_mount_path(e.source);
        out.push_back(e);
    }
    return out;
}

Config current_config() {
    Config config;
    std::string error;
    read_config_file(config, error);  // struct defaults on miss
    return config;
}

int print_storage_json() {
    const Config config = current_config();
    const std::string mirror_base = mount::storage::current_mirror_dir(config);

    // Report the active backend's storage base (tmpfs/ext4/erofs): the shared
    // mirror root if mounted, else the Magic Mount work tmpfs, else fall back
    // to the host /data filesystem. Loop-backed ext4/EROFS uses a loop-device
    // source, so the mirror is identified by mountpoint rather than source.
    std::string mode = "host";
    fs::path target = data_dir();
    for (const auto& m : read_mountinfo()) {
        if (m.mount_point == mirror_base ||
            (m.source == config.mount_source && m.mount_point == config.work_dir)) {
            mode = m.fstype;
            target = m.mount_point;
            break;
        }
    }

    struct statvfs st = {};
    if (statvfs(target.c_str(), &st) != 0) {
        std::cout << "{\"error\":\"not mounted\"}\n";
        return 0;
    }

    const auto total = static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const auto avail = static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;
    const auto used = total > avail ? total - avail : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100ULL) / total) : 0;

    std::cout << "{"
              << "\"size\":" << json_quote(format_bytes(total)) << ","
              << "\"used\":" << json_quote(format_bytes(used)) << ","
              << "\"avail\":" << json_quote(format_bytes(avail)) << ","
              << "\"percent\":" << percent << ","
              << "\"mode\":" << json_quote(mode) << "}\n";
    return 0;
}

int print_partitions_json() {
    std::cout << "[";
    for (std::size_t i = 0; i < managed_partitions().size(); ++i) {
        const auto& name = managed_partitions()[i];
        const auto mount_point = partition_mount_point(name);
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"name\":" << json_quote(name) << ","
                  << "\"mount_point\":" << json_quote(mount_point) << ","
                  << "\"fs_type\":\"\","
                  << "\"is_read_only\":"
                  << (path_is_read_only_mount(mount_point) ? "true" : "false") << ","
                  << "\"exists_as_symlink\":" << (fs::is_symlink(mount_point) ? "true" : "false")
                  << "}";
    }
    std::cout << "]";
    return 0;
}

int print_features_json(int bitmask) {
    const auto names = kasumi::feature_names(bitmask);
    std::cout << "{\"bitmask\":" << bitmask << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
    return 0;
}

int print_kasumi_snapshot_json() {
    const auto version = kasumi::version_info();
    const bool available = version.status == kasumi::Status::Available;
    const int bitmask = available ? kasumi::features() : 0;
    std::cout << "{\"transport\":\"ksu-fd\",\"available\":" << (available ? "true" : "false")
              << ",\"status\":" << static_cast<int>(version.status)
              << ",\"expected_protocol\":" << version.expected_protocol
              << ",\"kernel_protocol\":" << version.kernel_protocol
              << ",\"last_errno\":" << version.last_errno
              << ",\"enabled\":" << (available && kasumi::enabled_state() > 0 ? "true" : "false")
              << ",\"features\":";
    print_features_json(bitmask);
    std::cout << ",\"hooks\":" << json_quote(available ? kasumi::hooks() : "") << "}\n";
    return 0;
}

int apply_config_file(bool force_enable = false) {
    Config config;
    std::string error;
    if (!read_config_file(config, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (force_enable) {
        config.kasumi_enabled = true;
    }
    if (!config.kasumi_enabled) {
        if (kasumi::is_available() && !mount::kasumi::deactivate(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (!kasumi::is_available()) {
        std::cerr << "Kasumi is unavailable\n";
        return 1;
    }

    // Rebuild module mappings only when this boot already established that
    // Kasumi owns them. A first post-boot load may follow an Overlay/Magic
    // fallback and must not migrate or stack backends underneath live mounts.
    if (mount::kasumi::has_replayable_mappings() && !mount::kasumi::is_active()) {
        if (!mount::refresh_kasumi_modules(config)) {
            std::cerr << "failed to replay persisted Kasumi configuration\n";
            return 1;
        }
        return print_kasumi_snapshot_json();
    }

    if (!kasumi::set_enabled(false)) {
        error = std::string("failed to disable Kasumi: ") + std::strerror(errno);
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << error << "\n";
        return 1;
    }
    if (!mount::kasumi::apply_feature_config(config, error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << error << "\n";
        return 1;
    }
    if (!mount::fsutil::run_in_init_mount_ns(
            [&]() { return mount::overlay::restore_xattr_hiding(config); })) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << "failed to restore OverlayFS xattr hiding\n";
        return 1;
    }
    if (!kasumi::set_enabled(true)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << "failed to enable Kasumi after config apply\n";
        return 1;
    }
    (void)mount::kasumi::restore_persisted_hide_rules(error);
    return print_kasumi_snapshot_json();
}

void print_backend_statuses_json() {
    const auto statuses = mount::backend_statuses();
    std::cout << "[";
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto& status = statuses[i];
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"kind\":" << json_quote(mount::backend_kind_name(status.kind)) << ","
                  << "\"name\":" << json_quote(status.name) << ","
                  << "\"available\":" << (status.available ? "true" : "false") << ","
                  << "\"preferred\":" << (status.preferred ? "true" : "false") << ","
                  << "\"detail\":" << json_quote(status.detail) << "}";
    }
    std::cout << "]";
}

int print_system_json() {
    const auto version = kasumi::version_info();
    const int bitmask = version.status == kasumi::Status::Available ? kasumi::features() : 0;
    const std::string hook_text =
        version.status == kasumi::Status::Available ? kasumi::hooks() : "";

    // Count our live mounts (our mount source) for the stats panel.
    const Config config = current_config();
    auto owned_paths = mount::magic::active_mounts(config);
    const auto overlay_paths = mount::overlay::active_mounts(config);
    owned_paths.insert(owned_paths.end(), overlay_paths.begin(), overlay_paths.end());
    const std::set<std::string> owned(owned_paths.begin(), owned_paths.end());
    int total_mounts = 0;
    int overlay_mounts = 0;
    for (const auto& m : read_mountinfo()) {
        if (m.source != config.mount_source && !owned.count(m.mount_point)) {
            continue;
        }
        ++total_mounts;
        if (m.fstype == "overlay") {
            ++overlay_mounts;
        }
    }

    std::cout << "{"
              << "\"kernel\":" << json_quote(kernel_release()) << ","
              << "\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config)) << ","
              << "\"kasumi_available\":"
              << (version.status == kasumi::Status::Available ? "true" : "false") << ","
              << "\"kasumi_status\":" << static_cast<int>(version.status) << ","
              << "\"hooks\":" << json_quote(hook_text) << ","
              << "\"features\":";
    print_features_json(bitmask);
    std::cout << ",\"mountStats\":{\"total_mounts\":" << total_mounts
              << ",\"successful_mounts\":" << total_mounts
              << ",\"failed_mounts\":0,\"tmpfs_created\":0,\"files_mounted\":0,\"dirs_mounted\":0,"
              << "\"symlinks_created\":0,\"overlayfs_mounts\":" << overlay_mounts
              << ",\"success_rate\":" << (total_mounts > 0 ? 100 : 0) << "},"
              << "\"detectedPartitions\":";
    print_partitions_json();
    std::cout << ",\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

int print_meta_json(bool include_backends = true) {
    std::cout << "{"
              << "\"embedded\":true,\"native_control\":true,\"external_mount_owner\":"
              << json_quote(embedded_external_mount_owner()) << ","
              << "\"version\":" << json_quote(ksud::VERSION_NAME) << ","
              << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
              << "\"modules_dir\":" << json_quote(runtime_modules_dir().string()) << ","
              << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
              << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
              << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
              << "\"log_file\":" << json_quote(runtime_log_file().string()) << ","
              << "\"backends\":";
    if (include_backends)
        print_backend_statuses_json();
    else
        std::cout << "[]";
    std::cout << "}\n";
    return 0;
}

int print_mount_status_json() {
    const Config config = current_config();
    auto owned_paths = mount::magic::active_mounts(config);
    const auto overlay_paths = mount::overlay::active_mounts(config);
    owned_paths.insert(owned_paths.end(), overlay_paths.begin(), overlay_paths.end());
    const std::set<std::string> owned(owned_paths.begin(), owned_paths.end());
    int total = 0;
    int overlays = 0;
    std::vector<std::string> active;
    for (const auto& mount : read_mountinfo()) {
        if (mount.source != config.mount_source && !owned.count(mount.mount_point))
            continue;
        ++total;
        active.push_back(mount.mount_point);
        if (mount.fstype == "overlay")
            ++overlays;
    }
    std::cout << "{\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config))
              << ",\"mountStats\":{\"total_mounts\":" << total
              << ",\"overlayfs_mounts\":" << overlays << "},\"active_mounts\":";
    print_string_array(active);
    std::cout << ",\"detectedPartitions\":";
    print_partitions_json();
    std::cout << "}\n";
    return 0;
}

int print_kasumi_version_json() {
    const auto version = kasumi::version_info();
    const std::string rules =
        version.status == kasumi::Status::Available ? kasumi::active_rules() : "";
    const auto modules = kasumi::active_modules_from_rules(rules);
    const bool mismatch = version.status == kasumi::Status::KernelTooOld ||
                          version.status == kasumi::Status::ClientTooOld;

    const Config config = current_config();
    std::cout << "{"
              << "\"backend\":\"kasumi\","
              << "\"protocol_version\":" << version.expected_protocol << ","
              << "\"kernel_version\":" << version.kernel_protocol << ","
              << "\"kasumi_available\":"
              << (version.status == kasumi::Status::Available ? "true" : "false") << ","
              << "\"protocol_mismatch\":" << (mismatch ? "true" : "false") << ","
              << "\"mismatch_message\":" << json_quote(mismatch ? "Kasumi protocol mismatch" : "")
              << ","
              << "\"active_modules\":";
    print_string_array(modules);
    std::cout << ",\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config))
              << "}\n";
    return 0;
}

int print_kasumi_rules_json() {
    const std::string rules = kasumi::active_rules();
    std::istringstream lines(rules);
    std::string line;
    bool first = true;

    std::cout << "[";
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parts(line);
        std::string type;
        parts >> type;
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (!first) {
            std::cout << ",";
        }
        first = false;

        std::cout << "{\"type\":" << json_quote(type);
        if (type == "ADD" || type == "MERGE") {
            std::string target;
            std::string source;
            parts >> target >> source;
            std::cout << ",\"target\":" << json_quote(target)
                      << ",\"source\":" << json_quote(source);
        } else if (type == "HIDE") {
            std::string path;
            parts >> path;
            std::cout << ",\"path\":" << json_quote(path);
        } else {
            std::string rest;
            std::getline(parts, rest);
            if (!rest.empty() && rest[0] == ' ') {
                rest.erase(0, 1);
            }
            std::cout << ",\"args\":" << json_quote(rest);
        }
        std::cout << "}";
    }
    std::cout << "]\n";
    return 0;
}

int handle_config(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "show") {
        std::string config = read_file(config_file());
        if (config.empty()) {
            config = default_config_json();
        }
        JsonValue stored;
        std::string parse_error;
        if (!parse_json(config, stored, parse_error) || !stored.is_object()) {
            std::cerr << "invalid stored config: " << parse_error << "\n";
            return 1;
        }
        prune_config_fields(stored);
        std::cout << stringify_json(stored, 2);
        return 0;
    }
    if (sub == "gen") {
        std::string error;
        if (args.size() != 2) {
            std::cerr << "usage: ksud kagami config gen\n";
            return 1;
        }
        if (!write_default_config(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "merge-json") {
        if (args.size() != 3) {
            std::cerr << "usage: ksud kagami config merge-json JSON\n";
            return 1;
        }
        std::string error;
        if (!merge_config_json(args[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "apply") {
        if (args.size() != 2) {
            std::cerr << "usage: ksud kagami config apply\n";
            return 1;
        }
        return apply_config_file();
    }
    if (sub == "sync-partitions") {
        std::set<std::string> partitions;
        const fs::path module_root = modules_dir();
        if (fs::is_directory(module_root)) {
            for (const auto& module : fs::directory_iterator(module_root)) {
                if (!module.is_directory()) {
                    continue;
                }
                for (const auto& child : fs::directory_iterator(module.path())) {
                    if (child.is_directory()) {
                        const std::string name = child.path().filename().string();
                        if (!is_builtin_partition(name) &&
                            module_has_partition_content(module.path(), name)) {
                            partitions.insert(name);
                        }
                    }
                }
            }
        }
        if (partitions.empty()) {
            std::cout << "No new partitions\n";
        } else {
            for (const auto& partition : partitions) {
                std::cout << "Added partition: " << partition << "\n";
            }
        }
        return 0;
    }
    print_usage();
    return 1;
}

int handle_api(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "mounts")
        return print_mount_status_json();
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "system") {
        return print_system_json();
    }
    if (sub == "storage") {
        return print_storage_json();
    }
    if (sub == "kasumi") {
        return print_kasumi_snapshot_json();
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features()
                                                                               : 0);
    }
    if (sub == "hooks") {
        if (!kasumi::is_available()) {
            std::cerr << "Kasumi not available.\n";
            return 1;
        }
        std::cout << kasumi::hooks() << "\n";
        return 0;
    }
    if (sub == "backends") {
        print_backend_statuses_json();
        std::cout << "\n";
        return 0;
    }
    if (sub == "meta") {
        return print_meta_json(args.size() != 3 || args[2] != "--control-only");
    }
    print_usage();
    return 1;
}

bool save_bypass(const std::string& key, bool enabled, const char* file) {
    auto values = mount::load_module_modes(file);
    if (enabled)
        values[key] = "on";
    else
        values.erase(key);
    return mount::save_module_modes(values, file);
}

int handle_module(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "apply") {
        const auto id = arg_or_default(args, 2, "");
        if (args.size() != 3 || !valid_module_id(id))
            return 1;
        Config config;
        std::string error;
        if (!read_config_file(config, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto replayable = mount::kasumi::replayable_module_ids();
        const auto modules = mount::enumerate_mountable_modules();
        const bool applicable =
            config.kasumi_enabled &&
            std::find(replayable.begin(), replayable.end(), id) != replayable.end() &&
            std::any_of(modules.begin(), modules.end(),
                        [&](const auto& module) { return module.id == id; });
        if (applicable && !mount::refresh_kasumi_modules(config)) {
            std::cerr << "settings saved, but Kasumi rule application failed\n";
            return 1;
        }
        std::cout << "{\"applied\":" << (applicable ? "true" : "false") << "}\n";
        return 0;
    }
    if (sub == "list") {
        const auto modes = mount::load_module_modes();
        const auto bypasses = mount::load_module_modes("module_bypass.json");
        const bool include_disabled = args.size() == 3 && args[2] == "--all";
        const auto rule_map = mount::load_module_rules();
        const auto replayable = mount::kasumi::replayable_module_ids();
        const std::set<std::string> kasumi_boot_plan(replayable.begin(), replayable.end());
        const Config cfg = current_config();
        const fs::path module_root = modules_dir();
        std::cout << "{\"modules\":[";
        bool first = true;
        if (fs::is_directory(module_root)) {
            for (const auto& entry : fs::directory_iterator(module_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                // Skip modules that opt out of metamodule mounting.
                std::error_code mec;
                if ((!include_disabled && fs::exists(entry.path() / "disable", mec)) ||
                    fs::exists(entry.path() / "remove", mec) ||
                    fs::exists(entry.path() / "skip_mount", mec) ||
                    (!include_disabled &&
                     fs::exists(data_dir() / "run" / "hot_unmounted" / entry.path().filename(),
                                mec)) ||
                    !fs::exists(entry.path() / "module.prop", mec)) {
                    continue;
                }
                // Only list modules that contribute mounts (have a managed
                // partition tree); skip plain modules (zygisk, etc.).
                bool has_mount_content = false;
                const std::vector<std::string>& parts =
                    cfg.partitions.empty() ? mount::fsutil::managed_partitions() : cfg.partitions;
                for (const auto& part : parts) {
                    std::error_code ec;
                    if (fs::is_directory(entry.path() / part, ec)) {
                        has_mount_content = true;
                        break;
                    }
                }
                if (!has_mount_content) {
                    continue;
                }
                const std::string id = entry.path().filename().string();
                const auto props = read_prop_file(entry.path() / "module.prop");
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                const auto mode_it = modes.find(id);
                const std::string mode = mode_it == modes.end() ? "auto" : mode_it->second;
                const auto rules_it = rule_map.find(id);
                // The same-boot plan is authoritative for Kasumi ownership.
                // Without it, an explicit Kasumi choice may already be running
                // through the boot-time Overlay/Magic fallback.
                std::string strategy;
                if (kasumi_boot_plan.count(id) != 0) {
                    strategy = "kasumi";
                } else {
                    Config fallback_config = cfg;
                    fallback_config.kasumi_enabled = false;
                    strategy = mount::resolve_module_backend(mount::ModuleEntry{id, entry.path()},
                                                             fallback_config, modes);
                }
                std::cout << "{"
                          << "\"id\":" << json_quote(id) << ","
                          << "\"name\":" << json_quote(props.count("name") ? props.at("name") : id)
                          << ","
                          << "\"version\":"
                          << json_quote(props.count("version") ? props.at("version") : "") << ","
                          << "\"author\":"
                          << json_quote(props.count("author") ? props.at("author") : "") << ","
                          << "\"description\":"
                          << json_quote(props.count("description") ? props.at("description") : "")
                          << ","
                          << "\"mode\":" << json_quote(mode) << ","
                          << "\"bypass\":"
                          << ((bypasses.count(id) && bypasses.at(id) == "on") ? "true" : "false")
                          << ","
                          << "\"strategy\":" << json_quote(strategy) << ","
                          << "\"path\":" << json_quote(entry.path().string()) << ","
                          << "\"rules\":[";
                if (rules_it != rule_map.end()) {
                    for (std::size_t i = 0; i < rules_it->second.size(); ++i) {
                        if (i > 0) {
                            std::cout << ",";
                        }
                        std::cout << "{\"path\":" << json_quote(rules_it->second[i].path)
                                  << ",\"mode\":" << json_quote(rules_it->second[i].mode)
                                  << ",\"bypass\":"
                                  << (rules_it->second[i].bypass ? "true}" : "false}");
                    }
                }
                std::cout << "]}";
            }
        }
        std::cout << "]}\n";
        return 0;
    }
    if (sub == "set-bypass") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string value = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || args.size() != 4 || (value != "on" && value != "off"))
            return 1;
        if (value == "on" && !kasumi::rule_flags_supported(KSM_RULE_BYPASS_POLICY)) {
            std::cerr << "bypass requires a kernel with rule_bypass support\n";
            return 1;
        }
        if (!save_bypass(id, value == "on", "module_bypass.json"))
            return 1;
        return 0;
    }
    if (sub == "set-mode") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string mode = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || (mode != "auto" && mode != "kasumi" && mode != "overlay" &&
                                     mode != "magic" && mode != "none")) {
            std::cerr << "usage: ksud kagami module set-mode <id> auto|kasumi|overlay|magic|none\n";
            return 1;
        }
        auto modes = mount::load_module_modes();
        if (mode == "auto") {
            modes.erase(id);
        } else {
            modes[id] = mode;
        }
        if (!mount::save_module_modes(modes)) {
            std::cerr << "failed to save module modes\n";
            return 1;
        }
        return 0;
    }
    if (sub == "add-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        const std::string mode = arg_or_default(args, 4, "");
        const bool bypass = args.size() == 6 && args[5] == "--bypass";
        if ((args.size() != 5 && !bypass) ||
            (bypass && !kasumi::rule_flags_supported(KSM_RULE_BYPASS_POLICY))) {
            std::cerr << "bypass requires a kernel with rule_bypass support\n";
            return 1;
        }
        if (!valid_module_id(id) || path.empty() || path.front() != '/' ||
            !valid_module_mode(mode)) {
            std::cerr << "usage: ksud kagami module add-rule <id> <absolute-path> "
                         "kasumi|overlay|magic|none|hide [--bypass]\n";
            return 1;
        }
        auto rules = mount::load_module_rules();
        auto& module_rules = rules[id];
        const auto existing = std::find_if(module_rules.begin(), module_rules.end(),
                                           [&](const auto& rule) { return rule.path == path; });
        if (existing == module_rules.end()) {
            module_rules.push_back({path, mode, bypass});
        } else {
            existing->mode = mode;
            existing->bypass = bypass;
        }
        if (!mount::save_module_rules(rules)) {
            std::cerr << "failed to save module rules\n";
            return 1;
        }
        return 0;
    }
    if (sub == "remove-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || path.empty() || path.front() != '/') {
            std::cerr << "usage: ksud kagami module remove-rule <id> <absolute-path>\n";
            return 1;
        }
        auto rules = mount::load_module_rules();
        const auto rules_it = rules.find(id);
        if (rules_it == rules.end()) {
            return 0;
        }
        auto& module_rules = rules_it->second;
        module_rules.erase(std::remove_if(module_rules.begin(), module_rules.end(),
                                          [&](const auto& rule) { return rule.path == path; }),
                           module_rules.end());
        if (module_rules.empty()) {
            rules.erase(rules_it);
        }
        if (!mount::save_module_rules(rules)) {
            std::cerr << "failed to save module rules\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hot-mount" || sub == "hot-unmount" || sub == "add" || sub == "delete") {
        const std::string id = arg_or_default(args, 2, "");
        if (!valid_module_id(id)) {
            std::cerr << "usage: ksud kagami module " << sub << " <id>\n";
            return 1;
        }
        const Config cfg = current_config();
        const fs::path module_root = modules_dir();
        const fs::path module_path = module_root / id;
        if (!fs::is_directory(module_path) || !fs::exists(module_path / "module.prop")) {
            std::cerr << "module not found: " << id << "\n";
            return 1;
        }
        const auto replayable = mount::kasumi::replayable_module_ids();
        if (std::find(replayable.begin(), replayable.end(), id) == replayable.end()) {
            std::cerr
                << "module was not assigned to Kasumi this boot; reboot to change its backend\n";
            return 1;
        }
        const fs::path marker = data_dir() / "run" / "hot_unmounted" / id;
        std::error_code ec;
        const bool unmounting = sub == "hot-unmount" || sub == "delete";
        const bool existed = fs::exists(marker, ec);
        fs::create_directories(marker.parent_path(), ec);
        if (ec) {
            std::cerr << "failed to prepare hot-mount state: " << ec.message() << "\n";
            return 1;
        }
        if (unmounting) {
            std::ofstream(marker, std::ios::trunc).put('\n');
        } else {
            fs::remove(marker, ec);
        }
        if (!mount::refresh_kasumi_modules(cfg)) {
            if (unmounting && !existed) {
                fs::remove(marker, ec);
            } else if (!unmounting && existed) {
                std::ofstream(marker, std::ios::trunc).put('\n');
            }
            std::cerr << "failed to refresh Kasumi mappings\n";
            return 1;
        }
        std::cout << "{\"ok\":true,\"module\":" << json_quote(id)
                  << ",\"action\":" << json_quote(sub) << "}\n";
        return 0;
    }
    if (sub == "check-conflicts") {
        const Config cfg = current_config();
        const auto modules = mount::enumerate_mountable_modules();
        const std::vector<std::string>& parts =
            cfg.partitions.empty() ? mount::fsutil::managed_partitions() : cfg.partitions;
        std::map<std::string, std::vector<std::string>> owners;
        for (const auto& module : modules) {
            for (const auto& part : parts) {
                const fs::path root = module.path / part;
                std::error_code ec;
                auto it = fs::recursive_directory_iterator(root, ec);
                const auto end = fs::recursive_directory_iterator();
                for (; it != end && !ec; it.increment(ec)) {
                    if (it->is_regular_file(ec) || it->is_symlink(ec)) {
                        owners[(fs::path("/") / part / it->path().lexically_relative(root))
                                   .string()]
                            .push_back(module.id);
                    }
                }
            }
        }
        std::cout << "[";
        bool first = true;
        for (const auto& [path, ids] : owners) {
            if (ids.size() < 2) {
                continue;
            }
            if (!first) {
                std::cout << ",";
            }
            first = false;
            std::cout << "{\"file\":" << json_quote(path) << ",\"modules\":";
            print_string_array(ids);
            std::cout << "}";
        }
        std::cout << "]\n";
        return 0;
    }
    if (sub == "mount-all") {
        // Boot-only: mounting over live service namespaces is unsafe.
        if (system_boot_completed()) {
            std::cerr << "refusing module mount-all: magic mount only runs at boot via "
                         "ksud; post-boot mounting breaks namespaces\n";
            return 1;
        }
        Config config;
        std::string cfg_err;
        read_config_file(config, cfg_err);  // defaults on error
        const auto report = mount::mount_all_enabled(config);
        std::cout << "{"
                  << "\"ok\":" << (report.ok ? "true" : "false") << ","
                  << "\"backend\":" << json_quote(report.backend) << ","
                  << "\"modules\":" << report.modules << ","
                  << "\"mounts\":" << report.mounts << ","
                  << "\"detail\":" << json_quote(report.detail) << "}\n";
        return report.ok ? 0 : 1;
    }
    if (sub == "normalize") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty()) {
            std::cerr << "usage: ksud kagami module normalize <module_path>\n";
            return 1;
        }
        const bool ok = mount::magic::normalize_module(path);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"module\":" << json_quote(path)
                  << "}\n";
        return ok ? 0 : 1;
    }
    if (sub == "unmount") {
        // Tear down Kagami's own mounts (source-gated; never touches real partitions).
        Config config;
        std::string cfg_err;
        read_config_file(config, cfg_err);  // defaults on error
        const bool ok = mount::unmount_all(config);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << "}\n";
        return ok ? 0 : 1;
    }
    print_usage();
    return 1;
}

int handle_kasumi(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "version") {
        return print_kasumi_version_json();
    }
    if (sub == "list") {
        return print_kasumi_rules_json();
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features()
                                                                               : 0);
    }
    if (sub == "maps") {
        const std::string op = arg_or_default(args, 2, "");
        if (op == "clear") {
            if (!kasumi::clear_maps_rules()) {
                std::cerr << "failed to clear Kasumi maps rules\n";
                return 1;
            }
            return 0;
        }
        if (op == "add") {
            if (args.size() < 8) {
                std::cerr << "usage: ksud kagami kasumi maps add <target-ino> <target-dev> "
                             "<spoof-ino> <spoof-dev> <spoof-path>\n";
                return 1;
            }
            unsigned long target_ino = 0;
            unsigned long target_dev = 0;
            unsigned long spoof_ino = 0;
            unsigned long spoof_dev = 0;
            if (!parse_unsigned_long(args[3], target_ino) ||
                !parse_unsigned_long(args[4], target_dev) ||
                !parse_unsigned_long(args[5], spoof_ino) ||
                !parse_unsigned_long(args[6], spoof_dev)) {
                std::cerr << "Kasumi maps values must be unsigned integers\n";
                return 1;
            }
            if (!kasumi::add_maps_rule(target_ino, target_dev, spoof_ino, spoof_dev, args[7])) {
                std::cerr << "failed to add Kasumi maps rule\n";
                return 1;
            }
            return 0;
        }
        std::cerr << "usage: ksud kagami kasumi maps clear|add ...\n";
        return 1;
    }
    if (sub == "enable") {
        return apply_config_file(true);
    }
    if (sub == "disable") {
        if (!kasumi::set_enabled(false)) {
            std::cerr << "failed to set Kasumi enabled state\n";
            return 1;
        }
        return 0;
    }
    if (sub == "clear") {
        if (!kasumi::clear_rules()) {
            std::cerr << "failed to clear Kasumi rules\n";
            return 1;
        }
        mount::kasumi::invalidate_active_state();
        return 0;
    }
    if (sub == "hide-path" || sub == "delete-rule") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "Kasumi path must be absolute\n";
            return 1;
        }
        const bool ok = sub == "hide-path" ? kasumi::hide_path(path) : kasumi::delete_rule(path);
        if (!ok) {
            std::cerr << "failed to update Kasumi path rule\n";
            return 1;
        }
        return 0;
    }
    if (sub == "fix-mounts") {
        if (!kasumi::fix_mounts()) {
            std::cerr << "failed to reorder Kasumi mount ids\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hide-overlay-xattrs") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "kasumi hide-overlay-xattrs requires an absolute path\n";
            return 1;
        }
        if (!kasumi::hide_overlay_xattrs(path)) {
            std::cerr << "failed to hide overlay xattrs for " << path << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "mount-hide" || sub == "maps-spoof" || sub == "statfs-spoof" ||
        sub == "selinux-fix") {
        const std::string value = arg_or_default(args, 2, "off");
        const bool on = value == "on" || value == "normal";
        bool ok = false;
        if (sub == "mount-hide") {
            if (value != "off" && value != "on" && value != "normal") {
                std::cerr << "usage: ksud kagami kasumi mount-hide off|on|normal\n";
                return 1;
            }
            ok = kasumi::set_mount_hide(on);
        } else if (sub == "maps-spoof") {
            ok = kasumi::set_maps_spoof(on);
        } else if (sub == "statfs-spoof") {
            ok = kasumi::set_statfs_spoof(on);
        } else if (sub == "selinux-fix") {
            if (value != "off" && value != "on") {
                std::cerr << "usage: ksud kagami kasumi selinux-fix on|off\n";
                return 1;
            }
            ok = kasumi::set_selinux_fix(on);
        }
        if (!ok) {
            std::cerr << "failed to set Kasumi " << sub << "\n";
            return 1;
        }
        return 0;
    }
    print_usage();
    return 1;
}

int handle_hide(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "apply") {
        Config config;
        std::string error;
        if (!read_config_file(config, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (config.kasumi_enabled && !mount::kasumi::restore_persisted_hide_rules(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "list") {
        const auto rules = load_user_hide_rules();
        if (args.size() == 3 && args[2] == "--details") {
            const auto bypasses = mount::load_module_modes("hide_bypass.json");
            std::cout << "[";
            bool first = true;
            for (const auto& path : rules) {
                if (!first)
                    std::cout << ",";
                first = false;
                std::cout << "{\"path\":" << json_quote(path) << ",\"bypass\":"
                          << (bypasses.count(path) && bypasses.at(path) == "on" ? "true}"
                                                                                : "false}");
            }
            std::cout << "]\n";
            return 0;
        }
        print_string_array(rules);
        std::cout << "\n";
        return 0;
    }
    if (sub == "add") {
        const std::string path = arg_or_default(args, 2, "");
        const bool bypass = args.size() == 4 && args[3] == "--bypass";
        if ((args.size() != 3 && !bypass) ||
            (bypass && !kasumi::rule_flags_supported(KSM_RULE_BYPASS_POLICY))) {
            std::cerr << "bypass requires a kernel with rule_bypass support\n";
            return 1;
        }
        if (path.empty() || path[0] != '/') {
            std::cerr << "hide path must be absolute\n";
            return 1;
        }
        auto rules = load_user_hide_rules();
        if (std::find(rules.begin(), rules.end(), path) == rules.end()) {
            rules.push_back(path);
        }
        if (!save_bypass(path, bypass, "hide_bypass.json"))
            return 1;
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        bool managed = false;
        if (kasumi::is_available() &&
            (!kasumi::managed_hide_mode(managed) ||
             !(managed ? kasumi::upsert_user_hide(path, nullptr, bypass)
                       : mount::kasumi::update_legacy_user_hide(
                             path, false, bypass ? KSM_RULE_BYPASS_POLICY : 0)))) {
            std::cerr << "hide rule saved, but the kernel rejected it: " << std::strerror(errno)
                      << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "remove") {
        const std::string path = arg_or_default(args, 2, "");
        auto rules = load_user_hide_rules();
        rules.erase(std::remove(rules.begin(), rules.end(), path), rules.end());
        if (!save_bypass(path, false, "hide_bypass.json"))
            return 1;
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        bool managed = false;
        if (kasumi::is_available() &&
            (!kasumi::managed_hide_mode(managed) ||
             !(managed ? kasumi::delete_user_hide(path)
                       : mount::kasumi::update_legacy_user_hide(path, true)))) {
            std::cerr << "hide rule removed from config, but kernel removal failed: "
                      << std::strerror(errno) << "\n";
            return 1;
        }
        return 0;
    }
    print_usage();
    return 1;
}

int handle_recovery(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "status");
    if (sub == "reset") {
        mount::recovery_reset();
        std::cout << "{\"ok\":true,\"action\":\"reset\"}\n";
        return 0;
    }
    if (sub == "status") {
        std::cout << mount::recovery_status_json() << "\n";
        return 0;
    }
    std::cerr << "usage: ksud kagami recovery status|reset\n";
    return 1;
}
}  // namespace

int run_command(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << ksud::VERSION_NAME << "\n";
        return 0;
    }
    if (args[0] == "daemon") {
        return run_daemon_command(args);
    }
    if (args[0] == "config") {
        return handle_config(args);
    }
    if (args[0] == "api") {
        return handle_api(args);
    }
    if (args[0] == "module") {
        return handle_module(args);
    }
    if (args[0] == "kasumi") {
        return handle_kasumi(args);
    }
    if (args[0] == "debug") {
        if (args.size() >= 2 && (args[1] == "enable" || args[1] == "disable")) {
            return kasumi::set_debug(args[1] == "enable") ? 0 : 1;
        }
        if (args.size() >= 3 && args[1] == "stealth") {
            return kasumi::set_stealth(args[2] == "enable") ? 0 : 1;
        }
        return 0;
    }
    if (args[0] == "hide") {
        return handle_hide(args);
    }
    if (args[0] == "recovery") {
        return handle_recovery(args);
    }

    print_usage();
    return 1;
}

namespace {
bool is_query(const std::vector<std::string>& args) {
    if (args.empty())
        return true;
    const auto sub = arg_or_default(args, 1, "");
    return args[0] == "api" || args[0] == "version" || args[0] == "help" ||
           (args[0] == "config" && sub == "show") ||
           (args[0] == "module" && (sub == "list" || sub == "check-conflicts")) ||
           (args[0] == "hide" && sub == "list") ||
           (args[0] == "kasumi" &&
            (sub == "version" || sub == "list" || sub == "features" || sub == "hooks")) ||
           (args[0] == "recovery" && sub == "status");
}

std::string operation_description(const std::vector<std::string>& args) {
    if (args.empty())
        return "help";
    std::string result = args[0] + (args.size() > 1 ? " " + args[1] : "");
    if (args[0] == "config" && args.size() == 3 && args[1] == "merge-json") {
        JsonValue patch;
        std::string error;
        if (parse_json(args[2], patch, error) && patch.is_object()) {
            result += " keys=";
            const std::set<std::string> public_values = {"debug",
                                                         "verbose",
                                                         "kasumi_enabled",
                                                         "builtin_mount_enabled",
                                                         "enable_kernel_debug",
                                                         "enable_stealth",
                                                         "enable_mount_hide",
                                                         "enable_maps_spoof",
                                                         "enable_statfs_spoof",
                                                         "enable_selinux_fix",
                                                         "enable_overlay_xattr_hide",
                                                         "overlayfs_enabled",
                                                         "magic_mount_enabled",
                                                         "fs_type",
                                                         "mount_backend",
                                                         "mountsource"};
            for (const auto& item : patch.o) {
                result += item.first;
                if (public_values.count(item.first))
                    result += "=" + stringify_json(item.second);
                result += ",";
            }
        }
    } else if ((args[0] == "module" || args[0] == "hide") && args.size() > 2) {
        result += " target=" + json_quote(args[2]);
        if (args.size() > 3)
            result += " value=" + json_quote(args[3]);
        if (args.size() > 4)
            result += " mode=" + json_quote(args[4]);
    }
    return result;
}
}  // namespace

CommandResult run_command_capture(const std::vector<std::string>& args, CommandCapture& capture) {
    const auto started = std::chrono::steady_clock::now();
    const auto operation = operation_description(args);
    const auto level = is_query(args) ? logging::Level::Debug : logging::Level::Info;
    logging::write(level, "command", "begin " + operation);
    capture.out.reset();
    capture.err.reset();
    int exit_code;
    int error_number;
    {
        class RestoreStreams {
        public:
            RestoreStreams() = default;
            RestoreStreams(const RestoreStreams&) = delete;
            RestoreStreams& operator=(const RestoreStreams&) = delete;
            RestoreStreams(RestoreStreams&&) = delete;
            RestoreStreams& operator=(RestoreStreams&&) = delete;
            ~RestoreStreams() {
                std::cout.rdbuf(out);
                std::cerr.rdbuf(err);
                std::cout.clear(out_state);
                std::cerr.clear(err_state);
            }

        private:
            std::streambuf* out = std::cout.rdbuf();
            std::streambuf* err = std::cerr.rdbuf();
            std::ios::iostate out_state = std::cout.rdstate();
            std::ios::iostate err_state = std::cerr.rdstate();
        } restore;
        std::cout.rdbuf(&capture.out);
        std::cerr.rdbuf(&capture.err);
        errno = 0;
        try {
            exit_code = run_command(args);
        } catch (const std::exception& error) {
            std::cerr << "Kagami: " << error.what() << '\n';
            errno = EIO;
            exit_code = 1;
        }
        if (capture.out.failed() || capture.err.failed()) {
            errno = ENOBUFS;
            exit_code = 1;
        }
        error_number = exit_code == 0 ? 0 : (errno != 0 ? errno : EIO);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    logging::write(exit_code == 0 ? level : logging::Level::Error, "command",
                   operation + " result=" + (exit_code == 0 ? "ok" : "failed") + " exit=" +
                       std::to_string(exit_code) + " errno=" + std::to_string(error_number) +
                       " elapsed_ms=" + std::to_string(elapsed));
    if (!capture.err.view().empty())
        logging::write(exit_code == 0 ? logging::Level::Warning : logging::Level::Error, "command",
                       operation + ": " + std::string(capture.err.view()));
    if (capture.out.failed() || capture.err.failed())
        return {1, ENOBUFS, {}, "command output exceeds protocol capacity\n"};
    return {exit_code, error_number, capture.out.view(), capture.err.view()};
}

}  // namespace kagami
