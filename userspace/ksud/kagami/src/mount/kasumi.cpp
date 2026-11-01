#include "mount/kasumi.hpp"
#include "mount/kasumi_rule_index.hpp"

#include "core/json.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/mount_fs.hpp"
#include "uapi/kasumi.h"
#include "utils.hpp"

#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kagami::mount::kasumi {

namespace fs = std::filesystem;
using fsutil::mlog;

namespace {

bool user_hide_restore_pending = false;

fs::path active_file() {
    return runtime_data_dir() / "run" / "kasumi_active";
}
fs::path mapping_plan_file() {
    return runtime_data_dir() / "run" / "kasumi_mapping_plan";
}

// Resolve symlinked partition paths (e.g. /system/vendor -> /vendor) without
// discarding a non-existent leaf that a module adds.
std::string resolve_virtual_path(const std::string& value) {
    const fs::path path(value);
    if (!path.has_parent_path()) {
        return value;
    }
    fs::path current = path.parent_path();
    std::vector<fs::path> suffix;
    std::error_code ec;
    while (!current.empty() && current != "/" && !fs::exists(current, ec)) {
        if (ec) {
            return value;
        }
        suffix.push_back(current.filename());
        current = current.parent_path();
    }
    if (fs::exists(current, ec) && !ec) {
        current = fs::canonical(current, ec);
        if (ec) {
            return value;
        }
    }
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) {
        current /= *it;
    }
    current /= path.filename();
    return current.string();
}

struct RuleBatch {
    struct Mapping {
        std::string path, source;
        bool merge;
        int flags = 0;
    };
    std::vector<Mapping> mappings;
    std::map<std::string, int> hide;
};

bool merge_tree_supported(const fs::path& root) {
    bool opaque;
    if (!fsutil::directory_is_opaque(root.string(), opaque))
        return false;
    if (opaque) {
        mlog("kasumi: directory replacement is unsupported: " + root.string(),
             logging::Level::Error);
        return false;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec)
            return false;
        if (it->path().filename() == ".replace" ||
            (fs::is_directory(status) &&
             (!fsutil::directory_is_opaque(it->path().string(), opaque) || opaque))) {
            mlog("kasumi: unsupported replacement metadata: " + it->path().string(),
                 logging::Level::Error);
            return false;
        }
    }
    return !ec;
}

bool compile_tree(const fs::path& source_root, const std::string& virtual_root,
                  const detail::RuleIndex& rules, RuleBatch& batch, bool module_bypass) {
    std::error_code ec;
    if (!fs::is_directory(source_root, ec)) {
        return !ec || ec == std::errc::no_such_file_or_directory;
    }
    bool root_opaque;
    if (!fsutil::directory_is_opaque(source_root.string(), root_opaque) || root_opaque) {
        mlog("kasumi: unsupported partition replacement: " + source_root.string(),
             logging::Level::Error);
        return false;
    }
    auto it = fs::recursive_directory_iterator(source_root, ec);
    const auto end = fs::recursive_directory_iterator();
    for (; it != end && !ec; it.increment(ec)) {
        const fs::path source = it->path();
        const fs::path relative = source.lexically_relative(source_root);
        if (ec) {
            break;
        }
        const std::string virtual_path =
            resolve_virtual_path((fs::path(virtual_root) / relative).string());
        const auto* selected = rules.effective_rule(virtual_path);
        const int flags =
            (module_bypass || (selected && selected->bypass)) ? KSM_RULE_BYPASS_POLICY : 0;
        auto mode = selected ? std::string_view(selected->mode) : std::string_view("kasumi");
        if (mode == "hide") {
            batch.hide[virtual_path] |= flags;
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode == "none") {
            if (it->is_directory(ec) && !rules.has_nested_rule(virtual_path)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode != "kasumi" && mode != "auto") {
            // A module selected for Kasumi cannot safely create an OverlayFS or
            // Magic subtree after boot. Keep the path visible through Kasumi
            // rather than silently dropping it; mixed-backend planning remains
            // a boot-time concern for a later planner pass.
            mode = "kasumi";
        }

        if (source.filename() == ".replace") {
            mlog("kasumi: unsupported replacement marker: " + source.string(),
                 logging::Level::Error);
            return false;
        }
        struct stat st = {};
        if (lstat(source.c_str(), &st) != 0) {
            return false;
        }
        if (S_ISCHR(st.st_mode) && major(st.st_rdev) == 0 && minor(st.st_rdev) == 0) {
            batch.hide[virtual_path] |= flags;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            std::error_code target_ec;
            if (fs::is_directory(virtual_path, target_ec) && !target_ec &&
                !rules.has_nested_rule(virtual_path)) {
                if (!merge_tree_supported(source))
                    return false;
                batch.mappings.push_back({virtual_path, source.string(), true, flags});
                it.disable_recursion_pending();
            }
            continue;
        }
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (S_ISLNK(st.st_mode)) {
                std::error_code target_ec;
                const auto target_status = fs::symlink_status(virtual_path, target_ec);
                if (!target_ec && fs::is_symlink(target_status)) {
                    std::error_code link_ec;
                    const fs::path module_link = fs::read_symlink(source, link_ec);
                    if (!link_ec) {
                        const fs::path system_link = fs::read_symlink(virtual_path, link_ec);
                        if (!link_ec) {
                            const fs::path parent = fs::path(virtual_path).parent_path();
                            const auto destination = [&](const fs::path& link) {
                                return (link.is_absolute() ? link : parent / link)
                                    .lexically_normal();
                            };
                            if (destination(module_link) == destination(system_link)) {
                                mlog("kasumi: preserving matching symlink alias " + virtual_path +
                                     " from " + source.string());
                                continue;
                            }
                        }
                    }
                }
                target_ec.clear();
                if (fs::is_directory(virtual_path, target_ec) && !target_ec) {
                    mlog("kasumi: refusing to replace directory with symlink " + virtual_path +
                             " from " + source.string(),
                         logging::Level::Error);
                    return false;
                }
            }
            batch.mappings.push_back({virtual_path, source.string(), false, flags});
        }
    }
    if (ec) {
        mlog("kasumi: scan failed for " + source_root.string() + ": " + ec.message(),
             logging::Level::Error);
    }
    return !ec;
}

bool collect_module_rules(const std::vector<ModuleEntry>& modules, const Config& config,
                          const ModuleRuleMap& rules, RuleBatch& batch) {
    const std::vector<std::string>& partitions =
        config.partitions.empty() ? fsutil::managed_partitions() : config.partitions;
    const auto bypasses = load_module_modes("module_bypass.json");
    const auto bypass = [&](const std::string& id) {
        const auto it = bypasses.find(id);
        return it != bypasses.end() && it->second == "on";
    };
    for (const auto& module : modules) {
        const auto rule_it = rules.find(module.id);
        const std::vector<ModuleRule> empty_rules;
        const auto& module_rules = rule_it == rules.end() ? empty_rules : rule_it->second;
        for (const auto& rule : module_rules) {
            if (rule.mode == "hide" && !rule.path.empty() && rule.path.front() == '/') {
                batch.hide[resolve_virtual_path(rule.path)] |=
                    (bypass(module.id) || rule.bypass) ? KSM_RULE_BYPASS_POLICY : 0;
            }
        }
    }
    // Kernel rules use last-write-wins semantics. Reverse the sorted modules
    // to retain the same deterministic priority as YukiSU's hymo planner.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto rule_it = rules.find(it->id);
        const std::vector<ModuleRule> empty_rules;
        const auto& module_rules = rule_it == rules.end() ? empty_rules : rule_it->second;
        const detail::RuleIndex index(module_rules);
        for (const auto& partition : partitions) {
            if (!compile_tree(it->path / partition, "/" + partition, index, batch, bypass(it->id)))
                return false;
        }
    }
    return true;
}

bool collect_legacy_module_hides(std::map<std::string, int>& hides) {
    const auto replayable = replayable_module_ids();
    if (replayable.empty())
        return true;
    Config config;
    std::string error;
    if (!read_config_file(config, error)) {
        mlog("kasumi: " + error, logging::Level::Error);
        errno = EIO;
        return false;
    }
    std::vector<ModuleEntry> modules;
    modules.reserve(replayable.size());
    for (const auto& id : replayable) {
        const auto path = runtime_modules_dir() / id;
        std::error_code ec;
        if (!fs::is_directory(path, ec)) {
            errno = ec ? ec.value() : ENOENT;
            return false;
        }
        modules.push_back({id, path});
    }
    RuleBatch batch;
    if (!collect_module_rules(modules, config, load_module_rules(), batch))
        return false;
    hides = std::move(batch.hide);
    return true;
}

bool disable_kernel_features(std::string* error = nullptr) {
    bool ok = true;
    const auto disable = [&](bool result, const char* name) {
        if (!result) {
            ok = false;
            if (error && error->empty()) {
                *error = std::string("failed to disable Kasumi ") + name;
            }
        }
    };
    disable(::kagami::kasumi::set_debug(false), "kernel debug");
    disable(::kagami::kasumi::set_stealth(false), "stealth");
    disable(::kagami::kasumi::set_mount_hide(false), "mount hide");
    disable(::kagami::kasumi::set_maps_spoof(false), "maps spoof");
    disable(::kagami::kasumi::set_statfs_spoof(false), "statfs spoof");
    disable(::kagami::kasumi::set_selinux_fix(false), "SELinux Guard");
    return ok;
}

}  // namespace

bool apply_feature_config(const Config& config, std::string& error) {
    mlog("apply features: mount_hide=" + std::to_string(config.enable_mount_hide) +
         " maps=" + std::to_string(config.enable_maps_spoof) +
         " statfs=" + std::to_string(config.enable_statfs_spoof) +
         " selinux_guard=" + std::to_string(config.enable_selinux_fix) +
         " overlay_xattrs=" + std::to_string(config.enable_overlay_xattr_hide));
    bool ok = true;
    const auto apply = [&](bool result, const char* name) {
        if (!result) {
            if (error.empty()) {
                error = std::string("failed to set Kasumi ") + name;
            }
            ok = false;
        }
    };

    apply(::kagami::kasumi::set_debug(config.enable_kernel_debug), "kernel debug");
    apply(::kagami::kasumi::set_stealth(config.enable_stealth), "stealth");
    apply(::kagami::kasumi::set_mount_hide(config.enable_mount_hide), "mount hide");
    apply(::kagami::kasumi::set_maps_spoof(config.enable_maps_spoof), "maps spoof");
    apply(::kagami::kasumi::set_statfs_spoof(config.enable_statfs_spoof), "statfs spoof");
    apply(::kagami::kasumi::set_selinux_fix(config.enable_selinux_fix), "SELinux Guard");
    if (!ok) {
        (void)disable_kernel_features();
    }
    return ok;
}

bool disable_control_state(std::string& error) {
    user_hide_restore_pending = false;
    bool ok = true;
    if (!::kagami::kasumi::set_enabled(false)) {
        error = "failed to disable Kasumi";
        ok = false;
    }
    if (!disable_kernel_features(&error)) {
        ok = false;
    }
    return ok;
}

namespace {
bool restore_user_hide_rules(std::string& error, bool retry) {
    const auto bypasses = load_module_modes("hide_bypass.json");
    const auto bypass = [&](const std::string& path) {
        const auto it = bypasses.find(path);
        return it != bypasses.end() && it->second == "on";
    };
    if (ksud::getprop("sys.boot_completed") != "1" || !::kagami::kasumi::is_available()) {
        user_hide_restore_pending = false;
        return true;
    }
    bool managed = false;
    if (!::kagami::kasumi::managed_hide_mode(managed)) {
        error = "failed to query user hide ownership";
        return false;
    }
    if (managed) {
        user_hide_restore_pending = false;
        std::vector<::kagami::kasumi::UserHideRule> registered;
        if (!::kagami::kasumi::user_hide_rules(registered)) {
            error = "failed to query managed user hide rules";
            return false;
        }
        const auto paths = load_user_hide_rules();
        bool ok = true;
        for (const auto& rule : registered) {
            if (std::find(paths.begin(), paths.end(), rule.path) == paths.end())
                ok = ::kagami::kasumi::delete_user_hide(rule.path, rule.id) && ok;
        }
        for (const auto& path : paths) {
            const auto found = std::find_if(registered.begin(), registered.end(),
                                            [&](const auto& rule) { return rule.path == path; });
            if (found == registered.end() || found->bypass != bypass(path))
                ok = ::kagami::kasumi::upsert_user_hide(path, nullptr, bypass(path)) && ok;
        }
        if (!ok)
            error = "failed to synchronize managed user hide rules; module rules remain active";
        return ok;
    }
    const int enabled = ::kagami::kasumi::enabled_state();
    if (enabled == 0) {
        user_hide_restore_pending = false;
        return true;
    }
    if (enabled < 0) {
        user_hide_restore_pending = true;
        error = "failed to query Kasumi state before restoring user hide rules";
        if (!retry)
            mlog("kasumi: " + error, logging::Level::Error);
        return false;
    }
    const bool ok = fsutil::run_in_init_mount_ns([retry, &bypass]() {
        std::map<std::string, int> module_hides;
        if (!collect_legacy_module_hides(module_hides))
            return false;
        bool restored = true;
        for (const auto& path : load_user_hide_rules()) {
            struct stat parent = {};
            const bool parent_ready =
                !retry || (stat(fs::path(path).parent_path().c_str(), &parent) == 0 &&
                           S_ISDIR(parent.st_mode));
            const int flags = module_hides[resolve_virtual_path(path)] |
                              (bypass(path) ? KSM_RULE_BYPASS_POLICY : 0);
            const bool hidden = parent_ready && ::kagami::kasumi::hide_path(path, flags);
            if (!retry)
                mlog("user hide path=" + path +
                         (hidden ? " ok" : " failed errno=" + std::to_string(errno)),
                     hidden ? logging::Level::Debug : logging::Level::Error);
            restored = hidden && restored;
        }
        return restored;
    });
    user_hide_restore_pending = !ok;
    if (!ok) {
        error = "user hide rules pending retry; module rules remain active";
        if (!retry)
            mlog("kasumi: " + error, logging::Level::Error);
    } else if (retry) {
        mlog("kasumi: pending user hide rules restored");
    }
    return ok;
}
}  // namespace

bool restore_persisted_hide_rules(std::string& error) {
    return restore_user_hide_rules(error, false);
}

bool update_legacy_user_hide(const std::string& path, bool remove, int flags) {
    return fsutil::run_in_init_mount_ns([&]() {
        std::map<std::string, int> module_hides;
        if (!collect_legacy_module_hides(module_hides))
            return false;
        const auto module = module_hides.find(resolve_virtual_path(path));
        if (module != module_hides.end())
            return ::kagami::kasumi::hide_path(path, module->second | (remove ? 0 : flags));
        return remove ? ::kagami::kasumi::delete_rule(path)
                      : ::kagami::kasumi::hide_path(path, flags);
    });
}

bool has_pending_hide_rules() {
    return user_hide_restore_pending;
}

void retry_pending_hide_rules() {
    if (!user_hide_restore_pending)
        return;
    Config config;
    std::string error;
    if (!read_config_file(config, error))
        return;
    if (!config.kasumi_enabled) {
        user_hide_restore_pending = false;
        return;
    }
    (void)restore_user_hide_rules(error, true);
}

bool deactivate(std::string& error) {
    bool ok = disable_control_state(error);
    if (!::kagami::kasumi::clear_rules()) {
        if (error.empty()) {
            error = "failed to clear Kasumi rules";
        }
        ok = false;
    }
    invalidate_active_state();
    return ok;
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules) {
    if (!::kagami::kasumi::is_available()) {
        mlog("kasumi: backend requested but protocol is unavailable");
        return false;
    }
    // Kasumi redirects each target straight to the real module tree under
    // /data/adb/modules; the vnode clones the source inode's SELinux SID, so no
    // relabeled mirror is needed (unlike OverlayFS, which exposes the lowerdir's
    // context). Kasumi therefore mounts no workdir.
    RuleBatch batch;
    if (!collect_module_rules(modules, config, rules, batch))
        return false;

    bool ok = true;
    size_t add_count = 0;
    size_t merge_count = 0;
    for (const auto& rule : batch.mappings) {
        const bool added =
            rule.merge ? ::kagami::kasumi::add_merge_rule(rule.path, rule.source, rule.flags)
                       : ::kagami::kasumi::add_rule(rule.path, rule.source, rule.flags);
        rule.merge ? ++merge_count : ++add_count;
        mlog(std::string(rule.merge ? "merge" : "add") + " rule target=" + rule.path + " source=" +
                 rule.source + (added ? " ok" : " failed errno=" + std::to_string(errno)),
             added ? logging::Level::Debug : logging::Level::Error);
        ok = added && ok;
    }
    for (const auto& [path, flags] : batch.hide) {
        const bool hidden = ::kagami::kasumi::hide_path(path, flags);
        mlog("hide path=" + path + (hidden ? " ok" : " failed errno=" + std::to_string(errno)),
             hidden ? logging::Level::Debug : logging::Level::Error);
        ok = hidden && ok;
    }
    std::error_code ec;
    fs::create_directories(active_file().parent_path(), ec);
    if (ok && !write_boot_marker(active_file())) {
        ok = false;
        mlog("kasumi: failed to record restored runtime state", logging::Level::Error);
    }
    if (ok) {
        mlog("kasumi: installed add=" + std::to_string(add_count) + " merge=" +
             std::to_string(merge_count) + " hide=" + std::to_string(batch.hide.size()));
    } else {
        fs::remove(active_file(), ec);
        mlog("kasumi: one or more rule operations failed", logging::Level::Error);
    }
    return ok;
}

bool unmount_all(const Config& config) {
    (void)config;  // shared storage is released centrally after OverlayFS too.
    bool ok = true;
    if (::kagami::kasumi::is_available()) {
        std::string error;
        ok = deactivate(error);
        if (!ok) {
            mlog("kasumi: " + error);
        }
    }
    std::error_code ec;
    fs::remove(active_file(), ec);
    clear_replayable_mappings();
    return ok;
}

bool is_active() {
    return marker_matches_current_boot(active_file());
}

void invalidate_active_state() {
    user_hide_restore_pending = false;
    std::error_code ec;
    fs::remove(active_file(), ec);
}

std::vector<std::string> replayable_module_ids() {
    std::vector<std::string> ids;
    if (!marker_matches_current_boot(mapping_plan_file())) {
        return ids;
    }
    std::ifstream in(mapping_plan_file());
    std::string line;
    std::getline(in, line);  // boot id
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ids.push_back(line);
        }
    }
    return ids;
}

bool has_replayable_mappings() {
    return !replayable_module_ids().empty();
}

bool record_replayable_mappings(const std::vector<ModuleEntry>& modules) {
    if (modules.empty()) {
        clear_replayable_mappings();
        return true;
    }
    std::ostringstream ids;
    for (const auto& module : modules) {
        ids << module.id << "\n";
    }
    if (write_boot_marker(mapping_plan_file(), ids.str())) {
        return true;
    }
    clear_replayable_mappings();
    return false;
}

void clear_replayable_mappings() {
    std::error_code ec;
    fs::remove(mapping_plan_file(), ec);
}

}  // namespace kagami::mount::kasumi
