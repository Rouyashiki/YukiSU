#include "../src/boot/boot_patch.hpp"
#include "../src/core/uts_view.hpp"
#include "../src/utils.hpp"

#include <dirent.h>
#include <sys/utsname.h>
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path root;
std::string original_release;
std::string modinfo;
bool original_ok = true;
bool uts_supported = true;
int probes = 0;
int scans = 0;
int commands = 0;
}  // namespace

namespace ksud {
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
bool get_uts_view_original_release(std::string* release, bool* supported) {
    ++probes;
    *supported = uts_supported;
    *release = original_release;
    return original_ok;
}
ExecResult exec_command(const std::vector<std::string>& args) {
    assert((args == std::vector<std::string>{"modinfo", "/vendor/lib/modules/vendor.ko"}));
    ++commands;
    return {0, modinfo, ""};
}
}  // namespace ksud

extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    const auto actual = std::strcmp(path, "/proc/sys/kernel/osrelease") == 0
                            ? (root / "release").string()
                            : std::string(path);
    return __real_open(actual.c_str(), flags, mode);
}
extern "C" DIR* __real_opendir(const char*);
extern "C" DIR* __wrap_opendir(const char* path) {
    assert(std::strcmp(path, "/vendor/lib/modules") == 0);
    ++scans;
    return __real_opendir((root / "modules").c_str());
}
extern "C" int __wrap_uname(utsname* value) {
    std::strcpy(value->release, "6.1.145-android14-11-test");
    return 0;
}

int main() {
    using namespace ksud;
    char directory[] = "/tmp/yukisu-kmi-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    std::ofstream(root / "release") << "6.6.118-android15-8-test\n";
    assert(get_bootstrap_kmi() == "android15-6.6" && probes == 0 && scans == 0);

    std::ofstream(root / "release") << "6.6.118-custom\n";
    assert(get_bootstrap_kmi().empty() && probes == 0 && scans == 1);
    std::filesystem::create_directory(root / "modules");
    std::ofstream(root / "modules" / "modules.load") << "vendor.ko\n";
    assert(get_bootstrap_kmi().empty() && commands == 0);
    std::ofstream(root / "modules" / "vendor.ko");
    modinfo = "name: vendor\nvermagic: 6.6.118-android15-8 SMP preempt mod_unload aarch64\n";
    assert(get_bootstrap_kmi() == "android15-6.6" && probes == 0 && commands == 1);
    modinfo = "vermagic: 6.6.118-custom SMP\n";
    assert(get_bootstrap_kmi().empty());
    modinfo.clear();
    assert(get_bootstrap_kmi().empty());

    original_release = "6.12.69-android16-5-original";
    const int previous_scans = scans;
    assert(get_current_kmi() == "android16-6.12" && scans == previous_scans);
    original_ok = false;
    assert(get_current_kmi().empty() && scans == previous_scans);
    original_ok = true;
    uts_supported = false;
    std::filesystem::remove(root / "release");
    assert(get_current_kmi() == "android14-6.1" && scans == previous_scans);
    assert(get_bootstrap_kmi() == "android14-6.1" && probes == 3);
    std::filesystem::remove_all(root);
}
