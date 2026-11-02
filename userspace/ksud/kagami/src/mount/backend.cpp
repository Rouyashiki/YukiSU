#include "mount/backend.hpp"
#include <iterator>

#include "core/json.hpp"
#include "core/reboot_state.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace kagami::mount {

namespace fs = std::filesystem;

std::string backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Kasumi:
        return "kasumi";
    case BackendKind::Overlayfs:
        return "overlayfs";
    case BackendKind::MagicMount:
        return "magic_mount";
    }
    return "unknown";
}

namespace {
bool proc_filesystems_has(const std::string& name) {
    std::ifstream in("/proc/filesystems");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream parts(line);
        std::string first;
        std::string second;
        parts >> first >> second;
        if (first == name || second == name) {
            return true;
        }
    }
    return false;
}

int count_committed_mounts() {
    return static_cast<int>(magic::active_mounts(Config{}).size());
}

bool apply_kasumi_features(const Config& config) {
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available()) {
        return true;
    }
    std::string error;
    if (kasumi::apply_feature_config(config, error)) {
        return true;
    }
    fsutil::mlog("kasumi: feature apply failed: " + error, logging::Level::Error);
    return false;
}
}  // namespace

// --- bootloop protection state ---
constexpr int kMaxBootAttempts = 3;

namespace {
fs::path recovery_attempts_file() {
    return runtime_data_dir() / "run" / "boot_attempts";
}
fs::path recovery_disabled_file() {
    return runtime_data_dir() / "run" / "mount_disabled";
}
fs::path mount_orchestrator_file() {
    return runtime_data_dir() / "run" / "mount_orchestrator_boot";
}

bool claim_mount_orchestrator() {
    return !reboot::marker_matches_cycle(mount_orchestrator_file()) &&
           reboot::write_cycle_marker(mount_orchestrator_file(), "running\n");
}

int read_boot_attempts() {
    std::ifstream in(recovery_attempts_file());
    int n = 0;
    in >> n;
    return n > 0 ? n : 0;
}

void write_boot_attempts(int n) {
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    std::ofstream out(recovery_attempts_file(), std::ios::trunc);
    out << n << "\n";
}
}  // namespace

std::vector<BackendStatus> backend_statuses() {
    const auto version = ::kagami::kasumi::version_info();
    const bool kasumi_available = version.status == ::kagami::kasumi::Status::Available;
    std::ostringstream kasumi_detail;
    kasumi_detail << "protocol expected=" << version.expected_protocol
                  << " kernel=" << version.kernel_protocol << " uid=" << version.process_uid
                  << " euid=" << version.process_euid
                  << " status=" << static_cast<int>(version.status);

    const bool overlayfs_available = proc_filesystems_has("overlay");

    const int magic_mounts = count_committed_mounts();
    std::ostringstream magic_detail;
    magic_detail << "Magisk-style systemless mount; live mounts=" << magic_mounts;

    return {
        {BackendKind::Kasumi, "Kasumi", kasumi_available, true, kasumi_detail.str()},
        {BackendKind::Overlayfs, "OverlayFS", overlayfs_available, false,
         overlayfs_available ? "overlay filesystem registered"
                             : "overlay filesystem is not registered"},
        {BackendKind::MagicMount, "Magic Mount", true, false, magic_detail.str()},
    };
}

std::vector<ModuleEntry> enumerate_mountable_modules() {
    std::vector<ModuleEntry> out;
    const fs::path root = runtime_modules_dir();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return out;
    }
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        const fs::path& p = e.path();
        if (!fs::exists(p / "module.prop", ec)) {
            continue;
        }
        if (fs::exists(p / "disable", ec) || fs::exists(p / "remove", ec) ||
            fs::exists(p / "skip_mount", ec) ||
            fs::exists(runtime_data_dir() / "run" / "hot_unmounted" / p.filename(), ec)) {
            continue;
        }
        out.push_back({p.filename().string(), p});
    }
    std::sort(out.begin(), out.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) { return a.id < b.id; });
    return out;
}

// Per-module control-plane state is separate from module files so the manager
// can select backends without modifying another module's contents.
ModuleModeMap load_module_modes(const char* file) {
    ModuleModeMap out;
    std::ifstream in((runtime_data_dir() / file).string());
    if (!in) {
        return out;
    }
    std::stringstream buf;
    std::copy(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(),
              std::ostreambuf_iterator<char>(buf));
    JsonValue root;
    std::string error;
    if (parse_json(buf.str(), root, error) && root.is_object()) {
        for (const auto& [id, value] : root.o) {
            if (value.is_string()) {
                out[id] = value.s;
            }
        }
    }
    return out;
}

bool save_module_modes(const ModuleModeMap& modes, const char* file) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    for (const auto& [id, mode] : modes)
        root.o[id] = JsonValue(mode);
    return ksud::write_file_atomic(runtime_data_dir() / file, stringify_json(root, 2) + "\n");
}

ModuleRuleMap load_module_rules() {
    ModuleRuleMap out;
    std::ifstream in(runtime_data_dir() / "module_rules.json");
    if (!in) {
        return out;
    }
    std::stringstream buf;
    std::copy(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(),
              std::ostreambuf_iterator<char>(buf));
    JsonValue root;
    std::string error;
    if (!parse_json(buf.str(), root, error) || !root.is_object()) {
        return out;
    }
    for (const auto& [id, entries] : root.o) {
        if (!entries.is_array()) {
            continue;
        }
        for (const auto& entry : entries.a) {
            if (!entry.is_object()) {
                continue;
            }
            const JsonValue* path = entry.find("path");
            const JsonValue* mode = entry.find("mode");
            if (path && mode && path->is_string() && mode->is_string()) {
                const auto* bypass = entry.find("bypass");
                out[id].push_back({path->s, mode->s, bypass && bypass->is_bool() && bypass->b});
            }
        }
    }
    return out;
}

bool save_module_rules(const ModuleRuleMap& rules) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    for (const auto& [id, entries] : rules) {
        auto& array = root.o[id];
        array.type = JsonValue::Type::Array;
        for (const auto& entry : entries) {
            JsonValue rule;
            rule.type = JsonValue::Type::Object;
            rule.o["path"] = JsonValue(entry.path);
            rule.o["mode"] = JsonValue(entry.mode);
            rule.o["bypass"] = JsonValue(entry.bypass);
            array.a.push_back(std::move(rule));
        }
    }
    return ksud::write_file_atomic(runtime_data_dir() / "module_rules.json",
                                   stringify_json(root, 2) + "\n");
}

namespace {
bool dir_has_direct_files(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) {
            return true;  // a non-dir entry sits directly at this level
        }
    }
    return false;
}

// A module needs magic mount when it places files directly at a partition root
// (e.g. system/build.prop): overlay only stacks on leaf subdirs, never on a
// partition root. Everything else can be overlaid.
std::vector<std::string> managed_partitions(const Config& config) {
    std::vector<std::string> parts = fsutil::managed_partitions();
    for (const auto& part : config.partitions) {
        if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) {
            parts.push_back(part);
        }
    }
    return parts;
}

bool module_needs_magic(const ModuleEntry& m, const Config& config) {
    if (dir_has_direct_files(m.path / "system")) {
        return true;
    }
    const auto parts = managed_partitions(config);
    return std::any_of(parts.begin(), parts.end(), [&](const auto& part) {
        return part != "system" && (dir_has_direct_files(m.path / part) ||
                                    dir_has_direct_files(m.path / "system" / part));
    });
}

// True if the module has any managed-partition tree (i.e. contributes mounts).
bool module_has_content(const ModuleEntry& m, const Config& config) {
    for (const auto& part : managed_partitions(config)) {
        std::error_code ec;
        if (fs::is_directory(m.path / part, ec)) {
            return true;
        }
    }
    return false;
}

bool kasumi_usable() {
    const auto version = ::kagami::kasumi::version_info();
    return version.status == ::kagami::kasumi::Status::Available;
}

std::string fallback_backend(const ModuleEntry& m, const Config& config) {
    if (config.overlayfs_enabled && proc_filesystems_has("overlay") &&
        !module_needs_magic(m, config)) {
        return "overlay";
    }
    return config.magic_mount_enabled ? "magic" : "none";
}
}  // namespace

std::string resolve_module_backend(const ModuleEntry& m, const Config& config,
                                   const ModuleModeMap& modes) {
    if (!module_has_content(m, config)) {
        return "none";  // no managed-partition tree → contributes no mounts
    }
    std::string mode = "auto";
    const std::string& global = config.mount_backend;
    if (!global.empty() && global != "auto") {
        mode = global;  // global override forces every module
    } else {
        const auto it = modes.find(m.id);
        if (it != modes.end() && !it->second.empty()) {
            mode = it->second;
        }
    }
    // Kasumi is deliberately opt-in. `auto` preserves Kagami's established
    // OverlayFS -> Magic Mount fallback path; only an explicit module/global
    // "kasumi" selection reaches the LKM backend.
    if (mode == "kasumi") {
        return config.kasumi_enabled && kasumi_usable() ? "kasumi" : fallback_backend(m, config);
    }
    if (mode == "auto") {
        return fallback_backend(m, config);
    }
    if (mode == "overlay") {
        return config.overlayfs_enabled && proc_filesystems_has("overlay")
                   ? "overlay"
                   : fallback_backend(m, config);
    }
    if (mode == "magic") {
        return config.magic_mount_enabled ? "magic" : fallback_backend(m, config);
    }
    return mode == "none" ? "none" : fallback_backend(m, config);
}

MountReport mount_all_enabled(const Config& config) {
    MountReport report;
    fsutil::mlog("mount plan begin config=" + runtime_config_file().string());

    if (!claim_mount_orchestrator()) {
        std::ifstream marker(mount_orchestrator_file());
        std::string cycle;
        std::string result;
        std::getline(marker, cycle);
        std::getline(marker, result);
        report.ok = reboot::marker_matches_cycle(mount_orchestrator_file()) && result == "complete";
        report.detail = report.ok ? "module mount-all already completed this cycle"
                                  : "module mount cycle is incomplete or could not be recorded";
        fsutil::mlog(report.detail, logging::Level::Warning);
        return report;
    }
    kasumi::invalidate_active_state();
    kasumi::clear_replayable_mappings();

    const bool kasumi_present = ::kagami::kasumi::is_available();
    bool initial_kasumi_ok = true;
    if (!config.kasumi_enabled && kasumi_present) {
        std::string error;
        if (!kasumi::deactivate(error)) {
            fsutil::mlog("kasumi: disable failed: " + error, logging::Level::Error);
            initial_kasumi_ok = false;
        }
    }
    const auto modules = enumerate_mountable_modules();
    report.modules = static_cast<int>(modules.size());

    // Bootloop protection: refuse to mount if previous boots never confirmed
    // completion, so a bad mount cannot brick boot. The counter is bumped here
    // and cleared by recovery_boot_completed() once boot_completed is reached.
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    if (fs::exists(recovery_disabled_file(), ec)) {
        bool deactivate_ok = true;
        if (kasumi_present) {
            std::string error;
            deactivate_ok = kasumi::deactivate(error);
            if (!deactivate_ok) {
                fsutil::mlog("kasumi: bootloop disable failed: " + error, logging::Level::Error);
            }
        }
        report.ok = deactivate_ok;
        report.detail =
            deactivate_ok
                ? "mounting disabled by bootloop protection; run 'ksud kagami recovery reset'"
                : "mounting disabled, but Kasumi cleanup failed (see controller log)";
        return report;
    }
    const int attempts = read_boot_attempts();
    if (!reboot::authorized() && attempts >= kMaxBootAttempts) {
        bool deactivate_ok = true;
        if (kasumi_present) {
            std::string error;
            deactivate_ok = kasumi::deactivate(error);
            if (!deactivate_ok) {
                fsutil::mlog("kasumi: bootloop disable failed: " + error, logging::Level::Error);
            }
        }
        std::ofstream(recovery_disabled_file().string(), std::ios::trunc).put('\n');
        report.ok = deactivate_ok;
        report.detail =
            deactivate_ok
                ? "bootloop protection tripped after " + std::to_string(attempts) +
                      " unconfirmed boots; mounting disabled (run 'ksud kagami recovery reset')"
                : "bootloop protection tripped, but Kasumi cleanup failed (see controller log)";
        return report;
    }
    if (!reboot::authorized())
        write_boot_attempts(attempts + 1);

    const bool manage_kasumi = config.kasumi_enabled && ::kagami::kasumi::is_available();
    bool kasumi_prepare_ok = true;
    if (manage_kasumi) {
        std::string error;
        kasumi_prepare_ok = kasumi::deactivate(error);
        if (!kasumi_prepare_ok) {
            fsutil::mlog("kasumi: reset before rebuild failed: " + error, logging::Level::Error);
        }
    }

    // Orchestrate per module: a global override (config.mount_backend != "auto")
    // forces every module; otherwise each module's mode (module_mode.json, default
    // "auto") decides, with auto following OverlayFS -> Magic Mount -> none.
    // Kasumi is selected only by an explicit module/global setting.
    const auto modes = load_module_modes();
    const auto rules = load_module_rules();
    std::vector<ModuleEntry> overlay_set;
    std::vector<ModuleEntry> magic_set;
    std::vector<ModuleEntry> kasumi_set;
    for (const auto& m : modules) {
        const std::string mode = resolve_module_backend(m, config, modes);
        fsutil::mlog("selected module=" + m.id + " backend=" + mode);
        if (mode == "overlay") {
            overlay_set.push_back(m);
        } else if (mode == "magic") {
            magic_set.push_back(m);
        } else if (mode == "kasumi") {
            kasumi_set.push_back(m);
        }
        // "none" → skip
    }

    fsutil::mlog("orchestrator: overlay=" + std::to_string(overlay_set.size()) + " magic=" +
                 std::to_string(magic_set.size()) + " kasumi=" + std::to_string(kasumi_set.size()));

    bool non_kasumi_mounts_ok = true;
    bool kasumi_rules_ok = true;
    if (!overlay_set.empty() || !magic_set.empty()) {
        non_kasumi_mounts_ok = fsutil::run_in_init_mount_ns([&]() {
            bool r = true;
            // Preserve backend layering: OverlayFS is applied above Magic Mount.
            if (!magic_set.empty()) {
                r = magic::mount_modules(magic_set, config) && r;
            }
            if (!overlay_set.empty()) {
                r = overlay::mount_modules(overlay_set, config) && r;
            }
            return r;
        });
    }
    if (manage_kasumi) {
        kasumi_rules_ok = kasumi_prepare_ok && fsutil::run_in_init_mount_ns([&]() {
                              return kasumi::mount_modules(kasumi_set, config, rules);
                          });
        if (kasumi_rules_ok && !kasumi_set.empty() &&
            !kasumi::record_replayable_mappings(kasumi_set)) {
            kasumi_rules_ok = false;
            fsutil::mlog("kasumi: failed to commit the boot mapping plan", logging::Level::Error);
        }
        if (!kasumi_rules_ok || kasumi_set.empty()) {
            kasumi::clear_replayable_mappings();
        }
    } else {
        kasumi::clear_replayable_mappings();
    }

    bool features_ok = true;
    if (manage_kasumi && kasumi_prepare_ok && kasumi_rules_ok) {
        features_ok = apply_kasumi_features(config);
    } else if (manage_kasumi) {
        features_ok = false;
    }
    bool kasumi_ok = initial_kasumi_ok && kasumi_prepare_ok && features_ok && kasumi_rules_ok;
    if (manage_kasumi) {
        if (!::kagami::kasumi::set_enabled(kasumi_ok)) {
            kasumi_ok = false;
        }
        if (!kasumi_ok) {
            std::string error;
            (void)kasumi::deactivate(error);
        }
    }
    const bool ok = non_kasumi_mounts_ok && features_ok && kasumi_ok;

    report.backend = "hybrid(overlay=" + std::to_string(overlay_set.size()) +
                     ",magic=" + std::to_string(magic_set.size()) +
                     ",kasumi=" + std::to_string(kasumi_set.size()) + ")";
    report.ok = ok && reboot::write_cycle_marker(mount_orchestrator_file(), "complete\n");
    report.mounts = static_cast<int>(magic::active_mounts(config).size());
    report.detail = report.ok ? "ok" : "mounting or recording the plan failed (see controller log)";
    fsutil::mlog("mount plan complete backend=" + report.backend + " modules=" +
                     std::to_string(report.modules) + " result=" + (report.ok ? "ok" : "failed"),
                 report.ok ? logging::Level::Info : logging::Level::Error);
    return report;
}

bool unmount_all(const Config& config) {
    // Unwind the hybrid plan from its topmost backend.
    return fsutil::run_in_init_mount_ns([&]() {
        if (reboot::authorized()) {
            return kasumi::unmount_all(config) && overlay::unmount_all(config) &&
                   magic::unmount_all(config) && storage::teardown_shared(config);
        }
        bool r = true;
        r = kasumi::unmount_all(config) && r;
        r = overlay::unmount_all(config) && r;
        r = magic::unmount_all(config) && r;
        r = storage::teardown_shared(config) && r;
        return r;
    });
}

bool refresh_kasumi_modules(const Config& config) {
    if (!config.kasumi_enabled) {
        if (!::kagami::kasumi::is_available()) {
            return true;
        }
        std::string error;
        if (!kasumi::deactivate(error)) {
            fsutil::mlog("kasumi: disable failed: " + error, logging::Level::Error);
            return false;
        }
        return true;
    }
    if (!kasumi_usable()) {
        return false;
    }
    const auto replayable = kasumi::replayable_module_ids();
    if (replayable.empty()) {
        fsutil::mlog("kasumi: refusing post-boot mapping replay without a "
                     "Kasumi-owned boot plan",
                     logging::Level::Error);
        return false;
    }
    const std::set<std::string> replayable_set(replayable.begin(), replayable.end());
    const auto all = enumerate_mountable_modules();
    std::vector<ModuleEntry> selected;
    for (const auto& module : all) {
        if (replayable_set.count(module.id) != 0) {
            selected.push_back(module);
        }
    }
    const auto rules = load_module_rules();
    std::string error;
    const bool prepare_ok = kasumi::deactivate(error);
    if (!prepare_ok) {
        fsutil::mlog("kasumi: reset before refresh failed: " + error, logging::Level::Error);
    }
    const bool mount_ok = prepare_ok && fsutil::run_in_init_mount_ns([&]() {
                              return kasumi::mount_modules(selected, config, rules);
                          });
    const bool overlay_xattr_ok = mount_ok && fsutil::run_in_init_mount_ns([&]() {
                                      return overlay::restore_xattr_hiding(config);
                                  });
    const bool features_ok = apply_kasumi_features(config);
    bool ok = prepare_ok && mount_ok && overlay_xattr_ok && features_ok;
    if (!::kagami::kasumi::set_enabled(ok)) {
        ok = false;
    }
    if (!ok) {
        std::string cleanup_error;
        (void)kasumi::deactivate(cleanup_error);
    } else {
        (void)kasumi::restore_persisted_hide_rules(error);
    }
    return ok;
}

void recovery_boot_completed() {
    if (!marker_matches_current_boot(mount_orchestrator_file()))
        return;
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec);  // boot confirmed: clear the counter
}

void recovery_reset() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec);
    fs::remove(recovery_disabled_file(), ec);
}

std::string recovery_status_json() {
    std::error_code ec;
    const bool disabled = fs::exists(recovery_disabled_file(), ec);
    std::ostringstream out;
    out << "{\"boot_attempts\":" << read_boot_attempts() << ",\"max_attempts\":" << kMaxBootAttempts
        << ",\"mounting_disabled\":" << (disabled ? "true" : "false") << "}";
    return out.str();
}

}  // namespace kagami::mount
