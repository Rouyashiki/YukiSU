#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "kagami/config.hpp"

namespace kagami::mount {

enum class BackendKind {
    Kasumi,
    Overlayfs,
    MagicMount,
};

struct BackendStatus {
    BackendKind kind;
    std::string name;
    bool available;
    bool preferred;
    std::string detail;
};

std::vector<BackendStatus> backend_statuses();
std::string backend_kind_name(BackendKind kind);

// A regular module eligible for mounting: /data/adb/modules/<id>.
struct ModuleEntry {
    std::string id;
    std::filesystem::path path;
};

// A path-specific backend override persisted in module_rules.json. The longest
// matching absolute path wins, matching the YukiSU hymo control-plane contract.
struct ModuleRule {
    std::string path;
    std::string mode;
    bool bypass = false;
};

using ModuleModeMap = std::map<std::string, std::string>;
using ModuleRuleMap = std::map<std::string, std::vector<ModuleRule>>;

struct MountReport {
    bool ok = false;
    int modules = 0;  // enabled modules processed
    int mounts = 0;   // committed partition mount points
    std::string backend;
    std::string detail;
};

// Enabled, mountable modules under runtime_modules_dir(): skips entries with
// no module.prop or with a disable/remove/skip_mount marker.
std::vector<ModuleEntry> enumerate_mountable_modules();

ModuleModeMap load_module_modes(const char* file = "module_mode.json");
bool save_module_modes(const ModuleModeMap& modes, const char* file = "module_mode.json");
ModuleRuleMap load_module_rules();
bool save_module_rules(const ModuleRuleMap& rules);

// Resolve a module's effective backend ("overlay"|"magic"|"kasumi"|"none") from
// its configured mode (modes[id], the global override config.mount_backend, or
// the auto fallback). Used by the orchestrator and to report a module's actual
// mount method. `modes` is the parsed module_mode.json (id -> mode).
std::string resolve_module_backend(const ModuleEntry& module, const Config& config,
                                   const ModuleModeMap& modes);

// Rebuild only Kasumi mappings in the init namespace. This is safe to expose
// post-boot for the Kasumi hot-mount controls.
bool refresh_kasumi_modules(const Config& config);

// Apply the boot or late-load module mount plan inside the init mount namespace.
MountReport mount_all_enabled(const Config& config);

// Tear down everything Kagami mounted (enters the init mount ns).
bool unmount_all(const Config& config);

// Bootloop protection: mount_all_enabled() bumps an unconfirmed-boot counter and
// refuses once it exceeds the limit. recovery_boot_completed() clears it once a
// boot reaches boot_completed; recovery_reset() re-enables after a trip.
void recovery_boot_completed();
void recovery_reset();
std::string recovery_status_json();

}  // namespace kagami::mount
