#include "../kagami/src/mount/overlayfs.cpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <cassert>

namespace fs = std::filesystem;

namespace kagami {
fs::path runtime_modules_dir() {
    return "/installed-modules";
}
}  // namespace kagami

namespace kagami::mount::fsutil {
void mlog(const std::string&, logging::Level) {}
bool directory_is_opaque(const std::string& path, bool& opaque) {
    opaque = fs::exists(fs::path(path) / ".replace");
    return true;
}
std::string partition_mount_point(const std::string& partition) {
    return "/" + partition;
}
}  // namespace kagami::mount::fsutil

void check_partitions() {
    using namespace kagami::mount::overlay;
    const std::vector<std::string> parts = {"system", "product"};
    fs::create_directories("/product/etc");
    fs::create_directories("/modules/old/system");
    fs::create_directories("/modules/old/product/etc");
    const fs::path system = "/modules/old/system";
    const auto alias = system / "product";
    fs::create_directory_symlink("../product", alias);
    assert(validate_partition_tree(system, parts, true));
    assert(validate_partition_tree("/modules/old/product", parts, false));
    const auto plan = plan_overlays("/modules", {"old"}, parts);
    assert(plan.size() == 1);
    assert(plan.at("/product/etc") == std::vector<std::string>{"/modules/old/product/etc"});

    for (const char* target : {"./../product", "/modules/old/product"}) {
        fs::remove(alias);
        fs::create_directory_symlink(target, alias);
        assert(validate_partition_tree(system, parts, true));
    }
    for (const char* target : {"../vendor", "/product", "../../other/product"}) {
        fs::remove(alias);
        fs::create_directory_symlink(target, alias);
        assert(!validate_partition_tree(system, parts, true));
    }
    fs::remove(alias);
    fs::create_directory_symlink("../product", alias);
    fs::rename("/modules/old/product", "/modules/old/saved");
    assert(!validate_partition_tree(system, parts, true));
    fs::create_directory_symlink("saved", "/modules/old/product");
    assert(!validate_partition_tree(system, parts, true));
    fs::remove("/modules/old/product");
    fs::remove(alias);
    fs::create_directories(alias / "etc");
    assert(validate_partition_tree(system, parts, true));
    std::ofstream(alias / ".replace") << "";
    assert(!validate_partition_tree(system, parts, true));
}

void check_removed_content() {
    fs::create_directories("/installed-modules/kept");
    for (const char* name : {"kept", "removed", ".rw", "lost+found"})
        fs::create_directories(fs::path("/mirror") / name / "content");
    fs::create_directories("/unrelated/content");
    fs::create_directory_symlink("/unrelated", "/mirror/removed-link");
    assert(kagami::mount::overlay::prune_removed_content("/mirror"));
    assert(!fs::exists("/mirror/removed"));
    assert(!fs::is_symlink("/mirror/removed-link"));
    for (const char* name : {"kept", ".rw", "lost+found"})
        assert(fs::is_directory(fs::path("/mirror") / name / "content"));
    assert(fs::is_directory("/unrelated/content"));
    assert(kagami::mount::overlay::prune_removed_content("/mirror"));

    fs::create_directories("/mirror/blocked/content");
    assert(chmod("/", 0755) == 0 && chmod("/mirror", 0755) == 0 &&
           chmod("/mirror/blocked", 0555) == 0);
    assert(seteuid(65534) == 0);
    assert(!kagami::mount::overlay::prune_removed_content("/mirror"));
    assert(seteuid(0) == 0);
    assert(fs::is_directory("/mirror/blocked/content"));
}

int main() {
    if (geteuid() != 0)
        return 77;
    char directory[] = "/tmp/yukisu-overlay-partition-XXXXXX";
    assert(mkdtemp(directory));
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(chroot(directory) == 0 && chdir("/") == 0);
        check_partitions();
        check_removed_content();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    fs::remove_all(directory);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
