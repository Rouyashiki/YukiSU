#include "core/runtime.hpp"
#include "mount/mount_fs.hpp"
#include "mount/storage.hpp"

#include <unistd.h>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;
namespace {
fs::path root;
bool owned = true;
bool legacy_journal = false;
bool readable_journal = true;
}  // namespace

namespace kagami {
fs::path runtime_data_dir() {
    return root / "state";
}
}  // namespace kagami

namespace kagami::mount::fsutil {
void mlog(const std::string&, logging::Level) {}
bool mount_matches(const MountRecord& record, bool include_init) {
    assert(!include_init && record.path == (root / "mirror").string());
    return owned;
}
bool read_mount_journal(const std::string& journal, std::vector<MountRecord>& records,
                        bool& legacy) {
    assert(journal == (root / "state/run/mirror_mounts.list").string());
    records = {{(root / "mirror").string(), 1, 2, 3}};
    legacy = legacy_journal;
    return readable_journal;
}
}  // namespace kagami::mount::fsutil

int main() {
    char temporary[] = "/tmp/yukisu-storage-cleanup-XXXXXX";
    assert(mkdtemp(temporary));
    root = temporary;
    kagami::Config config;
    config.mirror_dir = (root / "unrelated").string();
    for (const char* directory : {"mirror/removed/content", "mirror/kept/content", "unrelated"})
        fs::create_directories(root / directory);
    using kagami::mount::storage::remove_module_content;
    owned = false;
    assert(remove_module_content(config, "removed"));
    assert(fs::exists(root / "mirror/removed/content"));
    owned = true;
    legacy_journal = true;
    assert(remove_module_content(config, "removed"));
    assert(fs::exists(root / "mirror/removed/content"));
    legacy_journal = false;
    readable_journal = false;
    assert(remove_module_content(config, "removed"));
    assert(fs::exists(root / "mirror/removed/content"));
    readable_journal = true;
    for (const char* id : {"", ".", "..", ".rw", "lost+found", "../unrelated", "/unrelated"})
        assert(!remove_module_content(config, id));
    assert(remove_module_content(config, "removed"));
    assert(remove_module_content(config, "removed"));
    assert(!fs::exists(root / "mirror/removed"));
    assert(fs::exists(root / "mirror/kept/content"));
    fs::create_directory_symlink(root / "unrelated", root / "mirror/removed-link");
    assert(remove_module_content(config, "removed-link"));
    assert(fs::exists(root / "unrelated"));
    fs::remove_all(root);
}
