#include "cli_args.hpp"
#include "../kagami/include/kagami/config.hpp"
#include "terminal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>
#include "core/json.hpp"

namespace ksud {
namespace {

struct Option {
    std::string_view name;
    std::string_view alias;
    std::string_view value;
    std::string_view default_value;
    std::string_view description;
    bool repeatable = false;
    bool allow_empty = false;
};

struct Command {
    std::string_view path;
    std::string_view description;
    std::string_view usage;
    size_t minimum = 0;
    size_t maximum = 0;
    std::vector<Option> options;
    bool forward_options = false;
};

constexpr size_t kTrailingArgs = std::numeric_limits<size_t>::max();
const Option kTemp{"--temp", "-t", "", ""};
const Option kSlot{"--slot", "", "a|b|_a|_b", ""};
const Option kFlags{"--flags", "-f", "UINT32", ""};
const Option kJson{"--json", "", "", ""};
const Option kOta{"--ota", "-u", "", ""};
const Option kMagiskboot{"--magiskboot", "", "PATH", ""};

std::vector<Option> uts_options() {
    static constexpr std::array<std::string_view, 6> names = {
        "--sysname", "--nodename", "--release", "--version", "--machine", "--domainname"};
    std::vector<Option> result;
    result.reserve(names.size() + 1);
    for (const auto name : names)
        result.push_back(
            {name, "", "TEXT", "", "Override this UTS field (at most 64 bytes)", false, true});
    result.push_back({"--inherit", "", "FIELD", "",
                      "Restore a field: sysname, nodename, release, version, machine, domainname",
                      true});
    return result;
}

const std::vector<Command>& commands() {
    static const std::vector<Command> specs = {
        {"", "YukiSU userspace daemon", "<COMMAND>"},
        {"module", "Manage modules", "<COMMAND>"},
        {"module install", "Install a module archive", "<ZIP>", 1, 1},
        {"module uninstall", "Mark a module for removal", "<ID>", 1, 1},
        {"module undo-uninstall", "Cancel module removal", "<ID>", 1, 1},
        {"module enable", "Enable a module", "<ID>", 1, 1},
        {"module disable", "Disable a module", "<ID>", 1, 1},
        {"module action", "Run a module action", "<ID>", 1, 1},
        {"module list", "List modules as JSON", ""},
        {"module config", "Manage configuration in a module's environment", "<COMMAND>"},
        {"module config get", "Read a configuration value", "<KEY>", 1, 1},
        {"module config set", "Write a configuration value", "<KEY> <VALUE>", 2, 2, {kTemp}},
        {"module config list", "List configuration values", ""},
        {"module config delete", "Delete a configuration value", "<KEY>", 1, 1, {kTemp}},
        {"module config clear", "Clear configuration values", "", 0, 0, {kTemp}},
        {"feature", "Manage kernel features", "<COMMAND>"},
        {"feature get", "Read a feature value", "<ID>", 1, 1},
        {"feature set", "Set a feature value", "<ID> <VALUE>", 2, 2},
        {"feature set-save", "Set and persist a feature atomically", "<ID> <VALUE>", 2, 2},
        {"feature list", "List features", ""},
        {"feature check", "Check feature support", "<ID>", 1, 1},
        {"feature load", "Apply saved feature configuration", ""},
        {"feature save", "Persist current feature configuration", ""},
        {"install",
         "Install userspace components",
         "",
         0,
         0,
         {kMagiskboot, {"--libadbroot", "", "PATH", ""}}},
        {"uninstall", "Uninstall YukiSU", "", 0, 0, {kMagiskboot}},
        {"insmod", "Load a kernel module with kallsyms access", "<KO> [PARAMS...]", 1,
         kTrailingArgs},
        {"late-load",
         "Load kernelsu.ko and execute late-load stages",
         "",
         0,
         0,
         {{"--magica", "", "PORT", "5555"},
          {"--post-magica", "", "", ""},
          {"--allow-shell", "", "", ""}}},
        {"post-fs-data", "Trigger post-fs-data", ""},
        {"services", "Trigger service scripts", ""},
        {"boot-completed", "Trigger boot-completed", ""},
        {"soft-reboot", "Restart Android and reapply module stages", ""},
        {"sepolicy", "Manage SELinux policy", "<COMMAND>"},
        {"sepolicy patch", "Apply a policy statement", "<POLICY>", 1, 1},
        {"sepolicy apply", "Apply policy from a file", "<FILE>", 1, 1},
        {"sepolicy check", "Check a policy statement", "<POLICY>", 1, 1},
        {"profile", "Manage app profiles", "<COMMAND>"},
        {"profile get-sepolicy", "Read an app's policy", "<PACKAGE>", 1, 1},
        {"profile set-sepolicy", "Write an app's policy", "<PACKAGE> <POLICY>", 2, 2},
        {"profile get-template", "Read a profile template", "<ID>", 1, 1},
        {"profile set-template", "Write a profile template", "<ID> <TEMPLATE>", 2, 2},
        {"profile delete-template", "Delete a profile template", "<ID>", 1, 1},
        {"profile list-templates", "List profile templates", ""},
        {"boot-info", "Inspect boot information", "<COMMAND>"},
        {"boot-info current-kmi", "Read the running kernel's KMI", ""},
        {"boot-info target-kmi",
         "Read the patch target's KMI",
         "",
         0,
         0,
         {kOta, {"--boot", "", "PATH", ""}}},
        {"boot-info supported-kmis", "List embedded KMIs", ""},
        {"boot-info is-ab-device", "Check A/B support", ""},
        {"boot-info default-partition", "Read the default boot partition", ""},
        {"boot-info available-partitions", "List boot partitions", ""},
        {"boot-info slot-suffix", "Read the slot suffix", "", 0, 0, {kOta}},
        {"ramdisk-editor", "Run a ramdisk editor protocol session", "<RAMDISK.CPIO>", 1, 1},
        {"boot-ramdisk-editor", "Edit a boot image's ramdisk", "<SOURCE.IMG> <OUTPUT.IMG>", 2, 2},
        {"umount", "Manage unmount paths", "<COMMAND>"},
        {"umount add", "Register an unmount path", "<MOUNT>", 1, 1, {kFlags}},
        {"umount del", "Delete an unmount path", "<MOUNT>", 1, 1},
        {"umount remove", "Alias for umount del", "<MOUNT>", 1, 1},
        {"umount list", "List unmount paths", ""},
        {"umount save", "Save the kernel unmount list", ""},
        {"umount apply", "Apply the saved unmount list", ""},
        {"umount clear-custom", "Clear custom unmount paths", ""},
        {"kernel", "Access kernel interfaces", "<COMMAND>"},
        {"kernel nuke-ext4-sysfs", "Hide ext4 sysfs entries", "<MOUNT>", 1, 1},
        {"kernel notify-module-mounted", "Report completed module mounting", ""},
        {"kernel umount", "Manage the kernel unmount list", "<COMMAND>"},
        {"kernel umount add", "Register an unmount path", "<MOUNT>", 1, 1, {kFlags}},
        {"kernel umount del", "Delete an unmount path", "<MOUNT>", 1, 1},
        {"kernel umount wipe", "Clear the kernel unmount list", ""},
        {"debug", "Developer tools", "<COMMAND>"},
        {"debug set-manager", "Set the manager package", "[PACKAGE]", 0, 1},
        {"debug insmod", "Alias for insmod", "<KO> [PARAMS...]", 1, kTrailingArgs},
        {"debug su", "Open a root shell", "", 0, 0, {{"-g", "--global-mnt", "", ""}}},
        {"debug version", "Read the kernel version", ""},
        {"debug info", "Read kernel compatibility and load information", ""},
        {"debug sulogd", "Launch the su log daemon", ""},
        {"debug mark", "Manage process marks", "<COMMAND>"},
        {"debug mark get", "Read process marks (0 means all)", "[PID]", 0, 1},
        {"debug mark mark", "Mark a process (0 means all)", "[PID]", 0, 1},
        {"debug mark unmark", "Unmark a process (0 means all)", "[PID]", 0, 1},
        {"debug mark refresh", "Refresh process marks", ""},
        {"flash", "Flash and inspect partition images", "<COMMAND>"},
        {"flash ak3",
         "Flash an AnyKernel3 archive",
         "<ZIP>",
         1,
         1,
         {kSlot,
          {"--log", "", "FILE", ""},
          {"--use-mkbootfs", "", "", ""},
          {"--verbose", "-v", "", ""}}},
        {"flash ak3-info", "Inspect an AnyKernel3 archive", "<ZIP>", 1, 1},
        {"flash image", "Flash an image to a partition", "<IMAGE> <PARTITION>", 2, 2, {kSlot}},
        {"flash backup", "Back up a partition", "<PARTITION> <OUTPUT>", 2, 2, {kSlot}},
        {"flash list", "List partitions", "", 0, 0, {kSlot, {"--all", "", "", ""}}},
        {"flash info", "Inspect a partition", "<PARTITION>", 1, 1, {kSlot}},
        {"flash slots", "Show A/B slot information", ""},
        {"flash map", "Map logical partitions for a slot", "<SLOT>", 1, 1},
        {"flash avb", "Show AVB/dm-verity status", "[disable]", 0, 1},
        {"flash kernel", "Read the kernel version", "", 0, 0, {kSlot}},
        {"flash boot-info", "Read boot slot information", ""},
        {"initrc", "Manage init.rc injection", "<COMMAND>"},
        {"initrc refresh", "Regenerate preinit modules.rc", ""},
        {"sulogd", "Run the su log daemon", ""},
        {"msud", "Run the su prompt daemon", "", 0, 0, {{"--ready-fd", "", "FD", ""}}},
        {"magisk-compat", "Manage the compatible su prompt", "<COMMAND>"},
        {"magisk-compat apply", "Apply the configured prompt state", ""},
        {"su-path", "Manage the persistent Kasumi su path", "<COMMAND>"},
        {"su-path get", "Read the active su path", ""},
        {"su-path set", "Set and persist the su path", "<ABSOLUTE-PATH>", 1, 1, {kJson}},
        {"su-path reset", "Restore the default su path", ""},
        {"yzctl", "Control YukiZygisk", "<COMMAND>"},
        {"yzctl status", "Read runtime state", "", 0, 0, {kJson}},
        {"yzctl ensure-daemon",
         "Start missing required daemons",
         "",
         0,
         0,
         {{"--abi", "", "32|64|all", ""}},
         true},
        {"yzctl reload", "Reload daemon configuration", ""},
        {"yzctl config", "Manage YukiZygisk configuration", "<COMMAND>"},
        {"yzctl config get", "Read configuration as JSON", ""},
        {"yzctl config set", "Save and reload configuration", "<KEY> <VALUE>...", 2, kTrailingArgs},
        {"yzctl refresh-snapshot", "Rebuild the native snapshot", ""},
        {"version", "Show version information", ""},
        {"boot-patch",
         "Patch a boot image",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", "", "Read this image instead of the detected boot partition"},
          {"--flash", "-f", "", "", "Write the patched result to the selected partition"},
          {"--out", "-o", "DIRECTORY", "", "Write the patched image into this directory"},
          {"--out-name", "", "NAME", "", "Name of the patched output image"},
          {"--module", "-m", "KO", "", "Use this LKM instead of an embedded one"},
          {"--kernel", "-k", "IMAGE", "", "Replace the kernel image"},
          {"--init", "-i", "FILE", "", "Replace init"},
          kOta,
          {"--partition", "", "boot|init_boot", "", "Select the boot partition"},
          {"--kmi", "", "KMI", "", "Override the detected KMI"},
          {"--backup", "", "", "", "Back up the stock image"},
          {"--superkey", "-s", "KEY", "", "Set the SuperKey"},
          {"--signature-bypass", "", "", "", "Relax LKM signature checking"},
          {"--allow-shell", "", "", "", "Keep a root shell available"},
          {"--no-custom-rc", "", "", "", "Skip custom init.rc injection"},
          {"--enable-adbd", "", "", "", "Run adbd as root"},
          {"--adb-debug-prop", "", "FILE", "", "Embed adb debug properties from this file"},
          {"--uts-config", "", "FILE", "", "Embed a UTS template"},
          kMagiskboot},
         true},
        {"boot-restore",
         "Restore a boot image",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", "", "Read this image instead of the detected boot partition"},
          {"--flash", "-f", "", "", "Write the restored result back to the partition"},
          {"--out-name", "", "NAME", "", "Name of the restored output image"},
          kMagiskboot},
         true},
        {"boot-patch-v2",
         "Patch a boot image with direct LKM injection",
         "",
         0,
         0,
         {{"--boot", "-b", "IMAGE", ""},
          {"--module", "-m", "KO", ""},
          {"--output", "-o", "IMAGE", ""},
          {"--out", "", "IMAGE", ""},
          kMagiskboot,
          {"--superkey", "", "KEY", ""},
          {"--uts-config", "", "FILE", ""},
          {"--force", "", "", ""},
          {"--flash", "", "", ""},
          {"--ota", "", "", ""},
          {"--allow-shell", "", "", ""},
          {"--enable-adbd", "", "", ""},
          {"--signature-bypass", "", "", ""},
          {"--no-reuse", "", "", ""}}},
        {"plugin", "Manage Lua plugins", "<COMMAND>"},
        {"plugin install", "Install a Lua plugin archive", "<ZIP>", 1, 1},
        {"plugin uninstall", "Uninstall a Lua plugin", "<ID>", 1, 1},
        {"plugin enable", "Enable a Lua plugin", "<ID>", 1, 1},
        {"plugin disable", "Disable a Lua plugin", "<ID>", 1, 1},
        {"plugin list", "List Lua plugins", ""},
        {"plugin run", "Run a plugin callback", "<ID> <CALLBACK>", 2, 2},
        {"plugin action", "Run a plugin action", "<ID>", 1, 1},
        {"plugin daemon",
         "Run a plugin daemon callback",
         "<ID> <CALLBACK> <INTERVAL>",
         3,
         3,
         {{"--ready-fd", "", "FD", ""}}},
        {"plugin log", "Read a plugin log", "<ID>", 1, 1},
        {"plugin clear-log", "Clear a plugin log", "<ID>", 1, 1},
        {"plugin config",
         "Manage plugin configuration",
         "<COMMAND>",
         0,
         0,
         {{"--id", "", "ID", "", "Plugin ID (required for configuration commands)"}}},
        {"plugin config get", "Read a plugin configuration value", "<KEY>", 1, 1, {}, true},
        {"plugin config set",
         "Write a plugin configuration value",
         "<KEY> <VALUE>",
         2,
         2,
         {},
         true},
        {"plugin config delete", "Delete a plugin configuration value", "<KEY>", 1, 1, {}, true},
        {"plugin config list", "List plugin configuration as JSON", "", 0, 0, {}, true},
        {"kagami", "Manage built-in Kasumi and module mounts", "<COMMAND>"},
        {"kagami version", "Show the built-in controller version", ""},
        {"kagami config", "Manage mount configuration", "<COMMAND>"},
        {"kagami config show", "Read configuration as JSON", ""},
        {"kagami config gen", "Write the default configuration", ""},
        {"kagami config merge-json", "Merge a JSON object into configuration", "<JSON>", 1, 1},
        {"kagami config apply", "Apply the saved configuration", ""},
        {"kagami config sync-partitions", "Find additional module partitions", ""},
        {"kagami api", "Query controller state as JSON", "<COMMAND>"},
        {"kagami api system", "Read system information", ""},
        {"kagami api storage", "Read storage information", ""},
        {"kagami api kasumi", "Read Kasumi information", ""},
        {"kagami api features", "Read Kasumi features", ""},
        {"kagami api hooks", "Read active kernel hooks", ""},
        {"kagami api mounts", "Read mount state", ""},
        {"kagami api backends", "Read available mount backends", ""},
        {"kagami api meta",
         "Read module mount metadata",
         "",
         0,
         0,
         {{"--control-only", "", "", "", "Omit the full mount inventory"}},
         true},
        {"kagami daemon", "Control the mount daemon", "<COMMAND>"},
        {"kagami daemon status", "Read daemon status", ""},
        {"kagami daemon start", "Start the daemon", ""},
        {"kagami daemon serve", "Run the daemon in the foreground", ""},
        {"kagami daemon ping", "Ping the daemon", ""},
        {"kagami daemon stop", "Stop the daemon", ""},
        {"kagami daemon call", "Send a validated Kagami command to the daemon",
         "<COMMAND> [ARGS...]", 1, kTrailingArgs},
        {"kagami module", "Manage module mounting", "<COMMAND>"},
        {"kagami module list",
         "List module mount state",
         "",
         0,
         0,
         {{"--all", "", "", "", "Include disabled modules"}},
         true},
        {"kagami module set-mode", "Set a module's mount backend", "<ID> <MODE>", 2, 2},
        {"kagami module add-rule", "Add a module path rule", "<ID> <ABSOLUTE-PATH> <MODE>", 3, 3},
        {"kagami module remove-rule", "Remove a module path rule", "<ID> <ABSOLUTE-PATH>", 2, 2},
        {"kagami module add", "Hot-mount a Kasumi module", "<ID>", 1, 1},
        {"kagami module delete", "Hot-unmount a Kasumi module", "<ID>", 1, 1},
        {"kagami module hot-mount", "Hot-mount a Kasumi module", "<ID>", 1, 1},
        {"kagami module hot-unmount", "Hot-unmount a Kasumi module", "<ID>", 1, 1},
        {"kagami module check-conflicts", "Find conflicting module paths", ""},
        {"kagami module mount-all", "Mount enabled modules during boot", ""},
        {"kagami module unmount", "Unmount the controller's mounts", ""},
        {"kagami module normalize", "Normalize a module's metadata", "<PATH>", 1, 1},
        {"kagami kasumi", "Control Kasumi", "<COMMAND>"},
        {"kagami kasumi version", "Read the Kasumi version", ""},
        {"kagami kasumi list", "Read Kasumi rules", ""},
        {"kagami kasumi features", "Read supported Kasumi features", ""},
        {"kagami kasumi enable", "Apply configuration and enable Kasumi", ""},
        {"kagami kasumi disable", "Disable Kasumi", ""},
        {"kagami kasumi clear", "Clear Kasumi rules", ""},
        {"kagami kasumi hide-path", "Hide an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"kagami kasumi delete-rule", "Delete a path rule", "<ABSOLUTE-PATH>", 1, 1},
        {"kagami kasumi fix-mounts", "Reorder mount IDs", ""},
        {"kagami kasumi hide-overlay-xattrs", "Hide OverlayFS attributes on a path",
         "<ABSOLUTE-PATH>", 1, 1},
        {"kagami kasumi mount-hide", "Set mount hiding: off, on, normal, aggressive", "<MODE>", 1,
         1},
        {"kagami kasumi maps-spoof", "Set maps spoofing: off or on", "<STATE>", 1, 1},
        {"kagami kasumi statfs-spoof", "Set statfs spoofing: off or on", "<STATE>", 1, 1},
        {"kagami kasumi maps", "Manage maps spoofing rules", "<COMMAND>"},
        {"kagami kasumi maps clear", "Clear maps spoofing rules", ""},
        {"kagami kasumi maps add", "Add a maps spoofing rule",
         "<TARGET-INO> <TARGET-DEV> <SPOOF-INO> <SPOOF-DEV> <PATH>", 5, 5},
        {"kagami debug", "Control kernel diagnostics", "<COMMAND>"},
        {"kagami debug enable", "Enable kernel diagnostics", ""},
        {"kagami debug disable", "Disable kernel diagnostics", ""},
        {"kagami debug stealth", "Set stealth mode: enable or disable", "<STATE>", 1, 1},
        {"kagami hide", "Manage persistent path hiding", "<COMMAND>"},
        {"kagami hide list", "Read persistent hidden paths", ""},
        {"kagami hide apply", "Restore hidden paths after boot completion", ""},
        {"kagami hide add", "Add an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"kagami hide remove", "Remove an absolute path", "<ABSOLUTE-PATH>", 1, 1},
        {"kagami recovery", "Manage mount recovery state", "<COMMAND>"},
        {"kagami recovery status", "Read recovery state", ""},
        {"kagami recovery reset", "Reset recovery state", ""},
        {"uts-view", "Manage UTS identity views", "<COMMAND>"},
        {"uts-view status", "Read UTS view status", ""},
        {"uts-view release-snapshot", "Read the release snapshot", ""},
        {"uts-view get", "Read configured UTS templates", ""},
        {"uts-view original", "Read the original UTS identity", ""},
        {"uts-view effective", "Read the effective UTS identity", ""},
        {"uts-view enable-global", "Enable the global UTS view", ""},
        {"uts-view disable-global", "Disable the global UTS view", ""},
        {"uts-view enable-scoped", "Enable the scoped UTS view", ""},
        {"uts-view disable-scoped", "Disable the scoped UTS view", ""},
        {"uts-view set-global", "Set the global UTS template", "", 0, 0, uts_options(), true},
        {"uts-view set-deny", "Set the scoped UTS template", "", 0, 0, uts_options(), true},
        {"dynamic", "Manage dynamic manager signatures", "<COMMAND>"},
        {"dynamic get-sign",
         "Read the signature of an APK or UID",
         "[APK]",
         0,
         1,
         {kJson, {"--uid", "", "UID", ""}}},
        {"dynamic set-hash", "Add a manager signature", "<SIZE> <HASH>", 2, 2},
        {"dynamic set-apk", "Add a manager APK", "<APK>", 1, 1},
        {"dynamic set-uid", "Add a manager by UID", "<UID>", 1, 1},
        {"dynamic list", "List dynamic manager signatures", ""},
        {"dynamic del", "Delete a manager signature", "<SIZE> <HASH>", 2, 2},
        {"dynamic clear", "Clear dynamic manager signatures", ""},
    };
    return specs;
}

const std::array<Option, 2> kGlobalOptions = {{
    {"--color", "", "auto|always|never", "", "Choose terminal colors (default: auto)"},
    {"--verbose", "", "", "", "Include detailed diagnostic logs"},
}};

std::vector<const Option*> options_for(const Command& command) {
    std::vector<const Option*> result;
    for (const auto& parent : commands()) {
        if (parent.path == command.path ||
            (!parent.path.empty() && command.path.size() > parent.path.size() &&
             command.path.substr(0, parent.path.size()) == parent.path &&
             command.path[parent.path.size()] == ' ')) {
            for (const auto& option : parent.options)
                result.push_back(&option);
        }
    }
    return result;
}

std::string usage(const Command& command) {
    std::string text = "ksud";
    if (!command.path.empty())
        text += " " + std::string(command.path);
    if (!command.usage.empty())
        text += " " + std::string(command.usage);
    if (!options_for(command).empty())
        text += " [OPTIONS]";
    return text;
}

std::vector<const Command*> children(std::string_view path) {
    const std::string prefix = path.empty() ? "" : std::string(path) + " ";
    std::vector<const Command*> result;
    for (const auto& candidate : commands()) {
        if (candidate.path.size() <= prefix.size() ||
            candidate.path.substr(0, prefix.size()) != prefix)
            continue;
        if (candidate.path.find(' ', prefix.size()) == std::string_view::npos)
            result.push_back(&candidate);
    }
    return result;
}

std::string_view option_description(const Option& option, std::string_view path) {
    if (!option.description.empty())
        return option.description;
    if (option.name == "--temp")
        return "Use temporary configuration, cleared on reboot";
    if (option.name == "--slot")
        return "Target an A/B slot (default: current slot)";
    if (option.name == "--flags")
        return "Unmount flags as an unsigned 32-bit integer (default: 0)";
    if (option.name == "--json")
        return "Write a machine-readable JSON result to stdout";
    if (option.name == "--ota")
        return "Target the inactive A/B slot";
    if (option.name == "--magiskboot")
        return path == "boot-patch" || path == "boot-restore" || path == "boot-patch-v2" ||
                       path == "uninstall"
                   ? "Accepted for compatibility; the embedded tool is used"
                   : "Use this magiskboot path";
    if (option.name == "--libadbroot")
        return "Install this adb root library";
    if (option.name == "--magica")
        return "Use Magica with an optional TCP port";
    if (option.name == "--post-magica")
        return "Run the post-Magica late-load stage";
    if (option.name == "--allow-shell")
        return "Keep a root shell available";
    if (option.name == "--enable-adbd")
        return "Run adbd as root";
    if (option.name == "--boot")
        return "Read this boot image instead of detecting a partition";
    if (option.name == "--module")
        return "Use this kernel module";
    if (option.name == "--output" || option.name == "--out")
        return "Write the patched boot image to this path";
    if (option.name == "--superkey")
        return "Set the SuperKey";
    if (option.name == "--uts-config")
        return "Embed a UTS template from this file";
    if (option.name == "--force")
        return "Allow replacement of an existing direct LKM capsule";
    if (option.name == "--flash")
        return "Write the result to the selected partition";
    if (option.name == "--signature-bypass")
        return "Relax LKM signature checking";
    if (option.name == "--no-reuse")
        return "Build a fresh capsule instead of reusing an existing one";
    if (option.name == "--log")
        return "Write the flashing log to this file";
    if (option.name == "--use-mkbootfs")
        return "Use the embedded mkbootfs in AnyKernel3";
    if (option.name == "--all")
        return "Include all partitions";
    if (option.name == "--verbose")
        return "Include diagnostic logs";
    if (option.name == "--ready-fd")
        return "Write daemon readiness to an inherited descriptor";
    if (option.name == "--uid")
        return "Resolve an APK from this Android UID";
    if (option.name == "-g")
        return "Use the global mount namespace";
    return "";
}

std::string_view example(std::string_view path) {
    if (path.empty())
        return "ksud module --help\n  ksud feature list\n  ksud --verbose boot-info current-kmi";
    if (path == "module" || path == "module install")
        return "ksud module install /sdcard/module.zip";
    if (path == "feature" || path == "feature set" || path == "feature set-save")
        return "ksud feature get sulog\n  ksud feature set-save sulog 1";
    if (path.substr(0, 13) == "plugin config")
        return "ksud plugin config --id example set theme dark";
    if (path == "boot-patch")
        return "ksud boot-patch --boot /sdcard/boot.img --out /sdcard/patched";
    if (path == "boot-patch-v2")
        return "ksud boot-patch-v2 --boot /sdcard/boot.img --output /sdcard/patched.img";
    if (path == "boot-restore")
        return "ksud boot-restore --boot /sdcard/patched.img";
    if (path == "flash" || path == "flash image")
        return "ksud flash image /sdcard/boot.img boot --slot a";
    if (path == "uts-view" || path.substr(0, 12) == "uts-view set")
        return "ksud uts-view set-global --release '6.12.0' --inherit version\n  ksud uts-view "
               "status";
    if (path == "kagami" || path == "kagami module" || path == "kagami module set-mode")
        return "ksud kagami module list\n  ksud kagami module set-mode example kasumi";
    if (path == "kagami config" || path == "kagami config merge-json")
        return "ksud kagami config merge-json '{\"debug\":true}'";
    if (path == "kagami daemon call")
        return "ksud kagami daemon call api system";
    if (path == "su-path" || path == "su-path set")
        return "ksud su-path set /data/local/su";
    return "";
}

void help(const Command& command) {
    (void)printf("%.*s\n\n", static_cast<int>(command.description.size()),
                 command.description.data());
    terminal::heading(stdout, "Usage: ");
    (void)printf("%s\n", usage(command).c_str());
    const auto subcommands = children(command.path);
    if (!subcommands.empty()) {
        terminal::heading(stdout, "\nCommands:\n");
        for (const auto* child : subcommands) {
            const auto name =
                child->path.substr(command.path.empty() ? 0 : command.path.size() + 1);
            (void)printf("  %-22.*s %.*s\n", static_cast<int>(name.size()), name.data(),
                         static_cast<int>(child->description.size()), child->description.data());
        }
    }
    terminal::heading(stdout, "\nOptions:\n");
    (void)printf("  %-32s %s\n", "-h, --help", "Show this help");
    for (const auto* option : options_for(command)) {
        std::string label(option->name);
        if (!option->alias.empty())
            label += ", " + std::string(option->alias);
        if (!option->value.empty())
            label += (option->default_value.empty() ? " <" : " [") + std::string(option->value) +
                     (option->default_value.empty() ? ">" : "]");
        const auto description = option_description(*option, command.path);
        (void)printf("  %-32s %.*s", label.c_str(), static_cast<int>(description.size()),
                     description.data());
        if (!option->default_value.empty())
            (void)printf(" (default when value is omitted: %.*s)",
                         static_cast<int>(option->default_value.size()),
                         option->default_value.data());
        (void)fputc('\n', stdout);
    }
    terminal::heading(stdout, "\nGlobal options:\n");
    (void)printf("  %-32s %s\n", "--color <auto|always|never>", "Choose colors (default: auto)");
    (void)printf("  %-32s %s\n", "--verbose", "Include diagnostic logs");
    if (command.path.empty())
        (void)printf("  %-32s %s\n", "-V, --version", "Show version");
    const auto examples = example(command.path);
    if (!examples.empty()) {
        terminal::heading(stdout, "\nExamples:\n");
        (void)printf("  %.*s\n", static_cast<int>(examples.size()), examples.data());
    }
    if (command.path == "boot-patch" || command.path == "boot-patch-v2" ||
        command.path == "boot-restore")
        (void)printf(
            "\nWithout --boot, the partition is detected. Partition writes require --flash.\n");
    (void)printf("\nUse -- before positional values that begin with '-'.\n");
}

size_t distance(std::string_view left, std::string_view right) {
    std::vector<size_t> row(right.size() + 1);
    std::iota(row.begin(), row.end(), 0U);
    for (size_t i = 0; i < left.size(); ++i) {
        size_t previous = row[0];
        row[0] = i + 1;
        for (size_t j = 0; j < right.size(); ++j) {
            const size_t old = row[j + 1];
            row[j + 1] = std::min({row[j] + 1, old + 1, previous + (left[i] != right[j])});
            previous = old;
        }
    }
    return row.back();
}

std::string suggestion(std::string_view word, const std::vector<std::string_view>& candidates) {
    if (word.size() > 64)
        return "";
    size_t best = 3;
    std::string result;
    for (const auto candidate : candidates) {
        const size_t score = distance(word, candidate);
        if (score < best) {
            best = score;
            result = candidate;
        } else if (score == best && result != candidate)
            result.clear();
    }
    return result.empty() ? "" : "; did you mean '" + result + "'?";
}

int fail(const Command& command, const std::string& message) {
    const int result = terminal::usage_error(message, usage(command));
    const std::string path = command.path.empty() ? "ksud" : "ksud " + std::string(command.path);
    (void)fprintf(stderr, "For more information, run '%s --help'.\n", path.c_str());
    return result;
}

const Option* find_option(const Command& command, std::string_view name) {
    for (const auto* option : options_for(command))
        if (option->name == name || (!option->alias.empty() && option->alias == name))
            return option;
    for (const auto& option : kGlobalOptions)
        if (option.name == name)
            return &option;
    return nullptr;
}

bool unsigned_value(std::string_view value, uint64_t maximum, bool auto_base = false) {
    if (value.empty() || value[0] == '-' || value[0] == '+')
        return false;
    int base = 10;
    if (auto_base && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    } else if (auto_base && value.size() > 1 && value[0] == '0')
        base = 8;
    uint64_t number = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), number, base);
    return error == std::errc{} && end == value.data() + value.size() && number <= maximum;
}

bool one_of(std::string_view value, std::string_view choices) {
    while (!choices.empty()) {
        const size_t separator = choices.find('|');
        if (value == choices.substr(0, separator))
            return true;
        if (separator == std::string_view::npos)
            break;
        choices.remove_prefix(separator + 1);
    }
    return false;
}

int read_option(const Command& command, const std::vector<std::string>& input, size_t& index,
                CliArguments& parsed) {
    const auto& argument = input[index];
    const size_t equals = argument.find('=');
    std::string name = argument.substr(0, equals);
    const Option* option = find_option(command, name);
    std::string attached;
    bool has_attached = equals != std::string::npos;
    if (has_attached)
        attached = argument.substr(equals + 1);
    if (!option && argument.size() > 2 && argument[0] == '-' && argument[1] != '-') {
        option = find_option(command, argument.substr(0, 2));
        if (option && !option->value.empty()) {
            name = argument.substr(0, 2);
            attached = argument.substr(2);
            has_attached = true;
        } else
            option = nullptr;
    }
    if (!option) {
        std::vector<std::string_view> names;
        for (const auto* candidate : options_for(command))
            names.push_back(candidate->name);
        for (const auto& candidate : kGlobalOptions)
            names.push_back(candidate.name);
        return fail(command, "unknown option '" + name + "'" + suggestion(name, names));
    }
    const bool global = option->name == "--color" || option->name == "--verbose";
    if (!global && !option->repeatable && parsed.has(std::string(option->name)))
        return fail(command, "option '" + name + "' was specified more than once");
    std::string value;
    if (option->value.empty()) {
        if (has_attached)
            return fail(command, "option '" + name + "' does not take a value");
    } else if (has_attached)
        value = attached;
    else if (index + 1 < input.size() && (input[index + 1].empty() || input[index + 1][0] != '-'))
        value = input[++index];
    else if (!option->default_value.empty())
        value = option->default_value;
    else
        return fail(command, "option '" + name + "' requires <" + std::string(option->value) + ">");
    if (!option->value.empty() && value.empty() && !option->allow_empty)
        return fail(command, "option '" + name + "' requires a non-empty value");
    if (option->value.find('|') != std::string_view::npos && !one_of(value, option->value))
        return fail(command, "invalid value '" + value + "' for " + name + "; expected " +
                                 std::string(option->value));
    if (option->value == "UINT32" && !unsigned_value(value, UINT32_MAX))
        return fail(command, "invalid value '" + value + "' for " + name +
                                 "; expected an unsigned 32-bit integer");
    if (option->value == "FD" && !unsigned_value(value, INT32_MAX))
        return fail(command,
                    "invalid file descriptor '" + value + "'; expected 0 through 2147483647");
    if (option->value == "PORT" &&
        (!unsigned_value(value, 65535) || value.find_first_not_of('0') == std::string::npos))
        return fail(command, "invalid port '" + value + "'; expected 1 through 65535");
    if (option->name == "--color") {
        (void)terminal::set_color(value);
    } else if (option->name == "--verbose") {
        parsed.verbose = true;
    } else {
        parsed.options[std::string(option->name)] = value;
        parsed.option_args.emplace_back(option->name);
        if (!option->value.empty())
            parsed.option_args.push_back(value);
    }
    ++index;
    return -1;
}

bool identifier(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
           });
}

int validate(const Command& command, const std::vector<std::string>& operands,
             const CliArguments& parsed) {
    const auto path = command.path;
    const auto bad = [&](const std::string& message) { return fail(command, message); };
    const auto numeric = [&](size_t index, uint64_t maximum, bool auto_base = false) {
        return unsigned_value(operands[index], maximum, auto_base);
    };
    if (path == "module config get" || path == "module config set" ||
        path == "module config delete") {
        if (operands[0].find_first_of("=\r\n") != std::string::npos)
            return bad("module configuration keys cannot contain '=' or line breaks");
        if (path == "module config set" && operands[1].find_first_of("\r\n") != std::string::npos)
            return bad("module configuration values must fit on one line");
    }
    if ((path == "feature set" || path == "feature set-save") && !numeric(1, UINT64_MAX))
        return bad("invalid value '" + operands[1] + "'; expected an unsigned 64-bit integer");
    if (path.substr(0, 11) == "debug mark " && !operands.empty() && !numeric(0, INT32_MAX))
        return bad("invalid PID '" + operands[0] + "'; expected 0 through 2147483647");
    if (path == "flash map" && !one_of(operands[0], "a|b|_a|_b"))
        return bad("invalid slot '" + operands[0] + "'; expected a, b, _a or _b");
    if (path == "flash avb" && !operands.empty() && operands[0] != "disable")
        return bad("unknown AVB operation '" + operands[0] + "'; expected disable");
    if (path == "boot-info target-kmi" && parsed.has("--ota") && parsed.has("--boot"))
        return bad("--ota and --boot cannot be used together");
    if (path == "boot-patch-v2" && parsed.has("--out") && parsed.has("--output"))
        return bad("--out and --output are aliases; specify only one");
    if (path == "dynamic get-sign") {
        if ((!operands.empty()) == parsed.has("--uid"))
            return bad("specify exactly one APK path or --uid <UID>");
        if (parsed.has("--uid") && !unsigned_value(parsed.value("--uid"), UINT32_MAX, true))
            return bad("invalid UID; expected an unsigned 32-bit integer");
    }
    if (path == "dynamic set-uid" && !numeric(0, UINT32_MAX, true))
        return bad("invalid UID; expected an unsigned 32-bit integer");
    if (path == "dynamic set-hash" || path == "dynamic del") {
        if (!numeric(0, UINT32_MAX, true))
            return bad("invalid signature size; expected an unsigned 32-bit integer");
        if (operands[1].size() != 64 ||
            !std::all_of(operands[1].begin(), operands[1].end(),
                         [](unsigned char ch) { return std::isxdigit(ch); }))
            return bad("invalid signature hash; expected exactly 64 hexadecimal digits");
    }
    if (path.substr(0, 14) == "plugin config ") {
        const std::string id = parsed.value("--id");
        if (!parsed.has("--id"))
            return bad("--id <ID> is required");
        if (id.size() > 64 || !identifier(id))
            return bad("invalid plugin ID '" + id + "'");
        if (!operands.empty() && (operands[0].size() > 64 || !identifier(operands[0])))
            return bad("invalid configuration key '" + operands[0] + "'");
    }
    if (path == "plugin daemon") {
        if (!parsed.has("--ready-fd"))
            return bad("--ready-fd <FD> is required");
        if (!numeric(2, INT32_MAX) || operands[2].find_first_not_of('0') == std::string::npos)
            return bad("invalid interval; expected 1 through 2147483647");
        if (unsigned_value(parsed.value("--ready-fd"), 2))
            return bad("readiness descriptor must be greater than 2");
    }
    if (path == "uts-view set-global" || path == "uts-view set-deny") {
        std::set<std::string> fields;
        for (size_t i = 0; i < parsed.option_args.size(); i += 2) {
            const auto& name = parsed.option_args[i];
            const auto& value = parsed.option_args[i + 1];
            const std::string field = name == "--inherit" ? value : name.substr(2);
            if (!one_of(field, "sysname|nodename|release|version|machine|domainname"))
                return bad("unknown UTS field '" + field + "'");
            if (!fields.insert(field).second)
                return bad("UTS field '" + field + "' was specified more than once");
            if (name != "--inherit" && value.size() > 64)
                return bad("UTS field '" + field + "' exceeds 64 bytes");
        }
    }
    if (path.substr(0, 7) == "kagami ") {
        if (path == "kagami config merge-json") {
            json::Value object;
            std::string error;
            if (!json::parse_checked(operands[0], object, error) ||
                object.type != json::Type::Object)
                return bad("invalid configuration JSON: " +
                           (error.empty() ? std::string("expected an object") : error));
            if (!kagami::validate_config_patch(object, error))
                return bad(error);
        }
        if (path == "kagami module set-mode" &&
            !one_of(operands[1], "auto|kasumi|overlay|magic|none"))
            return bad("invalid mount mode '" + operands[1] +
                       "'; expected auto, kasumi, overlay, magic or none");
        if (path == "kagami module add-rule" &&
            !one_of(operands[2], "auto|kasumi|overlay|magic|none|hide"))
            return bad("invalid rule mode '" + operands[2] +
                       "'; expected auto, kasumi, overlay, magic, none or hide");
        if (path == "kagami kasumi mount-hide" && !one_of(operands[0], "off|on|normal|aggressive"))
            return bad("invalid mount hiding mode; expected off, on, normal or aggressive");
        if ((path == "kagami kasumi maps-spoof" || path == "kagami kasumi statfs-spoof") &&
            !one_of(operands[0], "off|on"))
            return bad("invalid state '" + operands[0] + "'; expected off or on");
        if (path == "kagami debug stealth" && !one_of(operands[0], "enable|disable"))
            return bad("invalid stealth state; expected enable or disable");
        if (path == "kagami kasumi maps add") {
            for (size_t i = 0; i < 4; ++i)
                if (!numeric(i, UINT64_MAX, true))
                    return bad("invalid maps value '" + operands[i] +
                               "'; expected an unsigned integer");
        }
        if (path.substr(0, 14) == "kagami module " && !operands.empty() &&
            path != "kagami module normalize" && !identifier(operands[0]))
            return bad("invalid module ID '" + operands[0] + "'");
    }
    if (command.usage.find("<ABSOLUTE-PATH>") != std::string_view::npos && path != "su-path set") {
        const size_t index =
            path == "kagami module add-rule" || path == "kagami module remove-rule" ? 1 : 0;
        if (operands[index].empty() || operands[index][0] != '/')
            return bad("expected an absolute path, got '" + operands[index] + "'");
    }
    return -1;
}

int parse_input(const std::vector<std::string>& input, CliArguments& parsed, unsigned depth) {
    if (depth > 8)
        return terminal::usage_error("too many nested daemon calls",
                                     "ksud kagami daemon call <COMMAND>");
    size_t index = 0;
    const Command* current = &commands().front();
    bool help_requested = false;
    while (true) {
        if (index == input.size())
            break;
        const auto& argument = input[index];
        if (argument == "-h" || argument == "--help") {
            help(*current);
            return 0;
        }
        if (argument == "help") {
            help_requested = true;
            ++index;
            continue;
        }
        if (argument.size() > 1 && argument[0] == '-' &&
            !(current->path.empty() && one_of(argument, "-v|-V|--version")) &&
            !(current->path == "kagami" && argument == "--version")) {
            const int result = read_option(*current, input, index, parsed);
            if (result >= 0)
                return result;
            continue;
        }
        std::string name = argument;
        if (current->path.empty()) {
            if (one_of(name, "-v|-V|--version"))
                name = "version";
            else if (name == "yukizygisk")
                name = "yzctl";
        } else if (current->path == "kagami" && name == "--version")
            name = "version";
        const auto candidates = children(current->path);
        const std::string path =
            current->path.empty() ? name : std::string(current->path) + " " + name;
        const auto found =
            std::find_if(candidates.begin(), candidates.end(),
                         [&path](const Command* item) { return item->path == path; });
        if (found == candidates.end()) {
            std::vector<std::string_view> names;
            names.reserve(candidates.size());
            for (const auto* child : candidates)
                names.push_back(
                    child->path.substr(current->path.empty() ? 0 : current->path.size() + 1));
            return fail(*current, "unknown command '" + name + "'" + suggestion(name, names));
        }
        current = *found;
        ++index;
        if (children(current->path).empty())
            break;
    }
    if (help_requested || current->path.empty()) {
        help(*current);
        return 0;
    }
    if (!children(current->path).empty())
        return fail(*current, "a subcommand is required");
    parsed.path = current->path;
    const size_t separator = parsed.path.find(' ');
    parsed.command = parsed.path.substr(0, separator);
    parsed.forward_options = current->forward_options;
    size_t begin = separator;
    while (begin != std::string::npos) {
        const size_t end = parsed.path.find(' ', begin + 1);
        parsed.args.push_back(parsed.path.substr(begin + 1, end - begin - 1));
        begin = end;
    }
    std::vector<std::string> operands;
    bool options_enabled = true;
    while (index < input.size()) {
        const auto& argument = input[index];
        if (current->maximum == kTrailingArgs && !operands.empty()) {
            operands.push_back(argument);
            ++index;
            continue;
        }
        if (options_enabled && argument == "--") {
            options_enabled = false;
            ++index;
            continue;
        }
        if (options_enabled && (argument == "-h" || argument == "--help")) {
            help(*current);
            return 0;
        }
        if (options_enabled && argument.size() > 1 && argument[0] == '-') {
            const int result = read_option(*current, input, index, parsed);
            if (result >= 0)
                return result;
        } else {
            operands.push_back(argument);
            ++index;
        }
    }
    if (operands.size() < current->minimum)
        return fail(*current, "missing required argument: " + std::string(current->usage));
    if (operands.size() > current->maximum)
        return fail(*current, "unexpected argument '" + operands[current->maximum] + "'");
    const int validation = validate(*current, operands, parsed);
    if (validation >= 0)
        return validation;
    if (current->path == "kagami daemon call") {
        std::vector<std::string> nested_input = {"kagami"};
        nested_input.insert(nested_input.end(), operands.begin(), operands.end());
        CliArguments nested;
        nested.verbose = parsed.verbose;
        const int result = parse_input(nested_input, nested, depth + 1);
        if (result >= 0)
            return result;
        parsed.verbose = nested.verbose;
        operands = nested.args;
        operands.insert(operands.end(), nested.option_args.begin(), nested.option_args.end());
    }
    parsed.args.insert(parsed.args.end(), operands.begin(), operands.end());
    return -1;
}

}  // namespace

bool CliArguments::has(const std::string& name) const {
    return options.find(name) != options.end();
}
std::string CliArguments::value(const std::string& name) const {
    const auto found = options.find(name);
    return found == options.end() ? "" : found->second;
}

int parse_cli(const std::vector<std::string>& input, CliArguments& parsed) {
    parsed = {};
    (void)terminal::set_color("auto");
    return parse_input(input, parsed, 0);
}

}  // namespace ksud
