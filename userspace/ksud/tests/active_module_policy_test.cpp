#include "../src/defs.hpp"
#include "../src/module/module.hpp"
#include "../src/sepolicy/sepolicy.hpp"
#include "../src/utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <cassert>
#include <cstdarg>
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path root;
std::vector<std::string> rules;
std::vector<std::string> properties;
std::string redirect(const char* path) {
    const std::string value(path);
    return value.rfind(ksud::MODULE_DIR, 0) == 0
               ? (root / value.substr(std::char_traits<char>::length(ksud::MODULE_DIR))).string()
               : value;
}
}  // namespace

namespace ksud {
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
int sepolicy_live_patch(const std::string& rule) {
    rules.push_back(rule);
    return 0;
}
}  // namespace ksud

extern "C" int resetprop_main(int argc, char** argv) {
    assert(argc == 4 && std::string(argv[1]) == "-n");
    properties.emplace_back(std::string(argv[2]) + "=" + argv[3]);
    return 0;
}
extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return __real_open(redirect(path).c_str(), flags, mode);
}
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* status) {
    return __real_stat(redirect(path).c_str(), status);
}
extern "C" DIR* __real_opendir(const char*);
extern "C" DIR* __wrap_opendir(const char* path) {
    return __real_opendir(redirect(path).c_str());
}

extern "C" dirent* __real_readdir(DIR*);
extern "C" dirent* __wrap_readdir(DIR* dir) {
    auto* entry = __real_readdir(dir);
    if (entry)
        entry->d_type = DT_UNKNOWN;
    return entry;
}

int main() {
    char directory[] = "/tmp/yukisu-active-modules-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    for (const char* name : {"active", "disabled", "removed"}) {
        const auto module = root / name;
        std::filesystem::create_directory(module);
        std::ofstream(module / "sepolicy.rule") << "allow " << name << "\n";
        std::ofstream(module / "system.prop") << name << "=1\n";
    }
    std::ofstream(root / "disabled" / "disable");
    std::ofstream(root / "removed" / "remove");
    assert(ksud::load_sepolicy_rule() == 0 && ksud::load_system_prop() == 0);
    assert((rules == std::vector<std::string>{"allow active\n"}));
    assert((properties == std::vector<std::string>{"active=1"}));
    std::filesystem::remove_all(root);
}
