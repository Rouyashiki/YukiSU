#include "../kagami/include/kagami/config.hpp"
#include "../kagami/include/kagami/kasumi_client.hpp"
#include "../kagami/src/core/reboot_state.hpp"
#include "../kagami/src/core/runtime.hpp"
#include "../src/core/ksucalls.hpp"
#include "uapi/kasumi.h"

#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::filesystem::path root;
std::string boot = "test-boot";
bool write_ok = true, feature_ok = true, uapi_ok = true;
bool configured_kasumi = true;
bool safe_mode = false;
bool magisk = false;
int runtime_enabled = 1;
int capabilities = KSM_FEATURE_SAFE_RULE_RETIRE | KSM_FEATURE_MANAGED_HIDE;
kagami::kasumi::Status protocol = kagami::kasumi::Status::Available;
std::vector<std::string> hides = {"/storage/emulated/0/test"};
}  // namespace

namespace ksud {
bool is_safe_mode() {
    return safe_mode;
}
bool has_magisk() {
    return magisk;
}
bool ensure_uapi_version_matched(std::string* error) {
    if (!uapi_ok && error)
        *error = "UAPI mismatch";
    return uapi_ok;
}
int ksuctl(int, void* arg) {
    if (!feature_ok)
        return -1;
    auto* feature = static_cast<GetFeatureCmd*>(arg);
    feature->value = runtime_enabled > 0 ? 1 : 0;
    return 0;
}
bool write_file_atomic(const std::filesystem::path& path, const std::string& content) {
    if (!write_ok)
        return false;
    const auto temporary = path.string() + ".test-tmp";
    std::ofstream(temporary) << content;
    std::filesystem::rename(temporary, path);
    return true;
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
    return boot;
}
bool prepare_runtime(std::string&) {
    std::filesystem::create_directories(root / "run");
    return true;
}
bool read_config_file(Config& config, [[maybe_unused]] std::string& error,
                      [[maybe_unused]] MissingConfig missing) {
    config.kasumi_enabled = configured_kasumi;
    return true;
}
std::vector<std::string> load_user_hide_rules() {
    return hides;
}
namespace kasumi {
VersionInfo version_info() {
    VersionInfo info;
    info.status = protocol;
    info.last_errno = ENODEV;
    return info;
}
int enabled_state() {
    return runtime_enabled;
}
FeatureCapabilities feature_capabilities() {
    return {true, 0, capabilities};
}
}  // namespace kasumi
}  // namespace kagami

int main() {
    using namespace kagami;
    char directory[] = "/tmp/yukisu-soft-reboot-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    std::string error;
    assert(reboot::check_support(error));
    assert(!std::filesystem::exists(root / "run"));
    capabilities = KSM_FEATURE_MANAGED_HIDE;
    assert(!reboot::check_support(error));
    capabilities = KSM_FEATURE_SAFE_RULE_RETIRE;
    assert(!reboot::check_support(error));
    hides.clear();
    assert(reboot::check_support(error));
    runtime_enabled = -1;
    assert(!reboot::check_support(error));
    runtime_enabled = 1;
    protocol = kasumi::Status::KernelTooOld;
    assert(!reboot::check_support(error));
    protocol = kasumi::Status::NotPresent;
    runtime_enabled = -1;
    assert(reboot::check_support(error));
    protocol = kasumi::Status::Available;
    runtime_enabled = 1;
    feature_ok = false;
    assert(!reboot::check_support(error));
    feature_ok = true;
    uapi_ok = false;
    assert(!reboot::check_support(error));
    uapi_ok = true;
    safe_mode = true;
    assert(!reboot::check_support(error));
    safe_mode = false;
    magisk = true;
    assert(!reboot::check_support(error));
    magisk = false;
    for (const auto* name : {"user_hide_rules.json", "hide_bypass.json", "module_bypass.json",
                             "module_mode.json", "module_rules.json"}) {
        std::ofstream(root / name) << "invalid";
        assert(!reboot::check_support(error));
        std::filesystem::remove(root / name);
    }
    std::ofstream(root / "user_hide_rules.json") << "[123]";
    assert(!reboot::check_support(error));
    std::ofstream(root / "user_hide_rules.json") << "[\"/storage/emulated/0/test\"]";
    std::ofstream(root / "module_rules.json")
        << R"({"module-a":[{"path":"/system/test","mode":"hide","bypass":true}]})";
    assert(reboot::check_support(error));

    assert(reboot::acquire(error));
    std::ofstream(root / "run" / "mount_disabled") << '\n';
    assert(!reboot::check_support(error));
    std::filesystem::remove(root / "run" / "mount_disabled");
    assert(reboot::active() && reboot::authorized());
    assert(!reboot::acquire(error));
    const auto marker = root / "run" / "plan";
    assert(reboot::cycle_id() == boot);
    assert(reboot::write_cycle_marker(marker, "module-a\n"));
    assert(reboot::marker_matches_cycle(marker));
    assert(reboot::advance_cycle(error));
    assert(reboot::cycle_id() == boot + ":1");
    assert(!reboot::marker_matches_cycle(marker));
    write_ok = false;
    assert(!reboot::advance_cycle(error));
    assert(reboot::cycle_id() == boot + ":1");
    write_ok = true;
    assert(reboot::advance_cycle(error));
    assert(reboot::cycle_id() == boot + ":2");

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        reboot::release();
        assert(reboot::active());
        assert(!reboot::acquire(error));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(reboot::record("rebuild", "accepted"));
    reboot::release();
    assert(!reboot::active());
    int recovery = 0;
    assert(reboot::recover_abandoned([&](const std::string& phase, std::string& recovery_error) {
        assert(phase == "rebuild");
        assert(reboot::active() && reboot::authorized());
        const pid_t competitor = fork();
        assert(competitor >= 0);
        if (competitor == 0) {
            reboot::release();
            assert(reboot::active());
            assert(!reboot::acquire(error));
            _exit(0);
        }
        int competitor_status;
        assert(waitpid(competitor, &competitor_status, 0) == competitor);
        assert(WIFEXITED(competitor_status) && WEXITSTATUS(competitor_status) == 0);
        write_ok = false;
        assert(reboot::claim_services(recovery_error) == -1);
        write_ok = true;
        assert(reboot::claim_services(recovery_error) == 1);
        assert(reboot::claim_services(recovery_error) == 0);
        ++recovery;
        return true;
    }));
    assert(recovery == 1 && reboot::state().at("result").s == "failed");
    assert(!reboot::active() && !reboot::authorized() && reboot::client_token().empty());
    assert(reboot::state().at("services_started").bool_or(false));
    assert(reboot::claim_services(error) == -1);
    assert(!reboot::recover_abandoned([&](const std::string&, std::string&) {
        ++recovery;
        return true;
    }));
    assert(recovery == 1);
    assert(!reboot::record("complete", "completed"));
    assert(reboot::acquire(error));
    assert(reboot::claim_services(error) == 1);
    assert(reboot::claim_services(error) == 0);
    assert(reboot::record("services", "accepted"));
    reboot::release();
    assert(reboot::recover_abandoned([&](const std::string&, std::string& recovery_error) {
        assert(reboot::claim_services(recovery_error) == 0);
        recovery_error = "service exec failed";
        return false;
    }));
    assert(reboot::state().at("error").s.find("service exec failed") != std::string::npos);
    boot = "next-boot";
    assert(reboot::cycle_id() == boot);
    assert(!reboot::marker_matches_cycle(marker));
    std::ofstream(root / "run" / "mount_cycle") << "invalid";
    assert(reboot::cycle_id().empty());
    assert(!reboot::check_support(error));
    std::filesystem::remove_all(root);
    std::cout
        << "soft reboot state: capability gates, cycles, locks, and abandoned recovery passed\n";
}
