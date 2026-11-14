#pragma once

// OverlayFS backend: systemless mounting via stacked overlay layers. Module
// partition trees live on a clean base (tmpfs/ext4/erofs, see storage.hpp) and
// are overlaid over each system partition with an optional writable upper/work
// layer. Mount source is config.mount_source ("KSU") so KernelSU can manage them.

#include <vector>

#include "kagami/config.hpp"
#include "mount/backend.hpp"

namespace kagami::mount::overlay {

// Set up the storage base and overlay every enabled module's partition trees.
// Must run inside the init mount namespace.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config);

// Detach our overlay mounts and the storage base. Must run in the init ns.
bool unmount_all(const Config& config);

// Re-register xattr hiding for OverlayFS mounts that Kagami already owns.
// Must run inside the init mount namespace. This is idempotent and does not
// remount or otherwise migrate a live backend.
bool restore_xattr_hiding(const Config& config);
bool xattr_hiding_paths(const Config& config, std::vector<std::string>& paths);

std::vector<std::string> active_mounts(const Config& config);
bool is_active(const Config& config);

}  // namespace kagami::mount::overlay
