#include "../src/core/restorecon.hpp"

#include <sys/xattr.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace {
std::map<std::string, std::string> contexts;
std::vector<std::string> writes;
std::string denied;
}  // namespace

namespace ksud {
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
}  // namespace ksud

extern "C" ssize_t __wrap_lgetxattr(const char* path, const char* name, void* value, size_t size) {
    assert(std::strcmp(name, "security.selinux") == 0);
    const auto entry = contexts.find(path);
    if (entry == contexts.end()) {
        errno = EACCES;
        return -1;
    }
    assert(size >= entry->second.size());
    std::memcpy(value, entry->second.data(), entry->second.size());
    return static_cast<ssize_t>(entry->second.size());
}
extern "C" int __wrap_lsetxattr(const char* path, const char* name, const void* value, size_t size,
                                int flags) {
    assert(std::strcmp(name, "security.selinux") == 0 && flags == 0);
    assert(size == std::strlen(static_cast<const char*>(value)) + 1);
    writes.emplace_back(path);
    if (path == denied) {
        errno = EACCES;
        return -1;
    }
    contexts[path] = static_cast<const char*>(value);
    return 0;
}

int main() {
    namespace fs = std::filesystem;
    char directory[] = "/tmp/yukisu-restorecon-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory);
    const auto child = root / "child";
    const auto labeled = root / "labeled";
    const auto unreadable = root / "unreadable";
    std::ofstream(child) << "";
    std::ofstream(labeled) << "";
    std::ofstream(unreadable) << "";
    contexts[root] = ksud::UNLABEL_CON;
    contexts[child] = "";
    contexts[labeled] = "u:object_r:vendor_file:s0";
    assert(ksud::restore_syscon_if_unlabeled(root));
    assert(writes.size() == 2 && writes.front() == root);
    assert(contexts[root] == ksud::SYSTEM_CON && contexts[child] == ksud::SYSTEM_CON);
    assert(contexts[labeled] == "u:object_r:vendor_file:s0");
    assert(contexts.count(unreadable) == 0);

    writes.clear();
    contexts[root] = ksud::UNLABEL_CON;
    denied = root;
    assert(!ksud::restore_syscon_if_unlabeled(root));
    assert((writes == std::vector<std::string>{root}));
    writes.clear();
    assert(!ksud::restore_syscon(root));
    assert((writes == std::vector<std::string>{root}));
    denied.clear();
    writes.clear();
    assert(ksud::restore_syscon(root));
    assert(writes.size() == 4 && writes.front() == root);
    fs::remove_all(root);
}
