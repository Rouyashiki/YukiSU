#pragma once

#include <string>

#include "kagami/config.hpp"

namespace kagami::mount::storage {

// OverlayFS needs a clean module mirror: an overlay lowerdir exposes the source
// inode's SELinux context and /data contexts are wrong for /system, so module
// content is relabeled into a mirror. (Kasumi does NOT use this: its vnode
// clones the source SID and redirects straight to /data/adb/modules.) The mirror
// is a tmpfs (when it preserves overlay xattrs), an ext4 loop image, or a
// read-only erofs image paired with a tmpfs writable layer, mounted at a
// per-boot random /mnt/<rand> path (see current_mirror_dir) so it presents no
// fixed, /dev-anchored signature.
enum class Mode { Tmpfs, Ext4, Erofs };

const char* mode_name(Mode mode);

struct Handle {
    bool ok = false;
    Mode mode = Mode::Ext4;
    std::string content_dir;  // per-module lower trees live at content_dir/<id>/<part>
    std::string rw_dir;       // writable fs for per-partition upperdir/workdir
};

// Mount or reuse the shared mirror per config.fs_type ("auto" => tmpfs if
// /proc/config.gz enables CONFIG_TMPFS_XATTR, else ext4). Must run inside the init mount namespace;
// marks new mounts private and registers them with KernelSU. Returns
// Handle{ok=false} on failure. Picks and persists the per-boot mount path.
Handle setup(const Config& config);

// The overlay mirror mountpoint in effect for this boot: an explicit non-default
// config.mirror_dir, else the per-boot random /mnt path setup() recorded, else
// "" when no mirror is active. Read-only; never generates a path.
std::string current_mirror_dir(const Config& config);

// Called in the init mount namespace after the module is marked for removal.
bool remove_module_content(const Config& config, const std::string& module_id);

// Detach the one shared mirror after every backend has released it. Magic Mount
// never calls setup(), so an all-Magic configuration does not create this mount.
bool teardown_shared(const Config& config);

}  // namespace kagami::mount::storage
