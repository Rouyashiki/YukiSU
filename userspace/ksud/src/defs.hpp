#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// Kernel uapi headers provide feature IDs, event constants,
// mark/umount operation constants, and ioctl numbers.
extern "C" {
#include "uapi/feature.h"
}
#include "uapi/supercall.h"  // EVENT_*, KSU_MARK_*, KSU_UMOUNT_* are macros

namespace ksud {

constexpr auto BOOT_STAGE_TIMEOUT = std::chrono::seconds(35);
constexpr auto EMULATED_SOFT_REBOOT_TIMEOUT = std::chrono::seconds(5);

// Version info
constexpr const char* KSUD_VERSION = "1.0.0";
constexpr int KSUD_VERSION_CODE = 10000;
extern const char* const VERSION_CODE;
extern const char* const VERSION_NAME;

// Paths
constexpr const char* ADB_DIR = "/data/adb/";
constexpr const char* WORKING_DIR = "/data/adb/ksu/";
constexpr const char* BINARY_DIR = "/data/adb/ksu/bin/";
constexpr const char* LIBRARY_DIR = "/data/adb/ksu/lib/";
constexpr const char* LOG_DIR = "/data/adb/ksu/log/";

// Binary tool paths
constexpr const char* BUSYBOX_PATH = "/data/adb/ksu/bin/busybox";
constexpr const char* RESETPROP_PATH = "/data/adb/ksu/bin/resetprop";
constexpr const char* BOOTCTL_PATH = "/data/adb/ksu/bin/bootctl";

constexpr const char* PROFILE_DIR = "/data/adb/ksu/profile/";
constexpr const char* PROFILE_SELINUX_DIR = "/data/adb/ksu/profile/selinux/";
constexpr const char* PROFILE_TEMPLATE_DIR = "/data/adb/ksu/profile/templates/";

constexpr const char* FEATURE_CONFIG_PATH = "/data/adb/ksu/.ksurc";
constexpr const char* SHELL_RC_PATH = "/data/adb/ksu/bin/shellrc.sh";
constexpr const char* DAEMON_PATH = "/data/adb/ksud";
constexpr const char* MAGISKBOOT_PATH = "/data/adb/ksu/bin/magiskboot";
constexpr const char* LIBADBROOT_PATH = "/data/adb/ksu/lib/libadbroot.so";

// YukiZygisk runtime payload: ksud stages these at post-fs-data; the kernel
// reads the first-stage/core libraries as ksu_cred and hands them to target
// processes via memfd, so target processes never open these paths directly.
// Private to ksu's lib dir to avoid colliding with other zygisk implementations
// under /data/adb/zygisk.
constexpr const char* YUKIZYGISK_DIR = "/data/adb/ksu/lib/yukizygisk/";
constexpr const char* ZCORE64_PATH = "/data/adb/ksu/lib/yukizygisk/libzygisk64.so";
constexpr const char* ZCORE32_PATH = "/data/adb/ksu/lib/yukizygisk/libzygisk32.so";
constexpr const char* ZNCORE64_PATH = "/data/adb/ksu/lib/yukizygisk/libyukizncore64.so";
constexpr const char* ZNCORE32_PATH = "/data/adb/ksu/lib/yukizygisk/libyukizncore32.so";
// Split-out anonymous module loader; core dlopen's it (fd brokered by zygiskd).
constexpr const char* ZYUKILINKER64_PATH = "/data/adb/ksu/lib/yukizygisk/libyukilinker64.so";
constexpr const char* ZYUKILINKER32_PATH = "/data/adb/ksu/lib/yukizygisk/libyukilinker32.so";
constexpr const char* ZYGISKD64_PATH = "/data/adb/ksu/bin/zygiskd64";
constexpr const char* ZYGISKD32_PATH = "/data/adb/ksu/bin/zygiskd32";
// Runtime state remains separate from deployed payloads so updates cannot
// replace user configuration or diagnostic logs.
constexpr const char* YUKIZYGISK_STATE_DIR = "/data/adb/ksu/yukizygisk";
constexpr const char* YUKIZYGISK_CONFIG_PATH = "/data/adb/ksu/yukizygisk/yzconfig.json";
constexpr const char* YUKIZYGISK_DIAGNOSTICS_DIR = "/data/adb/ksu/yukizygisk/diagnostics";
constexpr const char* YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR =
    "/data/adb/ksu/yukizygisk/diagnostics/current";
constexpr const char* YUKIZYGISK_OLD_DIAGNOSTICS_DIR = "/data/adb/ksu/yukizygisk/diagnostics/old";
constexpr const char* YUKIZYGISK_LOG_DIR = "/data/adb/ksu/yukizygisk/diagnostics/current/logs";
constexpr const char* YUKIZYGISK_LOG64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd64.log";
constexpr const char* YUKIZYGISK_ROLLED_LOG64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd64.1.log";
constexpr const char* YUKIZYGISK_LOG32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd32.log";
constexpr const char* YUKIZYGISK_ROLLED_LOG32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd32.1.log";
constexpr const char* YUKIZYGISK_LINKER64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/linker64.json";
constexpr const char* YUKIZYGISK_LINKER32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/linker32.json";
constexpr const char* YUKIZYGISK_DIAGNOSTIC_EVIDENCE_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/evidence";
constexpr const char* YUKIZYGISK_LEGACY_LOG_DIR = "/data/adb/ksu/yukizygisk/log";
constexpr const char* DAEMON_LINK_PATH = "/data/adb/ksu/bin/ksud";
constexpr const char* SULOGD_LOCK_PATH = "/data/adb/ksu/sulogd.lock";

constexpr const char* MODULE_DIR = "/data/adb/modules/";
constexpr const char* MODULE_UPDATE_DIR = "/data/adb/modules_update/";
constexpr const char* METAMODULE_DIR = "/data/adb/metamodule/";
constexpr const char* PREINIT_DIR_WATCHDOG = "/metadata/watchdog/ksu/";
constexpr const char* PREINIT_DIR_DEFAULT = "/metadata/ksu/";
constexpr const char* MODULES_RC_FILE = "modules.rc";
constexpr const char* MODULES_RC_TMP_FILE = ".modules.rc.tmp";

constexpr const char* MODULE_WEB_DIR = "webroot";
constexpr const char* MODULE_ACTION_SH = "action.sh";
constexpr const char* DISABLE_FILE_NAME = "disable";
constexpr const char* UPDATE_FILE_NAME = "update";
constexpr const char* REMOVE_FILE_NAME = "remove";
constexpr const char* MODULE_INIT_RC_DIR = "initrc";

// Module config system
constexpr const char* MODULE_CONFIG_DIR = "/data/adb/ksu/module_configs/";
constexpr const char* PERSIST_CONFIG_NAME = "persist.config";
constexpr const char* TEMP_CONFIG_NAME = "tmp.config";

// Metamodule support
constexpr const char* METAMODULE_MOUNT_SCRIPT = "metamount.sh";
constexpr const char* METAMODULE_METAINSTALL_SCRIPT = "metainstall.sh";
constexpr const char* METAMODULE_METAUNINSTALL_SCRIPT = "metauninstall.sh";

// Plugin system
constexpr const char* PLUGIN_DIR = "/data/adb/plugins/";
constexpr const char* PLUGIN_STAGE_DIR = "/data/adb/ksu/plugin_stage/";
constexpr const char* PLUGIN_LOCK_DIR = "/data/adb/ksu/plugin_locks/";
constexpr const char* PLUGIN_MANIFEST = "plugin.json";
constexpr const char* PLUGIN_ENTRY = "main.lua";
constexpr const char* PLUGIN_OUTPUT_LOG = "last_output.log";
constexpr const char* PLUGIN_CONFIG_FILE = "config.json";

// Backup
constexpr const char* KSU_BACKUP_DIR = "/data/adb/ksu/";
constexpr const char* KSU_BACKUP_FILE_PREFIX = "ksu_backup_";
constexpr const char* BACKUP_FILENAME = "stock_image.sha1";
constexpr const char* UMOUNT_CONFIG_PATH = "/data/adb/ksu/.umount";

// No need to redefine FeatureId, EVENT_*, KSU_MARK_*, UMOUNT_* —
// they are all provided by uapi/feature.h and uapi/supercall.h.
// C++ callers can use the C enum ksu_feature_id values directly or
// via the convenience wrappers in core/ksucalls.hpp.

}  // namespace ksud
