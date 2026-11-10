#include "magica.hpp"

#include "../../third_party/resetpropAlone/src/prop_area.hpp"
#include "../../third_party/resetpropAlone/src/property_contexts.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "adb_client.hpp"

#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ksud::magica {

namespace {

constexpr int kConnectRetries = 30;
constexpr int kRetryDelaySeconds = 1;
constexpr const char* kPropertyDir = "/dev/__properties__";

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...

bool run_shell_command(const std::vector<std::string>& args, const char* prefix) {
    const auto result = exec_command(args);
    if (result.exit_code != 0) {
        LOGE("%s failed: %s", prefix, args.front().c_str());
        if (!result.stdout_str.empty()) {
            LOGE("stdout: %s", result.stdout_str.c_str());
        }
        if (!result.stderr_str.empty()) {
            LOGE("stderr: %s", result.stderr_str.c_str());
        }
        return false;
    }
    return true;
}

bool reset_prop(const char* name, const char* value) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 5> argv_c = {
        const_cast<char*>("resetprop"),
        const_cast<char*>("-n"),
        const_cast<char*>(name),
        const_cast<char*>(value),
        nullptr,
    };
    const int rc = resetprop_main(4, argv_c.data());
    if (rc != 0) {
        LOGE("resetprop failed for %s=%s", name, value);
        return false;
    }
    return true;
#else
    LOGE("resetprop is not available in this ksud build");
    (void)name;
    (void)value;
    return false;
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
}

bool delete_prop(const char* name) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 4> argv_c = {
        const_cast<char*>("resetprop"),
        const_cast<char*>("-d"),
        const_cast<char*>(name),
        nullptr,
    };
    const int rc = resetprop_main(3, argv_c.data());
    if (rc != 0) {
        LOGE("resetprop delete failed for %s", name);
        return false;
    }
    return true;
#else
    LOGE("resetprop is not available in this ksud build");
    (void)name;
    return false;
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
}

void compact_prop_context_if_possible(const char* name) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::string context;
    std::string error;
    if (!resetprop::resolve_property_context_from(kPropertyDir, name, &context, &error)) {
        LOGW("Failed to resolve property context for %s: %s", name, error.c_str());
        return;
    }

    resetprop::CompactSummary summary;
    if (!resetprop::compact_property_areas(kPropertyDir, context, &summary, &error)) {
        LOGW("Failed to compact property area for %s (%s): %s", name, context.c_str(),
             error.c_str());
        return;
    }

    LOGI("Compacted property area for %s (%s): scanned=%zu valid=%zu compacted=%zu", name,
         context.c_str(), summary.files_scanned, summary.valid_areas, summary.files_compacted);
#else
    (void)name;
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
}

bool clear_prop_if_exists(const char* name) {
    const auto current = getprop(name);
    if (!current.has_value()) {
        LOGI("Property %s already absent", name);
        return true;
    }

    if (delete_prop(name)) {
        LOGI("Deleting %s", name);
        if (!getprop(name).has_value()) {
            compact_prop_context_if_possible(name);
            return true;
        }
        LOGW("Property %s still visible after delete, fallback to clearing", name);
    }

    LOGI("Clearing %s", name);
    if (!reset_prop(name, "")) {
        return false;
    }

    const auto updated = getprop(name);
    if (!updated.has_value()) {
        return true;
    }

    if (!updated->empty()) {
        LOGE("Property %s did not clear (current=%s)", name, updated->c_str());
        return false;
    }

    return true;
}

bool ensure_prop_value(const char* name, const char* expected) {
    const auto current = getprop(name);
    if (current && *current == expected) {
        LOGI("%s already set to %s", name, expected);
        return true;
    }

    LOGI("Setting %s to %s", name, expected);
    if (!reset_prop(name, expected)) {
        return false;
    }

    const auto updated = getprop(name);
    if (!updated || *updated != expected) {
        LOGE("Property %s did not update to %s (current=%s)", name, expected,
             updated ? updated->c_str() : "<missing>");
        return false;
    }

    return true;
}

bool enable_adb_root(uint16_t port) {
    if (geteuid() != 0) {
        LOGE("Magica bootstrap must run as root");
        return false;
    }

    if (!ensure_prop_value("ro.debuggable", "1") || !ensure_prop_value("ro.adb.secure", "0")) {
        return false;
    }

    const std::string port_str = std::to_string(port);

    const std::array<std::vector<std::string>, 3> adb_cmds = {
        std::vector<std::string>{"setprop", "service.adb.root", "1"},
        std::vector<std::string>{"setprop", "service.adb.tcp.port", port_str},
        std::vector<std::string>{"setprop", "ctl.restart", "adbd"},
    };

    return std::all_of(adb_cmds.begin(), adb_cmds.end(),
                       [](const auto& cmd) { return run_shell_command(cmd, "magica"); });
}

bool connect_to_adbd(AdbClient* client, uint16_t port) {
    for (int attempt = 1; attempt <= kConnectRetries; ++attempt) {
        LOGI("Waiting for adbd to restart... (%d/%d)", attempt, kConnectRetries);
        std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySeconds));
        if (client->connect_tcp("127.0.0.1", port)) {
            return true;
        }
        LOGW("ADB connection retry failed: %s", client->last_error().c_str());
    }

    return false;
}

}  // namespace

int run(uint16_t port, const std::string& package_name, bool allow_shell) {
    LOGI("Magica bootstrap triggered on tcp:%u", port);

    if (!enable_adb_root(port)) {
        return 1;
    }

    AdbClient client;
    if (!connect_to_adbd(&client, port)) {
        (void)disable_adb_root();
        return 1;
    }

    std::array<char, PATH_MAX> exe_path{};
    const ssize_t len = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (len < 0) {
        LOGE("Failed to resolve current executable: %s", strerror(errno));
        return 1;
    }
    exe_path[static_cast<size_t>(len)] = '\0';

    const auto quote = [](const std::string& value) {
        std::string result = "'";
        for (const char ch : value)
            result += ch == '\'' ? "'\\''" : std::string(1, ch);
        return result + "'";
    };
    std::string command =
        quote(exe_path.data()) + " late-load --post-magica --package-name " + quote(package_name);
    if (allow_shell) {
        command += " --allow-shell";
    }
    std::string output;
    if (!client.shell_command(command, &output)) {
        LOGI("adb shell finished with error (may be expected): %s", client.last_error().c_str());
    }

    if (!output.empty()) {
        LOGI("adb shell output:\n%s", output.c_str());
    }

    return 0;
}

int disable_adb_root() {
    bool ok = ensure_prop_value("ro.debuggable", "0") && ensure_prop_value("ro.adb.secure", "1");

    for (const char* prop : {"service.adb.root", "service.adb.tcp.port", "ro.boot.selinux"}) {
        if (!clear_prop_if_exists(prop)) {
            ok = false;
        }
    }

    const std::array<std::vector<std::string>, 1> cleanup_cmds = {
        std::vector<std::string>{"setprop", "ctl.restart", "adbd"},
    };

    for (const auto& cmd : cleanup_cmds) {
        if (!run_shell_command(cmd, "magica cleanup")) {
            ok = false;
        }
    }

    return ok ? 0 : 1;
}

}  // namespace ksud::magica
