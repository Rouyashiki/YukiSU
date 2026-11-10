#include "../src/defs.hpp"
#include "../src/module/module.hpp"
#include "../src/sepolicy/sepolicy.hpp"
#include "../src/utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path root;
std::vector<std::string> rules;
std::vector<std::string> properties;
std::string failed_property;
bool fail_open = false;
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
    assert(argc == 5 && std::string(argv[1]) == "-n" && std::string(argv[2]) == "--");
    properties.emplace_back(std::string(argv[3]) + "=" + argv[4]);
    return argv[3] == failed_property ? 1 : 0;
}
extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (fail_open && std::filesystem::path(path).filename() == "system.prop") {
        errno = EACCES;
        return -1;
    }
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

    const auto prop_file = root / "active" / "system.prop";
    std::ofstream(prop_file) << "  # ignored=comment\r\n\n"
                                "malformed line\n = ignored\n"
                                " ro.build.display.id = Build #123 \r\n"
                                " hash.leading = #value\n"
                                "equals=a=b#c\n"
                                "empty=\n"
                                "switch=--delete";
    properties.clear();
    assert(ksud::load_system_prop() == 0);
    assert((properties == std::vector<std::string>{"ro.build.display.id=Build #123",
                                                   "hash.leading=#value", "equals=a=b#c",
                                                   "empty=", "switch=--delete"}));
    failed_property = "hash.leading";
    properties.clear();
    assert(ksud::load_system_prop() != 0 && properties.size() == 2);
    failed_property = "switch";
    properties.clear();
    assert(ksud::load_system_prop() != 0 && properties.size() == 5);
    failed_property.clear();
    properties.clear();
    fail_open = true;
    assert(ksud::load_system_prop() != 0 && properties.empty());
    fail_open = false;
    std::filesystem::remove(prop_file);
    std::filesystem::create_directory(prop_file);
    assert(ksud::load_system_prop() != 0 && properties.empty());
    std::filesystem::remove_all(root);
}
