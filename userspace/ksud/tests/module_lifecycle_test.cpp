#include "../src/core/ksucalls.hpp"
#include "../src/defs.hpp"
#include "../src/module/metamodule.hpp"
#include "../src/module/module.cpp"
#include "../src/module/module.hpp"
#include "../src/module/module_config.hpp"
#include "../src/utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace {
bool fail_fork = false;
bool fail_rename = false;
bool fail_props = false;
int forks = 0;
int renames = 0;
int prop_writes = 0;
int builtin_mounts = 0;
int builtin_uninstall_result = 0;
std::vector<std::string> builtin_uninstalls;
pid_t caller_group;

void put(const fs::path& path, const std::string& contents = "") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    assert(output << contents);
}

std::vector<std::string> launched() {
    std::vector<std::string> result;
    std::ifstream input("/launched");
    for (std::string line; std::getline(input, line);)
        result.push_back(line);
    return result;
}

void reset_modules() {
    fs::remove_all("/data");
    fs::create_directories(ksud::MODULE_DIR);
    fs::create_directories(ksud::MODULE_UPDATE_DIR);
    put("/launched");
    fail_fork = fail_rename = fail_props = false;
    forks = renames = prop_writes = 0;
    builtin_uninstall_result = 0;
    builtin_uninstalls.clear();
}
}  // namespace

namespace ksud {
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
ModuleVersions module_script_versions() {
    return {};
}
void apply_module_version_env(const ModuleVersions&) {}
const char* runtime_mode() {
    return "LKM";
}
bool is_late_load() {
    return true;
}
std::pair<uint64_t, bool> get_feature(uint32_t) {
    return {0, true};
}
int set_init_pgrp() {
    return -1;
}
}  // namespace ksud

namespace kagami {
int embedded_command(const std::vector<std::string>& args) {
    assert(args.size() == 3 && args[0] == "module" && args[1] == "metauninstall");
    builtin_uninstalls.push_back(args[2]);
    return builtin_uninstall_result;
}
int embedded_mount() {
    ++builtin_mounts;
    return 0;
}
void embedded_mount_skipped(const std::string&) {}
}  // namespace kagami

extern "C" pid_t __real_fork();
extern "C" pid_t __wrap_fork() {
    ++forks;
    if (fail_fork) {
        errno = EAGAIN;
        return -1;
    }
    return __real_fork();
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    ++renames;
    if (fail_rename) {
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}
extern "C" dirent* __real_readdir(DIR*);
extern "C" dirent* __wrap_readdir(DIR* dir) {
    auto* entry = __real_readdir(dir);
    if (entry)
        entry->d_type = DT_UNKNOWN;
    return entry;
}
extern "C" int __wrap_execl(const char*, const char* argv0, ...) {
    va_list args;
    va_start(args, argv0);
    const fs::path script = va_arg(args, const char*);
    va_end(args);
    if (script.filename() == "metauninstall.sh" || script.filename() == "metamount.sh") {
        assert(getpgrp() == caller_group);
        assert(std::string(getenv("MAGISK_VER")) == "inherited");
        const auto expected_cwd =
            script.filename() == "metauninstall.sh" ? script.parent_path() : fs::path("/caller");
        assert(fs::current_path() == expected_cwd);
        if (script.filename() == "metauninstall.sh")
            assert(std::string(getenv("MODULE_ID")) == "removed-module");
    }
    std::ofstream output("/launched", std::ios::app);
    assert(output << script.string() << '\n');
    output.close();
    _exit(0);
}
extern "C" int resetprop_main(int argc, char** argv) {
    assert(argc == 5 && std::string(argv[1]) == "-n" && std::string(argv[2]) == "--");
    ++prop_writes;
    return fail_props ? 1 : 0;
}

void check_metamodule_paths() {
    using namespace ksud;
    reset_modules();
    const fs::path external = "/data/adb/custom/meta";
    for (const char* script : {"service.sh", "metamount.sh", "metauninstall.sh"})
        put(external / script);
    put(fs::path(MODULE_DIR) / "meta/service.sh");
    fs::create_directory_symlink(external, fs::path(MODULE_DIR) / "alias");
    fs::create_directory_symlink("custom/meta", "/data/adb/metamodule");
    assert(get_metamodule_path() == external && get_metamodule_id() == "meta");
    assert(metamodule_exec_stage_script("service", ScriptWait::forever()) == 0);
    assert(exec_stage_script("service", ScriptWait::forever()) == 0);
    assert((launched() == std::vector<std::string>{(external / "service.sh").string(),
                                                   std::string(MODULE_DIR) + "meta/service.sh"}));
    fs::create_directory("/caller");
    fs::current_path("/caller");
    setenv("MAGISK_VER", "inherited", 1);
    caller_group = getpgrp();
    assert(metamodule_exec_mount_script() == 0 && builtin_mounts == 0);
    assert(metamodule_exec_uninstall_script("removed-module") == 0);
    assert(builtin_uninstalls.empty());
    assert(launched().size() == 4);
    fs::current_path("/");

    fs::remove("/data/adb/metamodule");
    fs::remove_all(fs::path(MODULE_DIR) / "meta");
    put(external / "module.prop", "id=meta\nmetamodule=1\n");
    assert(get_metamodule_path() == std::string(MODULE_DIR) + "alias");
}

void check_install_wrappers() {
    using namespace ksud;
    reset_modules();
    bool custom = true;
    const std::string builtin = "install_module kagami\nexit $?\n";
    assert(build_install_wrapper_script(false, custom).find(builtin) != std::string::npos);
    assert(!custom);
    assert(build_install_wrapper_script(true, custom).find(builtin) == std::string::npos);
    assert(!custom);

    const auto meta = fs::path(MODULE_DIR) / "meta";
    put(meta / "module.prop", "id=meta\nmetamodule=1\n");
    put(meta / "metamount.sh");
    assert(build_install_wrapper_script(false, custom).find(builtin) == std::string::npos);
    assert(!custom);

    put(meta / "metainstall.sh");
    const auto external = ". " + (meta / "metainstall.sh").string() + "\n";
    const auto script = build_install_wrapper_script(false, custom);
    assert(custom && script.find(external) != std::string::npos);
    assert(script.find(builtin) == std::string::npos);
    assert(build_install_wrapper_script(true, custom).find(external) == std::string::npos);
    assert(!custom);

    put(meta / "disable");
    assert(build_install_wrapper_script(false, custom).find(builtin) != std::string::npos);
    assert(!custom);
    fs::remove(meta / "disable");
    fs::remove(meta / "metainstall.sh");
    fs::remove(meta / "metamount.sh");
    assert(build_install_wrapper_script(false, custom).find(builtin) != std::string::npos);
    assert(!custom);
}

void check_uninstall_hooks() {
    using namespace ksud;
    reset_modules();
    assert(metamodule_exec_uninstall_script("removed-module") == 0);
    assert((builtin_uninstalls == std::vector<std::string>{"removed-module"}));
    builtin_uninstall_result = 7;
    assert(metamodule_exec_uninstall_script("removed-module") == 7);
    builtin_uninstall_result = 0;
    builtin_uninstalls.clear();

    const auto meta = fs::path(MODULE_DIR) / "meta";
    put(meta / "module.prop", "id=meta\nmetamodule=1\n");
    put(meta / "metamount.sh");
    assert(metamodule_exec_uninstall_script("removed-module") == 0);
    assert(builtin_uninstalls.empty());
    put(meta / "metauninstall.sh");
    assert(metamodule_exec_uninstall_script("removed-module") == 0);
    assert(builtin_uninstalls.empty());
    put(meta / "disable");
    assert(metamodule_exec_uninstall_script("removed-module") == 0);
    assert(builtin_uninstalls.size() == 1);

    reset_modules();
    put(fs::path(MODULE_DIR) / "removed-module/remove");
    assert(prune_modules() == 0);
    assert((builtin_uninstalls == std::vector<std::string>{"removed-module"}));
    assert(!fs::exists(fs::path(MODULE_DIR) / "removed-module"));
}

void check_batch_errors() {
    using namespace ksud;
    reset_modules();
    for (const char* name : {"alpha", "beta"}) {
        put(fs::path(MODULE_DIR) / name / "service.sh");
        put(fs::path(MODULE_DIR) / name / "system.prop", "example=1\n");
        put(fs::path(MODULE_UPDATE_DIR) / name / "updated");
        const auto common = fs::path("/data/adb/service.d") / name;
        put(common);
        assert(chmod(common.c_str(), 0755) == 0);
    }
    fail_fork = true;
    std::string error;
    assert(exec_stage_script("service", ScriptWait::forever(), &error) != 0);
    assert(forks == 1 && !error.empty());
    forks = 0;
    error.clear();
    assert(exec_common_scripts("service.d", ScriptWait::forever(), &error) != 0);
    assert(forks == 1 && !error.empty());
    fail_fork = false;
    put("/outside-props/system.prop", "linked=1\n");
    fs::create_directory_symlink("/outside-props", fs::path(MODULE_DIR) / "linked");
    assert(load_system_prop() == 0 && prop_writes == 3);
    prop_writes = 0;
    fail_props = true;
    assert(load_system_prop() != 0 && prop_writes == 1);
    fail_rename = true;
    assert(handle_updated_modules() != 0 && renames == 1);
    assert(fs::exists(fs::path(MODULE_UPDATE_DIR) / "alpha/updated"));
    assert(fs::exists(fs::path(MODULE_UPDATE_DIR) / "beta/updated"));
    fail_rename = false;
    assert(handle_updated_modules() == 0);

    reset_modules();
    put("/outside-updated/updated");
    fs::create_directory_symlink("/outside-updated", fs::path(MODULE_UPDATE_DIR) / "linked");
    assert(handle_updated_modules() == 0);
    assert(fs::is_symlink(fs::path(MODULE_DIR) / "linked"));
    assert(fs::exists(fs::path(MODULE_DIR) / "linked/updated"));
}

void check_prune_errors() {
    using namespace ksud;
    reset_modules();
    for (const char* name : {"alpha", "beta", "gamma"}) {
        put(fs::path(MODULE_DIR) / name / REMOVE_FILE_NAME);
        assert(chmod((fs::path(MODULE_DIR) / name).c_str(), 0777) == 0);
    }
    std::vector<fs::path> order;
    for (const auto& entry : fs::directory_iterator(MODULE_DIR))
        order.push_back(entry.path());
    const auto config = fs::path(MODULE_CONFIG_DIR) / order[0].filename();
    put(config / "blocked");
    assert(chmod(config.c_str(), 0555) == 0);
    assert(chmod(MODULE_CONFIG_DIR, 0777) == 0);
    assert(chmod(MODULE_DIR, 0777) == 0);
    assert(chmod(order[1].c_str(), 0555) == 0);
    assert(seteuid(1000) == 0);
    assert(prune_modules() == 0);
    assert(seteuid(0) == 0);
    assert(fs::exists(config) && !fs::exists(order[0]));
    assert(fs::exists(order[1]) && !fs::exists(order[2]));

    reset_modules();
    put("/outside-pruned/remove");
    put("/outside-pruned/keep");
    fs::create_directory_symlink("/outside-pruned", fs::path(MODULE_DIR) / "linked");
    assert(prune_modules() == 0);
    assert(!fs::is_symlink(fs::path(MODULE_DIR) / "linked"));
    assert(fs::exists("/outside-pruned/keep"));
}

void check_temp_configs() {
    reset_modules();
    const auto regular = fs::path(ksud::MODULE_CONFIG_DIR) / "regular";
    const fs::path linked = "/outside-config";
    for (const auto& dir : {regular, linked}) {
        put(dir / ksud::TEMP_CONFIG_NAME);
        put(dir / "keep");
    }
    fs::create_directory_symlink(linked, fs::path(ksud::MODULE_CONFIG_DIR) / "linked");
    ksud::clear_all_temp_configs();
    for (const auto& dir : {regular, linked}) {
        assert(!fs::exists(dir / ksud::TEMP_CONFIG_NAME));
        assert(fs::exists(dir / "keep"));
    }
}

int main() {
    // Privileged filesystem cases are confined to a disposable child chroot.
    if (geteuid() != 0)
        return 77;
    char directory[] = "/tmp/yukisu-module-lifecycle-XXXXXX";
    assert(mkdtemp(directory));
    const pid_t pid = __real_fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(chroot(directory) == 0 && chdir("/") == 0 && chmod("/", 0755) == 0);
        check_metamodule_paths();
        check_install_wrappers();
        check_uninstall_hooks();
        check_batch_errors();
        check_prune_errors();
        check_temp_configs();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    fs::remove_all(directory);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
