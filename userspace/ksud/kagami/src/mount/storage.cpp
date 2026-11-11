#include "mount/storage.hpp"
#include "core/runtime.hpp"
#include "kagami/embedded.hpp"
#include "utils.hpp"

#include "mount/mount_fs.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace kagami::mount::storage {

namespace fs = std::filesystem;
using fsutil::mlog;

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Tmpfs:
        return "tmpfs";
    case Mode::Ext4:
        return "ext4";
    case Mode::Erofs:
        return "erofs";
    }
    return "?";
}

namespace {
bool run_tool(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        errno = EINVAL;
        return false;
    }
    mlog("tool=" + argv.front() + " target=" + argv.back() + " begin", logging::Level::Debug);
    const auto result = ksud::exec_command(argv);
    const bool ok = result.exit_code == 0;
    mlog("tool=" + argv.front() + " target=" + argv.back() +
             " exit=" + std::to_string(result.exit_code) +
             (result.stderr_str.empty() ? "" : " stderr=" + result.stderr_str),
         ok ? logging::Level::Debug : logging::Level::Error);
    if (!ok)
        errno = result.error_number ? result.error_number : EIO;
    return ok;
}

bool tmpfs_xattr_enabled() {
    gzFile file = gzopen("/proc/config.gz", "rbe");
    if (!file) {
        mlog("storage: cannot read /proc/config.gz: " + std::string(std::strerror(errno)) +
                 "; defaulting to ext4",
             logging::Level::Warning);
        return false;
    }
    std::string config;
    char buffer[8192];
    int count;
    while ((count = gzread(file, buffer, sizeof(buffer))) > 0) {
        config.append(buffer, static_cast<size_t>(count));
        if (config.size() > 4UL * 1024 * 1024)
            break;
    }
    int error;
    (void)gzerror(file, &error);
    const bool valid = count == 0 && error == Z_OK && gzeof(file) && !gzdirect(file);
    const int closed = gzclose(file);
    if (!valid || closed != Z_OK) {
        mlog("storage: invalid or incomplete /proc/config.gz; defaulting to ext4",
             logging::Level::Warning);
        return false;
    }
    bool enabled = false;
    ksud::for_each_line(config, [&](std::string_view line) {
        if (line == "CONFIG_TMPFS_XATTR=y")
            enabled = true;
    });
    mlog(std::string("storage: CONFIG_TMPFS_XATTR=") +
         (enabled ? "y; defaulting to tmpfs" : "disabled; defaulting to ext4"));
    return enabled;
}

bool make_ext4_image(const std::string& img, int size_mb) {
    const std::string size = std::to_string(size_mb) + "M";
    if (!run_tool({"truncate", "-s", size, img}) && !run_tool({"fallocate", "-l", size, img})) {
        mlog("storage: failed to allocate image " + img, logging::Level::Error);
        return false;
    }
    if (!run_tool({"mke2fs", "-t", "ext4", "-O", "^has_journal", "-F", img}) &&
        !run_tool({"mkfs.ext4", "-O", "^has_journal", "-F", img})) {
        mlog("storage: mke2fs failed for " + img, logging::Level::Error);
        return false;
    }
    return true;
}

// Make a mount private and register it with KernelSU for per-app unmount.
bool finalize(const std::string& dir) {
    return ::mount("none", dir.c_str(), nullptr, MS_PRIVATE, nullptr) == 0 &&
           fsutil::register_umount(dir);
}
}  // namespace

class PendingMounts {
public:
    PendingMounts() = default;
    PendingMounts(const PendingMounts&) = delete;
    PendingMounts& operator=(const PendingMounts&) = delete;
    PendingMounts(PendingMounts&&) = delete;
    PendingMounts& operator=(PendingMounts&&) = delete;
    void add(const std::string& path) { paths.push_back(path); }
    void commit() { paths.clear(); }
    ~PendingMounts() {
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
            if (umount2(it->c_str(), MNT_DETACH) != 0)
                mlog("storage: rollback failed for " + *it + ": " + std::strerror(errno),
                     logging::Level::Error);
            else
                (void)fsutil::unregister_umount(*it);
        }
    }

private:
    std::vector<std::string> paths;
};

// Return the filesystem type when `path` is an exact mountpoint in this mount
// namespace. A later backend must reuse the mirror acquired by the first one;
// blindly detaching it would invalidate the other backend's lower trees.
namespace {
bool mounted_mode(const std::string& path, Mode& mode) {
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> fields;
        std::string field;
        while (pre >> field) {
            fields.push_back(field);
        }
        if (fields.size() < 5 || fsutil::decode_mount_path(fields[4]) != path) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        std::string fstype;
        post >> fstype;
        if (fstype == "tmpfs") {
            mode = Mode::Tmpfs;
            return true;
        }
        if (fstype == "ext4") {
            mode = Mode::Ext4;
            return true;
        }
        if (fstype == "erofs") {
            mode = Mode::Erofs;
            return true;
        }
        return false;
    }
    return false;
}

// The per-boot mirror path is recorded here so status/teardown in later kagamid
// invocations agree with the mount-all that created it.
fs::path mirror_run_file(const Config& /*unused*/) {
    return runtime_data_dir() / "run" / "mirror_mounts.list";
}

// "" and the retired fixed default both mean "randomize"; any other explicit
// mirror_dir is an operator override that wins.
bool mirror_is_auto(const std::string& dir) {
    return dir.empty() || dir == "/dev/kagami_mirror";
}

std::string random_mount_name() {
    std::ifstream u("/dev/urandom", std::ios::binary);
    static const char hex[] = "0123456789abcdef";
    std::string name;
    for (int i = 0; i < 8; ++i) {
        unsigned char c = 0;
        if (!u.read(reinterpret_cast<char*>(&c), 1)) {
            break;
        }
        name.push_back(hex[(c >> 4) & 0xF]);
        name.push_back(hex[c & 0xF]);
    }
    return name.size() == 16 ? name : std::string("kagami-fallback");
}

bool mirror_records(const Config& config, std::vector<fsutil::MountRecord>& records) {
    bool legacy;
    return fsutil::read_mount_journal(mirror_run_file(config), records, legacy) && !legacy;
}

bool mirror_owns(const Config& config, const std::string& path, bool include_init = false) {
    std::vector<fsutil::MountRecord> records;
    if (!mirror_records(config, records))
        return false;
    return std::any_of(records.begin(), records.end(), [&path, include_init](const auto& record) {
        return record.path == path && fsutil::mount_matches(record, include_init);
    });
}

std::string owned_mirror(const Config& config, bool include_init = false) {
    std::vector<fsutil::MountRecord> records;
    if (!mirror_records(config, records) || records.empty() ||
        !fsutil::mount_matches(records.front(), include_init))
        return {};
    return records.front().path;
}

bool record_mirror(const Config& config, const std::string& path, const std::string& rw = {}) {
    std::vector<std::string> paths{path};
    if (!rw.empty())
        paths.push_back(rw);
    else if (mirror_owns(config, path + ".rw"))
        paths.push_back(path + ".rw");
    return fsutil::write_mount_journal(mirror_run_file(config), paths);
}
}  // namespace

std::string current_mirror_dir(const Config& config) {
    auto owned = owned_mirror(config, true);
    if (!owned.empty())
        return owned;
    return mirror_is_auto(config.mirror_dir) ? std::string{} : config.mirror_dir;
}

bool remove_module_content(const Config& config, const std::string& module_id) {
    if (module_id.empty() || module_id.front() == '.' || module_id == "lost+found" ||
        fs::path(module_id).filename() != module_id) {
        errno = EINVAL;
        return false;
    }
    const auto mirror = owned_mirror(config);
    if (mirror.empty())
        return true;
    std::error_code error;
    fs::remove_all(fs::path(mirror) / module_id, error);
    if (error)
        mlog("storage: remove module content " + module_id + ": " + error.message(),
             logging::Level::Error);
    return !error;
}

// Reuse this boot's committed mirror path, or choose a fresh mountpoint.
namespace {
std::string acquire_mirror_dir(const Config& config) {
    if (!mirror_is_auto(config.mirror_dir)) {
        return config.mirror_dir;
    }
    std::string existing = current_mirror_dir(config);
    if (!existing.empty()) {
        return existing;
    }
    return "/mnt/" + random_mount_name();
}
}  // namespace

Handle setup(const Config& config) {
    Handle h;
    PendingMounts pending;
    const std::string base = acquire_mirror_dir(config);
    if (base.empty())
        return h;
    h.content_dir = base;
    const std::string img = (runtime_data_dir() / "mirror.img").string();
    const std::string erofs_img = (runtime_data_dir() / "mirror.erofs").string();

    std::error_code ec;
    fs::create_directories(h.content_dir, ec);
    if (ec) {
        mlog("storage: create mirror mountpoint: " + ec.message(), logging::Level::Error);
        return h;
    }

    const std::string& mode = config.fs_type;
    const bool want_auto = mode == "auto" || mode.empty();
    const bool writable = config.overlay_writable;  // upper/work layer is opt-in

    if (mounted_mode(h.content_dir, h.mode)) {
        if (owned_mirror(config) != base) {
            mlog("storage: refusing to reuse unowned mount " + base, logging::Level::Error);
            return h;
        }
        if (writable) {
            h.rw_dir = h.mode == Mode::Erofs ? base + ".rw" : h.content_dir + "/.rw";
            fs::create_directories(h.rw_dir, ec);
            if (ec) {
                mlog("storage: failed to create shared writable layer: " + ec.message(),
                     logging::Level::Error);
                return Handle{};
            }
            if (h.mode == Mode::Erofs) {
                Mode rw_mode;
                if (mounted_mode(h.rw_dir, rw_mode) && !mirror_owns(config, h.rw_dir)) {
                    mlog("storage: refusing unowned writable layer " + h.rw_dir,
                         logging::Level::Error);
                    return Handle{};
                }
                if (!mounted_mode(h.rw_dir, rw_mode)) {
                    if (::mount(config.mount_source.c_str(), h.rw_dir.c_str(), "tmpfs", 0,
                                nullptr) != 0) {
                        mlog("storage: shared erofs writable tmpfs failed: " +
                                 std::string(std::strerror(errno)),
                             logging::Level::Error);
                        return Handle{};
                    }
                    pending.add(h.rw_dir);
                    if (!finalize(h.rw_dir))
                        return Handle{};
                }
            }
        }
        if (!record_mirror(config, base, h.mode == Mode::Erofs ? h.rw_dir : ""))
            return Handle{};
        pending.commit();
        h.ok = true;
        mlog(std::string("storage: reusing shared ") + mode_name(h.mode) + " mirror");
        return h;
    }
    if (!fsutil::prepare_empty_mountpoint(h.content_dir))
        return h;

    // erofs: read-only content image, plus a separate tmpfs writable layer if opted in.
    if (mode == "erofs") {
        if (!fs::exists(erofs_img)) {
            mlog("storage: erofs image missing (" + erofs_img + "); build it at install time");
            return h;
        }
        std::string metadata_error;
        if (!prepare_loop_image(erofs_img, metadata_error)) {
            mlog("storage: " + metadata_error, logging::Level::Error);
            return h;
        }
        if (!run_tool({"mount", "-t", "erofs", "-o", "loop,ro", erofs_img, h.content_dir})) {
            mlog("storage: mount erofs image failed", logging::Level::Error);
            return h;
        }
        pending.add(h.content_dir);
        if (writable) {
            // EROFS mounts the root read-only, so its optional OverlayFS
            // upper/work tmpfs must be a sibling rather than a child.
            h.rw_dir = base + ".rw";
            fs::create_directories(h.rw_dir, ec);
            if (!fsutil::prepare_empty_mountpoint(h.rw_dir))
                return h;
            if (::mount(config.mount_source.c_str(), h.rw_dir.c_str(), "tmpfs", 0, nullptr) != 0) {
                mlog("storage: erofs writable tmpfs failed: " + std::string(std::strerror(errno)),
                     logging::Level::Error);
                return h;
            }
            pending.add(h.rw_dir);
            if (!finalize(h.rw_dir))
                return Handle{};
        }
        if (!finalize(h.content_dir))
            return Handle{};
        h.mode = Mode::Erofs;
        if (!record_mirror(config, base, h.mode == Mode::Erofs ? h.rw_dir : ""))
            return Handle{};
        pending.commit();
        h.ok = true;
        mlog(std::string("storage: erofs content") +
             (writable ? " + tmpfs writable layer" : " (read-only)"));
        return h;
    }

    const bool use_tmpfs = mode == "tmpfs" || (want_auto && tmpfs_xattr_enabled());
    if (use_tmpfs) {
        if (::mount(config.mount_source.c_str(), h.content_dir.c_str(), "tmpfs", 0, nullptr) == 0) {
            pending.add(h.content_dir);
            if (writable) {
                h.rw_dir = h.content_dir + "/.rw";
                fs::create_directories(h.rw_dir, ec);
                if (ec) {
                    mlog("storage: create writable layer: " + ec.message(), logging::Level::Error);
                    return Handle{};
                }
            }
            if (!finalize(h.content_dir))
                return Handle{};
            h.mode = Mode::Tmpfs;
            if (!record_mirror(config, base, h.mode == Mode::Erofs ? h.rw_dir : ""))
                return Handle{};
            pending.commit();
            h.ok = true;
            mlog(std::string("storage: selected tmpfs") +
                 (writable ? " (writable)" : " (read-only)"));
            return h;
        }
        mlog("storage: tmpfs mount failed: " + std::string(std::strerror(errno)) +
                 (want_auto ? "; falling back to ext4" : ""),
             want_auto ? logging::Level::Warning : logging::Level::Error);
        if (!want_auto)
            return h;
    }

    // ext4 loop image (forced, or the auto fallback). Writable layer lives inside.
    if (!fs::exists(img) && !make_ext4_image(img, config.mirror_img_size_mb)) {
        return h;
    }
    std::string metadata_error;
    if (!prepare_loop_image(img, metadata_error)) {
        mlog("storage: " + metadata_error, logging::Level::Error);
        return h;
    }
    if (!run_tool({"mount", "-t", "ext4", "-o", "loop,rw,noatime", img, h.content_dir})) {
        mlog("storage: mount ext4 image failed", logging::Level::Error);
        return h;
    }
    pending.add(h.content_dir);
    if (writable) {
        h.rw_dir = h.content_dir + "/.rw";
        fs::create_directories(h.rw_dir, ec);
        if (ec)
            return Handle{};
    }
    if (!finalize(h.content_dir))
        return Handle{};
    h.mode = Mode::Ext4;
    if (!record_mirror(config, base, h.mode == Mode::Erofs ? h.rw_dir : ""))
        return Handle{};
    pending.commit();
    h.ok = true;
    mlog(std::string("storage: selected ext4") + (writable ? " (writable)" : " (read-only)"));
    return h;
}

bool teardown_shared(const Config& config) {
    std::vector<fsutil::MountRecord> records;
    if (!mirror_records(config, records))
        return false;
    bool ok = true;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (!fsutil::mount_matches(*it))
            continue;
        if (umount2(it->path.c_str(), MNT_DETACH) != 0) {
            mlog("storage: detach failed for " + it->path + ": " + std::strerror(errno),
                 logging::Level::Error);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(it->path) && ok;
        }
    }
    if (ok) {
        std::error_code ec;
        fs::remove(mirror_run_file(config), ec);
        ok = !ec;
    }
    return ok;
}

}  // namespace kagami::mount::storage
