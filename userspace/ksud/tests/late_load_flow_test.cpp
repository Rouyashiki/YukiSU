#include "../src/assets.hpp"
#include "../src/boot/boot_patch.hpp"
#include "../src/core/feature.hpp"
#include "../src/core/ksucalls.hpp"
#include "../src/core/restorecon.hpp"
#include "../src/core/uts_view.hpp"
#include "../src/init_event.hpp"
#include "../src/kernelsu_loader.hpp"
#include "../src/late_load.hpp"
#include "../src/magica/magica.hpp"
#include "../src/module/hot_install.hpp"
#include "../src/module/metamodule.hpp"
#include "../src/module/module.hpp"
#include "../src/module/module_config.hpp"
#include "../src/profile/profile.hpp"
#include "../src/umount.hpp"
#include "../src/utils.hpp"
#include "../src/yukizygisk_diagnostics.hpp"

#include <cassert>

namespace {
ksud::ProcessDaemonizeResult daemon_result;
bool driver_ready = false;
bool load_ok = true;
bool stdio_ok = true;
int install_result = 0;
int mount_result = 0;
int cleanup = 0;
int loads = 0;
std::vector<std::string> stages;
std::vector<std::vector<std::string>> commands;
}  // namespace

namespace ksud {
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
std::optional<std::string> read_file(const std::string&) {
    return std::nullopt;
}
std::string trim(const std::string& value) {
    return value;
}
ProcessDaemonizeResult daemonize_process(bool use_init_pgrp) {
    assert(!use_init_pgrp);
    return daemon_result;
}
bool has_kernelsu() {
    return driver_ready;
}
bool reset_stdio_to_devnull() {
    return stdio_ok;
}
std::string get_bootstrap_kmi() {
    return "android15-6.6";
}
std::optional<std::vector<uint8_t>> get_asset_data(const std::string& name) {
    assert(name == "android15-6.6_kernelsu.ko");
    return std::vector<uint8_t>{1, 2, 3};
}
namespace kernelsu_loader {
bool load_module(std::vector<uint8_t> image, const std::string& params, bool lookup) {
    assert((image == std::vector<uint8_t>{1, 2, 3}) && params == "allow_shell=1" && !lookup);
    ++loads;
    driver_ready = load_ok;
    return load_ok;
}
}  // namespace kernelsu_loader
namespace magica {
int disable_adb_root() {
    return ++cleanup, 0;
}
}  // namespace magica
void umask(mode_t) {}
bool has_magisk() {
    return false;
}
bool is_safe_mode() {
    return false;
}
void clear_all_temp_configs() {}
int install(const std::optional<std::string>&, const std::optional<std::string>&) {
    return install_result;
}
void clean_hot_install_trash() {}
int handle_updated_modules() {
    return 0;
}
int prune_modules() {
    return 0;
}
bool restorecon() {
    return true;
}
int load_sepolicy_rule() {
    return 0;
}
int load_system_prop() {
    stages.emplace_back("system.prop");
    return 0;
}
int apply_profile_sepolies() {
    return 0;
}
int apply_uts_view_config() {
    return 0;
}
int init_features() {
    return 0;
}
std::pair<uint64_t, bool> get_feature(uint32_t) {
    return {0, true};
}
bool prepare_yukizygisk_diagnostics(bool) {
    return true;
}
void update_yukizygisk_boot_diagnostics(bool, bool, bool, const char*) {}
int exec_common_scripts(const std::string& stage, ScriptWait, std::string*) {
    stages.push_back(stage);
    return 0;
}
int metamodule_exec_stage_script(const std::string&, ScriptWait, std::string*) {
    return 0;
}
int exec_stage_script(const std::string&, ScriptWait, std::string*) {
    return 0;
}
int metamodule_exec_mount_script(std::string*) {
    stages.emplace_back("mount");
    return mount_result;
}
int umount_apply_config() {
    return 0;
}
int refresh_sucompat_vfs() {
    return 0;
}
int run_services_stage(std::string*) {
    stages.emplace_back("service");
    return 0;
}
int run_boot_completed_stage() {
    stages.emplace_back("boot-completed");
    return 0;
}
ExecResult exec_command(const std::vector<std::string>& args) {
    commands.push_back(args);
    return {0, "", ""};
}
}  // namespace ksud

int main() {
    using namespace ksud;
    const auto run = [](bool magica = true) {
        stages.clear();
        commands.clear();
        return late_load::run("com.anatdx.yukisu", magica, true);
    };
    daemon_result = ProcessDaemonizeResult::Parent;
    assert(run() == 0 && cleanup == 0 && loads == 0 && stages.empty());
    daemon_result = ProcessDaemonizeResult::Error;
    assert(run(false) == 1 && cleanup == 0 && loads == 0);
    assert(run() == 1 && cleanup == 1 && loads == 0);
    daemon_result = ProcessDaemonizeResult::Daemon;
    for (int i = 0; i < 2; ++i) {
        assert(run() == 0 && loads == 1 && cleanup == i + 2);
        assert((stages == std::vector<std::string>{"late-load.d", "system.prop", "mount",
                                                   "post-mount.d", "service", "boot-completed"}));
        assert(commands.size() == 2 && commands[0][1] == "force-stop" &&
               commands[1][3] == "com.anatdx.yukisu/com.anatdx.yukisu.ui.MainActivity");
    }
    mount_result = 1;
    assert(run(false) == 0 && cleanup == 3);
    assert((stages == std::vector<std::string>{"late-load.d", "system.prop", "mount",
                                               "post-mount.d", "service", "boot-completed"}));
    mount_result = 0;
    install_result = 1;
    assert(run() == 1 && cleanup == 4 && stages.empty() && commands.empty());
    stdio_ok = false;
    assert(run() == 1 && cleanup == 5 && stages.empty());
    driver_ready = false;
    load_ok = false;
    assert(run() == 1 && cleanup == 6 && loads == 2 && stages.empty());
}
