#include "core/ksucalls.hpp"
#include "core/log.hpp"
#include "core/reboot_state.hpp"
#include "core/runtime.hpp"
#include "init_event.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "mount/kasumi.hpp"
#include "uapi/kasumi.h"
#include "utils.hpp"

#include <unistd.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::filesystem::path root;
bool prepare_ok = true;
bool deny_config_read = false;
bool scripts_ok = true;
bool uapi_ok = true;
std::string expected_stage = "boot-completed";
int runtime_enabled = 1;
int preparations = 0;
std::vector<std::string> callbacks;
}  // namespace

namespace ksud {
bool ensure_uapi_version_matched([[maybe_unused]] std::string* error) {
    return uapi_ok;
}
void hide_bootloader_status() {
    callbacks.emplace_back("bootloader");
}
bool is_safe_mode() {
    return false;
}
bool has_magisk() {
    return false;
}
void umask([[maybe_unused]] mode_t mask) {}
void log_i([[maybe_unused]] const char* format, ...) {}
void log_e([[maybe_unused]] const char* format, ...) {}
void log_w([[maybe_unused]] const char* format, ...) {}
int ksuctl([[maybe_unused]] int request, void* arg) {
    static_cast<GetFeatureCmd*>(arg)->value = 1;
    return 0;
}
void report_boot_complete() {
    callbacks.emplace_back("kernel");
}
void ensure_msud_running_if_enabled() {
    callbacks.emplace_back("msud");
}
namespace preinit {
void on_boot_completed() {
    callbacks.emplace_back("preinit");
}
}  // namespace preinit
int exec_common_scripts(const std::string& stage, ScriptWait wait,
                        [[maybe_unused]] std::string* error) {
    assert(stage == expected_stage + ".d" && wait.mode == ScriptWait::Mode::NoWait);
    callbacks.emplace_back("common");
    return 0;
}
int metamodule_exec_stage_script(const std::string& stage, ScriptWait wait,
                                 [[maybe_unused]] std::string* error) {
    assert(stage == expected_stage && wait.mode == ScriptWait::Mode::NoWait);
    callbacks.emplace_back("metamodule");
    return 0;
}
int exec_stage_script(const std::string& stage, ScriptWait wait,
                      [[maybe_unused]] std::string* error) {
    assert(stage == expected_stage && wait.mode == ScriptWait::Mode::NoWait);
    callbacks.emplace_back("modules");
    return scripts_ok ? 0 : 1;
}
bool exec_plugin_stage(const std::string& stage, bool block) {
    assert(stage == expected_stage && !block);
    callbacks.emplace_back("plugins");
    return true;
}
std::optional<std::string> read_file(const std::string& path) {
    if (deny_config_read && path == (root / "config.json").string()) {
        errno = EACCES;
        return std::nullopt;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    std::string content;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            const int error = errno;
            close(fd);
            errno = error;
            return count == 0 ? std::optional<std::string>(content) : std::nullopt;
        }
        content.append(buffer.data(), static_cast<size_t>(count));
    }
}
bool write_file_atomic(const std::filesystem::path& path, const std::string& content) {
    const auto temporary = path.string() + ".test-tmp";
    std::ofstream out(temporary);
    if (!(out << content))
        return false;
    out.close();
    return rename(temporary.c_str(), path.c_str()) == 0;
}
}  // namespace ksud

namespace kagami {
std::filesystem::path runtime_data_dir() {
    return root;
}
std::filesystem::path runtime_config_file() {
    return root / "config.json";
}
std::string runtime_boot_id() {
    return "boundary-boot";
}
bool prepare_runtime(std::string& error) {
    ++preparations;
    if (!prepare_ok) {
        error = "runtime directory is unavailable";
        return false;
    }
    std::filesystem::create_directories(root / "run");
    return true;
}
void embedded_boot_completed() {
    callbacks.emplace_back("kagami");
}
namespace logging {
void set_debug_enabled([[maybe_unused]] bool value) noexcept {}
void write([[maybe_unused]] Level level, [[maybe_unused]] const std::string& component,
           [[maybe_unused]] const std::string& message) noexcept {}
}  // namespace logging
namespace mount {
ModuleModeMap load_module_modes([[maybe_unused]] const char* file) {
    return {};
}
}  // namespace mount
namespace kasumi {
bool is_available() {
    return true;
}
int enabled_state() {
    return runtime_enabled;
}
VersionInfo version_info() {
    VersionInfo info;
    info.status = Status::Available;
    return info;
}
FeatureCapabilities feature_capabilities() {
    return {true, 0, KSM_FEATURE_SAFE_RULE_RETIRE | KSM_FEATURE_MANAGED_HIDE};
}
bool user_hide_rules(std::vector<UserHideRule>& rules) {
    rules.clear();
    return true;
}
}  // namespace kasumi
}  // namespace kagami

int main() {
    using namespace kagami;
    char directory[] = "/tmp/yukisu-soft-reboot-boundary-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    Config config;
    std::string error;
    int pending = 42;

    assert(!read_config_file(config, error));
    config.kasumi_enabled = false;
    assert(read_config_file(config, error, MissingConfig::UseDefaults));
    assert(config.kasumi_enabled && error.empty());
    assert(reboot::check_support(error));
    assert(mount::kasumi::verify_persisted_hide_rules(pending, error));
    assert(pending == 0 && error.empty());
    assert(!std::filesystem::exists(root / "config.json"));
    assert(!std::filesystem::exists(root / "run"));

    runtime_enabled = 0;
    assert(!mount::kasumi::verify_persisted_hide_rules(pending, error));
    runtime_enabled = 1;
    for (const auto* contents : {"invalid", "[]"}) {
        std::ofstream(root / "config.json") << contents;
        errno = ENOENT;
        assert(!read_config_file(config, error, MissingConfig::UseDefaults));
        assert(!reboot::check_support(error));
        assert(!mount::kasumi::verify_persisted_hide_rules(pending, error));
    }
    std::ofstream(root / "config.json") << "{\"kasumi_enabled\":false}";
    runtime_enabled = 0;
    assert(reboot::check_support(error));
    assert(mount::kasumi::verify_persisted_hide_rules(pending, error));
    deny_config_read = true;
    assert(!read_config_file(config, error, MissingConfig::UseDefaults));
    assert(!reboot::check_support(error));
    assert(!mount::kasumi::verify_persisted_hide_rules(pending, error));
    deny_config_read = false;
    std::filesystem::remove(root / "config.json");

    assert(ksud::on_boot_completed() == 0);
    const std::vector<std::string> expected = {"kernel", "preinit",    "kagami",  "msud",
                                               "common", "metamodule", "modules", "plugins"};
    assert(callbacks == expected && preparations == 1);
    const auto marker = root / "run" / "boot_completed_cycle";
    assert(ksud::read_file(marker.string()) == "boundary-boot\ncomplete\n");
    assert(ksud::on_boot_completed() == 0 && callbacks == expected);
    assert(!std::filesystem::exists(root / "config.json"));

    std::filesystem::remove_all(root / "run");
    callbacks.clear();
    prepare_ok = false;
    assert(ksud::on_boot_completed() != 0 && callbacks.empty());
    prepare_ok = true;
    scripts_ok = false;
    assert(ksud::on_boot_completed() != 0 && callbacks == expected);
    assert(ksud::read_file(marker.string()) == "boundary-boot\nfailed\n");
    assert(ksud::on_boot_completed() != 0 && callbacks == expected);

    // Late-load still runs after a failed boot marker and without boot-event admission.
    scripts_ok = true;
    prepare_ok = false;
    uapi_ok = false;
    for (int i = 0; i < 2; ++i) {
        callbacks.clear();
        assert(ksud::run_boot_completed_stage() == 0 && callbacks == expected);
        assert(ksud::read_file(marker.string()) == "boundary-boot\nfailed\n");
        callbacks.clear();
        expected_stage = "service";
        assert(ksud::run_services_stage() == 0);
        assert((callbacks == std::vector<std::string>{"bootloader", "common", "metamodule",
                                                      "modules", "plugins"}));
        expected_stage = "boot-completed";
    }
    std::filesystem::remove_all(root);
    std::cout << "soft reboot boundaries: missing runtime, default config and errors passed\n";
}
