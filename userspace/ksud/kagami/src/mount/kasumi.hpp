#pragma once

#include <map>
#include <string>
#include <vector>

#include "mount/backend.hpp"

namespace kagami::mount::kasumi {

// Apply each Kasumi runtime feature exactly as configured while the global
// gate is disabled. No feature implicitly enables another one.
bool apply_feature_config(const Config& config, std::string& error);

// Disable the global gate and runtime features without discarding path rules.
bool disable_control_state(std::string& error);

// Restore user HIDE rules in init's mount namespace after boot completion,
// only while Kasumi is enabled. Failures must not roll back module mappings.
bool restore_persisted_hide_rules(std::string& error, bool before_start = false);
bool verify_persisted_hide_rules(int& pending, std::string& error);
// Legacy user and module hides share one kernel slot; preserve every owner.
bool update_legacy_user_hide(const std::string& path, bool remove, int flags = 0);
bool has_pending_hide_rules();
// Called by the daemon's event loop; reloads current rules before retrying.
void retry_pending_hide_rules();

// Disable Kasumi and clear path rules without requiring an active mirror.
bool deactivate(std::string& error);

// Compiles each enabled module's /data/adb/modules tree into ADD/MERGE/HIDE
// Kasumi rules that redirect straight to the source inode (no mirror; the vnode
// clones the source SELinux SID). Must run in PID 1's mount namespace so the
// rules resolve against the init-namespace view.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules);

// Removes Kagami-owned Kasumi rules. Kasumi holds no mirror storage of its own.
// Must run in the init mount namespace.
bool unmount_all(const Config& config);

bool is_active();
void invalidate_active_state();
bool has_replayable_mappings();
std::vector<std::string> replayable_module_ids();
bool record_replayable_mappings(const std::vector<ModuleEntry>& modules);
void clear_replayable_mappings();

}  // namespace kagami::mount::kasumi
