#include "mount/overlayfs.hpp"
#include "core/runtime.hpp"

#include "kagami/kasumi_client.hpp"
#include "mount/backend.hpp"
#include "mount/mount_fs.hpp"
#include "mount/storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The new mount API (fsopen/fsconfig/fsmount/move_mount). bionic doesn't wrap
// these at our minSdk, so invoke them by number; the values are stable across
// the generic syscall table (arm64/x86_64).
#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

namespace kagami::mount::overlay {

namespace fs = std::filesystem;
using fsutil::mlog;

static constexpr unsigned kFsopenCloexec = 0x00000001u;
static constexpr unsigned kFsconfigSetString = 1u;
static constexpr unsigned kFsconfigCmdCreate = 6u;
static constexpr unsigned kFsmountCloexec = 0x00000001u;
static constexpr unsigned kMoveMountEmptyPath = 0x00000004u;
static constexpr unsigned kOpenTreeClone = 0x00000001u;    // OPEN_TREE_CLONE
static constexpr unsigned kOpenTreeCloexec = 0x00080000u;  // OPEN_TREE_CLOEXEC (== O_CLOEXEC)

namespace {
int sys_fsopen(const char* fsname, unsigned flags) {
    return static_cast<int>(syscall(__NR_fsopen, fsname, flags));
}
int sys_open_tree(int dfd, const char* path, unsigned flags) {
    return static_cast<int>(syscall(__NR_open_tree, dfd, path, flags));
}
int sys_fsconfig(int fd, unsigned cmd, const char* key, const char* value, int aux) {
    return static_cast<int>(syscall(__NR_fsconfig, fd, cmd, key, value, aux));
}
int sys_fsmount(int fd, unsigned flags, unsigned attr) {
    return static_cast<int>(syscall(__NR_fsmount, fd, flags, attr));
}
int sys_move_mount(int from_fd, const char* from, int to_fd, const char* to, unsigned flags) {
    return static_cast<int>(syscall(__NR_move_mount, from_fd, from, to_fd, to, flags));
}
}  // namespace

struct AttachedMount {
    std::string path;
    bool registered = false;
};

namespace {
bool record_mount(const std::string& target, std::vector<AttachedMount>& attached,
                  bool register_path = true) {
    attached.push_back({target, false});
    if (!register_path)
        return true;
    attached.back().registered = fsutil::register_umount(target);
    return attached.back().registered;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
}  // namespace

struct LayerSelection {
    std::vector<std::string> dirs;
    bool include_stock = true;
};

namespace {
bool select_layers(const std::vector<std::string>& roots, const std::string& relative,
                   LayerSelection& selected) {
    selected = {};
    std::vector<std::string> components;
    for (const auto& component : fs::path(relative).relative_path())
        components.push_back(component.string());
    for (const auto& root : roots) {
        std::string path = root;
        bool present = true;
        bool cutoff = false;
        for (size_t i = 0;; ++i) {
            struct stat st{};
            if (lstat(path.c_str(), &st) != 0) {
                if (errno != ENOENT && errno != ENOTDIR)
                    return false;
                present = false;
                break;
            }
            if (!S_ISDIR(st.st_mode)) {
                cutoff = true;
                present = false;
                break;
            }
            bool opaque = false;
            if (!fsutil::directory_is_opaque(path, opaque))
                return false;
            cutoff = cutoff || opaque;
            if (i == components.size())
                break;
            path += "/" + components[i];
        }
        if (present)
            selected.dirs.push_back(path);
        if (cutoff) {
            selected.include_stock = false;
            break;
        }
    }
    return true;
}

// Mount an overlay at dest: lower_dirs stacked over lowest, with an optional
// writable upper/work layer. Tries the fsopen API (no length limit on the
// lowerdir list) and falls back to classic mount(2).
bool mount_overlayfs(const std::vector<std::string>& lower_dirs, const std::string& lowest,
                     const std::string& upperdir, const std::string& workdir,
                     const std::string& dest, const std::string& source,
                     std::vector<AttachedMount>& attached, bool include_stock = true) {
    LayerSelection selected;
    if (!select_layers(lower_dirs, "", selected) || selected.dirs.empty())
        return false;
    include_stock = include_stock && selected.include_stock;
    if (include_stock) {
        selected.dirs.push_back(lowest);
    } else if (selected.dirs.size() == 1) {
        // A read-only OverlayFS mount needs at least two lower directories.
        const auto empty = (runtime_data_dir() / "run" / "overlay_empty").string();
        if (!fsutil::prepare_empty_mountpoint(empty))
            return false;
        selected.dirs.push_back(empty);
    }
    std::string lowerdir;
    for (const auto& dir : selected.dirs) {
        if (!lowerdir.empty())
            lowerdir += ':';
        lowerdir += dir;
    }

    const bool writable =
        !upperdir.empty() && !workdir.empty() && fs::exists(upperdir) && fs::exists(workdir);

    const int fsfd = sys_fsopen("overlay", kFsopenCloexec);
    if (fsfd >= 0) {
        bool ok = sys_fsconfig(fsfd, kFsconfigSetString, "lowerdir", lowerdir.c_str(), 0) == 0;
        if (ok && writable) {
            ok = sys_fsconfig(fsfd, kFsconfigSetString, "upperdir", upperdir.c_str(), 0) == 0 &&
                 sys_fsconfig(fsfd, kFsconfigSetString, "workdir", workdir.c_str(), 0) == 0;
        }
        if (ok) {
            ok = sys_fsconfig(fsfd, kFsconfigSetString, "source", source.c_str(), 0) == 0;
        }
        if (ok) {
            ok = sys_fsconfig(fsfd, kFsconfigCmdCreate, nullptr, nullptr, 0) == 0;
        }
        const int mfd = ok ? sys_fsmount(fsfd, kFsmountCloexec, 0) : -1;
        if (mfd >= 0) {
            const bool moved =
                sys_move_mount(mfd, "", AT_FDCWD, dest.c_str(), kMoveMountEmptyPath) == 0;
            close(mfd);
            close(fsfd);
            if (moved) {
                return record_mount(dest, attached);
            }
        } else {
            close(fsfd);
        }
    }

    std::string data = "lowerdir=" + lowerdir;
    if (writable) {
        data += ",upperdir=" + upperdir + ",workdir=" + workdir;
    }
    if (::mount(source.c_str(), dest.c_str(), "overlay", 0, data.c_str()) == 0) {
        return record_mount(dest, attached);
    }
    mlog("overlay: mount " + dest + " failed: " + std::strerror(errno), logging::Level::Error);
    return false;
}

// Re-establish a (possibly overlay-shadowed) sub-mount onto dst. open_tree clones
// the source mount tree recursively and move_mount attaches it, faithfully
// reproducing sub-mounts even when the source path is shadowed by an overlay;
// fall back to a classic recursive bind if the new mount API is unavailable.
bool rbind_mount(const std::string& src, const std::string& dst,
                 std::vector<AttachedMount>& attached) {
    const int tree =
        sys_open_tree(AT_FDCWD, src.c_str(), kOpenTreeClone | kOpenTreeCloexec | AT_RECURSIVE);
    if (tree >= 0) {
        const bool ok = sys_move_mount(tree, "", AT_FDCWD, dst.c_str(), kMoveMountEmptyPath) == 0;
        close(tree);
        if (ok) {
            return record_mount(dst, attached, false);
        }
    }
    return ::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0 &&
           record_mount(dst, attached, false);
}

// Mount points strictly under root (a sub-mount shadowed once we overlay root),
// sorted shallowest-first.
std::vector<std::string> child_mounts(const std::string& root) {
    std::set<std::string> found;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::vector<std::string> f;
        std::string tok;
        for (int i = 0; i < 5 && iss >> tok; ++i) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        const std::string mp = fsutil::decode_mount_path(f[4]);
        if (mp != root && starts_with(mp, root + "/")) {
            found.insert(mp);
        }
    }
    return {found.begin(), found.end()};
}

// Re-establish a sub-mount that the root overlay just shadowed: overlay the
// modules that touch it on top of the stock content, or bind the stock back.
bool mount_overlay_child(const std::string& mount_point, const std::string& relative,
                         const std::vector<std::string>& module_roots, const std::string& stock,
                         const std::string& source, std::vector<AttachedMount>& attached) {
    LayerSelection selected;
    if (!select_layers(module_roots, relative, selected))
        return false;
    if (selected.dirs.empty())
        return !selected.include_stock || rbind_mount(stock, mount_point, attached);
    return mount_overlayfs(selected.dirs, stock, "", "", mount_point, source, attached,
                           selected.include_stock);
}

// Overlay module_roots over the stock partition at root, then re-overlay every
// sub-mount the root overlay shadowed. The stock layer is the pre-overlay root,
// reached as "." after chdir (the cwd stays pinned to it once root is covered).
bool mount_overlay(const std::string& root, const std::vector<std::string>& module_roots,
                   const std::string& upperdir, const std::string& workdir,
                   const std::string& source, std::vector<AttachedMount>& attached) {
    mlog("overlay: " + root);
    if (chdir(root.c_str()) != 0) {
        mlog("overlay: chdir " + root + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }
    const std::string stock = ".";
    const std::vector<std::string> children = child_mounts(root);
    LayerSelection selected;
    if (!select_layers(module_roots, "", selected))
        return false;

    if (!mount_overlayfs(module_roots, stock, upperdir, workdir, root, source, attached)) {
        return false;
    }
    if (!selected.include_stock)
        return true;
    for (const auto& mp : children) {
        const std::string relative = mp.substr(root.size());
        const std::string stock_child = stock + relative;
        struct stat visible{};
        struct stat original{};
        if (lstat(mp.c_str(), &visible) != 0) {
            if (errno == ENOENT || errno == ENOTDIR)
                continue;
            return false;
        }
        if (lstat(stock_child.c_str(), &original) != 0) {
            if (errno == ENOENT || errno == ENOTDIR)
                continue;
            return false;
        }
        if ((visible.st_mode & S_IFMT) != (original.st_mode & S_IFMT) ||
            (!S_ISDIR(visible.st_mode) && !S_ISREG(visible.st_mode))) {
            continue;
        }
        if (!mount_overlay_child(mp, relative, module_roots, stock_child, source, attached)) {
            mlog(std::string("overlay: child ")
                     .append(mp)
                     .append(" failed; reverting ")
                     .append(root),
                 logging::Level::Warning);
            return false;
        }
    }
    return true;
}

// True if name is a managed partition other than "system" (vendor/product/...).
// These appear under a module's system/ tree and remap to /<name>.
bool is_sub_partition(const std::string& name, const std::vector<std::string>& parts) {
    return std::any_of(parts.begin(), parts.end(),
                       [&name](const auto& part) { return part != "system" && part == name; });
}

// Record a leaf overlay op: target is the live stock dir to overlay, layer is a
// module's content for it. Reject unrepresentable targets before attaching mounts.
void add_leaf(std::map<std::string, std::vector<std::string>>& ops, const std::string& target,
              const std::string& layer) {
    struct stat st{};
    if (lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        const std::string error = "overlay: target must be an existing directory: " + target;
        mlog(error, logging::Level::Error);
        throw std::runtime_error(error);
    }
    ops[target].push_back(layer);
}

// Emit depth-1 leaves under a partition root: every subdirectory of subroot
// becomes an overlay at <mount>/<child>. The partition root itself is NEVER
// overlaid — doing so shadows its bind sub-mounts (e.g. /system/vendor, or
// /vendor's qcrild tree) and breaks the device. Deeper sub-mounts under a leaf
// are re-established at mount time by mount_overlay's child handling.
void plan_partition_root(std::map<std::string, std::vector<std::string>>& ops,
                         const std::string& subroot, const std::string& mount) {
    const std::unique_ptr<DIR, decltype(&closedir)> d(opendir(subroot.c_str()), closedir);
    if (!d) {
        return;
    }
    std::error_code ec;
    struct dirent* e;
    while ((e = readdir(d.get())) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        const std::string layer = subroot + "/" + e->d_name;
        if (fs::is_directory(layer, ec)) {
            add_leaf(ops, mount + "/" + e->d_name, layer);
        }
    }
}

// Build the leaf overlay plan for all enabled modules. Module content lives at
// content_dir/<id>/<part>. Under system/, managed children (vendor/product/...)
// remap to /<part>; other system/ children and top-level partition trees become
// depth-1 leaves. Leaves are grouped by target so multiple modules stack.
std::map<std::string, std::vector<std::string>> plan_overlays(
    const std::string& content_dir, const std::vector<std::string>& enabled,
    const std::vector<std::string>& parts) {
    std::map<std::string, std::vector<std::string>> ops;
    std::error_code ec;
    for (const auto& id : enabled) {
        const std::string mbase = (fs::path(content_dir) / id).string();

        const std::string sysroot = mbase + "/system";
        const std::unique_ptr<DIR, decltype(&closedir)> d(opendir(sysroot.c_str()), closedir);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d.get())) != nullptr) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                const std::string child = sysroot + "/" + e->d_name;
                if (!fs::is_directory(fs::symlink_status(child, ec))) {
                    continue;
                }
                if (is_sub_partition(e->d_name, parts)) {
                    plan_partition_root(ops, child, std::string("/") + e->d_name);
                } else {
                    add_leaf(ops, (fs::path("/system") / e->d_name).string(), child);
                }
            }
        }

        for (const auto& p : parts) {
            if (p == "system") {
                continue;
            }
            const std::string top = (fs::path(mbase) / p).string();
            if (fs::is_directory(top, ec)) {
                plan_partition_root(ops, top, fsutil::partition_mount_point(p));
            }
        }
    }
    return ops;
}

std::string overlay_journal() {
    return (runtime_data_dir() / "run" / "overlay_mounts.list").string();
}

std::vector<std::string> our_overlays(const std::string& source) {
    (void)source;
    std::vector<fsutil::MountRecord> records;
    bool legacy;
    std::vector<std::string> paths;
    if (!fsutil::read_mount_journal(overlay_journal(), records, legacy) || legacy)
        return paths;
    for (const auto& record : records)
        if (fsutil::mount_matches(record, true))
            paths.push_back(record.path);
    return paths;
}

bool relabel_part(const std::string& dst, const std::string& part,
                  const std::vector<std::string>& parts, const fsutil::CopiedTree& tree) {
    struct Ancestor {
        std::size_t index;
        std::string path;
        std::string target;
        std::string context;
    };
    std::vector<Ancestor> ancestors;
    // Copying must finish before final labels are applied: changing a directory's
    // SELinux type earlier could change permissions for creating its children.
    // The copy's owned preorder replaces a second filesystem enumeration while
    // retaining parent-first labels and nearest-parent fallback for new paths.
    for (std::size_t i = 0; i < tree.entries.size(); ++i) {
        const auto& entry = tree.entries[i];
        const char* name = tree.names.c_str() + entry.name_offset;
        std::string path = dst;
        std::string target = fsutil::partition_mount_point(part);
        if (i != 0) {
            while (!ancestors.empty() && ancestors.back().index != entry.parent)
                ancestors.pop_back();
            if (ancestors.empty())
                return false;
            path = ancestors.back().path + "/" + name;
            target = part == "system" && entry.parent == 0 && is_sub_partition(name, parts)
                         ? std::string("/") + name
                         : ancestors.back().target + "/" + name;
        }
        std::string context;
        if (!fsutil::get_context(target, context)) {
            if (errno != ENOENT || ancestors.empty() || ancestors.back().context.empty()) {
                mlog("overlay: read context " + target + ": " + std::strerror(errno),
                     logging::Level::Error);
                return false;
            }
            context = ancestors.back().context;
        }
        if (!fsutil::set_context(path, context)) {
            mlog("overlay: set context " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        ancestors.push_back({i, std::move(path), std::move(target), std::move(context)});
    }
    return !tree.entries.empty();
}

bool component_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}

bool validate_partition_tree(const fs::path& path, const std::vector<std::string>& parts,
                             bool system) {
    bool opaque = false;
    if (!fsutil::directory_is_opaque(path.string(), opaque))
        return false;
    if (opaque) {
        mlog("overlay: partition-root replacement requires Magic Mount: " + path.string(),
             logging::Level::Error);
        return false;
    }
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec)
            return false;
        const auto name = it->path().filename();
        // Older installers link system/<part> to the module's top-level tree.
        if (system && is_sub_partition(name.string(), parts) && fs::is_symlink(status)) {
            const auto link = fs::read_symlink(it->path(), ec);
            if (ec)
                return false;
            const auto partition = path.parent_path() / name;
            const auto destination = (path / link).lexically_normal();
            if (destination == partition.lexically_normal() &&
                fs::is_directory(fs::symlink_status(partition, ec)) && !ec)
                continue;
        }
        if (!fs::is_directory(status)) {
            mlog("overlay: partition-root file or whiteout requires Magic Mount: " +
                     it->path().string(),
                 logging::Level::Error);
            return false;
        }
        if (system && is_sub_partition(it->path().filename().string(), parts) &&
            !validate_partition_tree(it->path(), parts, false))
            return false;
    }
    return !ec;
}

bool prune_removed_content(const fs::path& mirror) {
    std::error_code error;
    for (fs::directory_iterator it(mirror, error), end; it != end && !error; it.increment(error)) {
        const auto id = it->path().filename().string();
        if (id.front() == '.' || id == "lost+found")
            continue;
        const bool installed = fs::exists(runtime_modules_dir() / id, error);
        if (error)
            return false;
        if (!installed)
            fs::remove_all(it->path(), error);
        if (error)
            break;
    }
    if (error)
        mlog("overlay: prune removed module content: " + error.message(), logging::Level::Error);
    return !error;
}

// Rebuild the selected modules before publishing any overlay, including on ext4.
bool sync_content(const std::vector<ModuleEntry>& modules, const storage::Handle& base,
                  const std::vector<std::string>& partitions) {
    if (base.mode == storage::Mode::Erofs) {
        std::error_code ec;
        for (const auto& module : modules) {
            if (!component_name(module.id))
                return false;
            for (const auto& part : partitions) {
                if (!component_name(part))
                    return false;
                const auto tree = fs::path(base.content_dir) / module.id / part;
                if (!fs::is_directory(tree, ec)) {
                    if (ec && ec != std::errc::no_such_file_or_directory)
                        return false;
                    ec.clear();
                    continue;
                }
                if (!validate_partition_tree(tree, partitions, part == "system"))
                    return false;
                for (fs::recursive_directory_iterator it(tree, ec), end; it != end && !ec;
                     it.increment(ec)) {
                    if (it->path().filename() == ".replace") {
                        mlog("overlay: EROFS image contains an unconverted .replace marker: " +
                                 it->path().string(),
                             logging::Level::Error);
                        return false;
                    }
                }
                if (ec)
                    return false;
            }
        }
        return true;
    }
    for (const auto& part : partitions) {
        if (!component_name(part))
            return false;
    }
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(base.content_dir, ec)))
        return false;
    ec.clear();
    fs::create_directories(base.content_dir, ec);
    if (ec)
        return false;
    // Boot-time module removal runs before the persistent mirror is mounted.
    if (!prune_removed_content(base.content_dir))
        return false;
    for (const auto& module : modules) {
        if (!component_name(module.id) || module.id == ".rw" || module.id == "lost+found")
            return false;
        const fs::path cache = fs::path(base.content_dir) / module.id;
        fs::remove_all(cache, ec);
        if (ec)
            return false;
        for (const auto& part : partitions) {
            const auto src = module.path / part;
            const auto status = fs::symlink_status(src, ec);
            if (ec == std::errc::no_such_file_or_directory) {
                ec.clear();
                continue;
            }
            if (ec)
                return false;
            if (!fs::is_directory(status))
                continue;
            if (!validate_partition_tree(src, partitions, part == "system"))
                return false;
            const auto dst = cache / part;
            fs::create_directories(dst.parent_path(), ec);
            fsutil::CopiedTree copied;
            if (ec || !fsutil::copy_tree(src.string(), dst.string(), copied) ||
                !relabel_part(dst.string(), part, partitions, copied)) {
                mlog("overlay: sync failed for " + module.id + "/" + part, logging::Level::Error);
                return false;
            }
        }
    }
    return true;
}
}  // namespace

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config) {
    const storage::Handle base = storage::setup(config);
    if (!base.ok) {
        mlog("overlay: storage base setup failed", logging::Level::Error);
        return false;
    }
    mlog(std::string("overlay: base mode=") + storage::mode_name(base.mode) +
         " content=" + base.content_dir);

    const std::vector<std::string>& partitions =
        config.partitions.empty() ? fsutil::managed_partitions() : config.partitions;

    if (!sync_content(modules, base, partitions)) {
        mlog("overlay: content synchronization failed", logging::Level::Error);
        return false;
    }

    std::vector<std::string> enabled;
    enabled.reserve(modules.size());
    for (const auto& m : modules) {
        enabled.push_back(m.id);
    }

    // Overlay each module-touched leaf (a child of a partition root), never the
    // partition root itself. Shallower targets first so a parent leaf is mounted
    // before any nested leaf under it.
    const auto ops = plan_overlays(base.content_dir, enabled, partitions);
    std::vector<AttachedMount> attached;
    bool ok = !ops.empty();
    for (const auto& [target, layers] : ops) {
        if (layers.empty())
            continue;
        if (!mount_overlay(target, layers, "", "", config.mount_source, attached)) {
            ok = false;
            break;
        }
        if (config.kasumi_enabled && config.enable_overlay_xattr_hide &&
            ::kagami::kasumi::is_available() && !::kagami::kasumi::hide_overlay_xattrs(target)) {
            mlog("overlay: failed to hide xattrs for " + target, logging::Level::Error);
            ok = false;
            break;
        }
    }
    if (ok) {
        std::vector<std::string> paths;
        for (const auto& entry : attached)
            if (entry.registered)
                paths.push_back(entry.path);
        ok = fsutil::write_mount_journal(overlay_journal(), paths);
    }
    if (!ok) {
        for (auto it = attached.rbegin(); it != attached.rend(); ++it) {
            if (umount2(it->path.c_str(), MNT_DETACH) != 0)
                mlog("overlay: rollback failed for " + it->path + ": " + std::strerror(errno),
                     logging::Level::Error);
            else if (it->registered && !fsutil::unregister_umount(it->path))
                mlog("overlay: unregister failed for " + it->path, logging::Level::Error);
        }
        mlog("overlay: mount plan failed", logging::Level::Error);
        return false;
    }
    mlog("overlay: mounted " + std::to_string(ops.size()) + " leaf overlay(s)");
    return true;
}

bool unmount_all(const Config& config) {
    (void)config;
    std::vector<fsutil::MountRecord> records;
    bool legacy;
    if (!fsutil::read_mount_journal(overlay_journal(), records, legacy) || legacy)
        return false;
    bool ok = true;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (!fsutil::mount_matches(*it))
            continue;
        if (umount2(it->path.c_str(), MNT_DETACH) != 0) {
            mlog("overlay: detach failed for " + it->path + ": " + std::strerror(errno),
                 logging::Level::Error);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(it->path) && ok;
        }
    }
    if (ok) {
        std::error_code ec;
        fs::remove(overlay_journal(), ec);
        ok = !ec;
    }
    return ok;
}

bool restore_xattr_hiding(const Config& config) {
    std::vector<fsutil::MountRecord> records;
    bool legacy;
    if (!fsutil::read_mount_journal(overlay_journal(), records, legacy) || legacy)
        return false;
    if (!::kagami::kasumi::clear_overlay_xattr_hiding())
        return false;
    if (!config.enable_overlay_xattr_hide) {
        return true;
    }
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available()) {
        return false;
    }

    bool ok = true;
    for (const auto& target : our_overlays(config.mount_source)) {
        if (!::kagami::kasumi::hide_overlay_xattrs(target)) {
            mlog("overlay: failed to restore xattr hiding for " + target, logging::Level::Error);
            ok = false;
        }
    }
    return ok;
}

std::vector<std::string> active_mounts(const Config& config) {
    return our_overlays(config.mount_source);
}

bool is_active(const Config& config) {
    return !our_overlays(config.mount_source).empty();
}

}  // namespace kagami::mount::overlay
