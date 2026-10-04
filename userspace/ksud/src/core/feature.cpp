#include "feature.hpp"
#include "../../kagami/include/kagami/kasumi_client.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../magisk_compat/msud.hpp"
#include "../magisk_compat/su_transition.hpp"
#include "../module/module.hpp"
#include "../sulog.hpp"
#include "../terminal.hpp"
#include "../utils.hpp"
#include "../yukizygisk_snapshot.hpp"
#include "ksucalls.hpp"
#include "su_path.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ksud {

namespace {

const std::string& get_feature_config_path() {
    static const std::string path = std::string(WORKING_DIR) + ".feature_config";
    return path;
}
constexpr uint32_t FEATURE_MAGIC = 0x7f4b5355;
constexpr uint32_t FEATURE_VERSION = 1;
constexpr const char* LEGACY_HIDE_BOOTLOADER_CONFIG = "/data/adb/ksu/.hide_bootloader";

bool sync_feature_directory() {
    const int fd = open(WORKING_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

const std::map<std::string, uint32_t>& get_feature_map() {
    static const std::map<std::string, uint32_t> map = {
        {"su_compat", KSU_FEATURE_SU_COMPAT},
        {"kernel_umount", KSU_FEATURE_KERNEL_UMOUNT},
        {"enhanced_security", KSU_FEATURE_ENHANCED_SECURITY},
        {"adb_root", KSU_FEATURE_ADB_ROOT},
        {"selinux_hide", KSU_FEATURE_SELINUX_HIDE},
        {"webview_zygote_umount", KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT},
        {"default_no_new_privs", KSU_FEATURE_DEFAULT_NO_NEW_PRIVS},
        {"sulog", KSU_FEATURE_SULOG},
        {"magisk_compat", KSU_FEATURE_MAGISK_COMPAT},
        {"yukizygisk", KSU_FEATURE_YUKIZYGISK},
        {"hide_bootloader", KSU_FEATURE_HIDE_BOOTLOADER},
        {"kasumi_sucompat", KSU_FEATURE_KASUMI_SUCOMPAT},
        {"kasumi", KSU_FEATURE_KASUMI},
        {"unshare_mnt", KSU_FEATURE_UNSHARE_MNT},
    };
    return map;
}

const std::map<uint32_t, const char*>& get_feature_descriptions() {
    static const std::map<uint32_t, const char*> desc = {
        {KSU_FEATURE_SU_COMPAT,
         "Classic SU Compatibility Mode - allows authorized apps to gain root via the legacy "
         "stat-intercepted 'su' path; mutually exclusive with kasumi_sucompat"},
        {KSU_FEATURE_KERNEL_UMOUNT,
         "Kernel Umount - controls whether kernel automatically unmounts modules when not needed"},
        {KSU_FEATURE_ENHANCED_SECURITY,
         "Enhanced Security - disable non-KSU root elevation and unauthorized UID downgrades"},
        {KSU_FEATURE_ADB_ROOT,
         "ADB Root - run adbd with root privileges via kernel feature injection"},
        {KSU_FEATURE_SELINUX_HIDE,
         "SELinux Hide - hides KernelSU sepolicy changes from app-facing SELinux probes"},
        {KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT,
         "WebView Zygote Umount - unmounts modules before WebView sandbox processes inherit "
         "the zygote mount namespace"},
        {KSU_FEATURE_DEFAULT_NO_NEW_PRIVS,
         "Default No-New-Privs - profiles using the default root profile block re-escalation "
         "(anti-escape) by default"},
        {KSU_FEATURE_SULOG,
         "SU Log - streams kernel sulog events to userspace and persists them to disk"},
        {KSU_FEATURE_MAGISK_COMPAT,
         "Magisk-compat su prompt - shows a visible su and asks for authorization on first use "
         "for apps that are not in the allowlist; automatically enables kasumi_sucompat"},
        {KSU_FEATURE_YUKIZYGISK,
         "YukiZygisk - kernel captures zygote and injects Zygisk modules; the daemon is brought "
         "up at post-fs-data when enabled (off by default)"},
        {KSU_FEATURE_HIDE_BOOTLOADER,
         "Hide Bootloader - rewrites bootloader and verified-boot properties after Android boot "
         "completes (off by default)"},
        {KSU_FEATURE_KASUMI_SUCOMPAT,
         "Kasumi su compatibility - exposes su through dirhijack and vnode instead of legacy "
         "stat syscall interception; mutually exclusive with classic su compatibility (off by "
         "default)"},
        {KSU_FEATURE_KASUMI,
         "Kasumi - initializes the embedded VFS engine on demand; required for all Kasumi "
         "functions. Disabling takes effect after reboot (off by default)"},
        {KSU_FEATURE_UNSHARE_MNT,
         "Mount View Cleanup - rebuilds the namespace after module unmounting and normalizes "
         "visible propagation IDs (off by default)"},
    };
    return desc;
}

void normalize_sucompat_config(std::map<uint32_t, uint64_t>& features) {
    auto magisk = features.find(KSU_FEATURE_MAGISK_COMPAT);
    if (magisk != features.end() && magisk->second != 0) {
        const bool magisk_supported = get_feature(KSU_FEATURE_MAGISK_COMPAT).second;
        const bool kasumi_supported = get_feature(KSU_FEATURE_KASUMI_SUCOMPAT).second;
        if (magisk_supported && kasumi_supported) {
            if (features[KSU_FEATURE_SU_COMPAT] != 0 ||
                features[KSU_FEATURE_KASUMI_SUCOMPAT] == 0) {
                LOGW("magisk_compat requires kasumi_sucompat; normalizing sucompat config");
            }
            features[KSU_FEATURE_SU_COMPAT] = 0;
            features[KSU_FEATURE_KASUMI_SUCOMPAT] = 1;
        } else {
            magisk->second = 0;
            features[KSU_FEATURE_SU_COMPAT] = 1;
            features[KSU_FEATURE_KASUMI_SUCOMPAT] = 0;
            LOGW("magisk_compat dependency is unsupported; restoring classic su_compat");
        }
    }

    auto classic = features.find(KSU_FEATURE_SU_COMPAT);
    auto kasumi = features.find(KSU_FEATURE_KASUMI_SUCOMPAT);
    if (classic == features.end() || kasumi == features.end() || classic->second == 0 ||
        kasumi->second == 0) {
        return;
    }

    const auto [_, kasumi_supported] = get_feature(KSU_FEATURE_KASUMI_SUCOMPAT);
    if (kasumi_supported) {
        classic->second = 0;
        LOGW("Both su_compat modes were enabled in config; preferring kasumi_sucompat");
    } else {
        kasumi->second = 0;
        LOGW("kasumi_sucompat is unsupported; keeping classic su_compat enabled");
    }
}

void settle_sucompat_values(std::map<uint32_t, uint64_t>& features) {
    for (const uint32_t id :
         {KSU_FEATURE_SU_COMPAT, KSU_FEATURE_MAGISK_COMPAT, KSU_FEATURE_KASUMI_SUCOMPAT}) {
        const auto [value, supported] = get_feature(id);
        if (supported) {
            features[id] = value;
        } else if (id == KSU_FEATURE_MAGISK_COMPAT) {
            features[id] = 0;
        }
    }
}

void close_su_view_after_path_failure() {
    (void)set_feature(KSU_FEATURE_MAGISK_COMPAT, 0);
    (void)set_feature(KSU_FEATURE_KASUMI_SUCOMPAT, 0);
    (void)set_feature(KSU_FEATURE_SU_COMPAT, 0);
    kill_msud_locked();
    LOGE("Custom su path is unavailable; keeping su disabled and preserving its saved "
         "configuration");
}

int disable_kasumi_sucompat() {
    const auto [magisk, magisk_supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
    const auto [kasumi, kasumi_supported] = get_feature(KSU_FEATURE_KASUMI_SUCOMPAT);
    if (magisk_supported && magisk != 0 && set_feature(KSU_FEATURE_MAGISK_COMPAT, 0) < 0)
        return 1;
    kill_msud_locked();
    if (((kasumi_supported && kasumi != 0) || (magisk_supported && magisk != 0)) &&
        set_feature(KSU_FEATURE_SU_COMPAT, 1) < 0)
        return 1;
    if (reset_su_path_locked() != 0) {
        LOGE("Failed to reset the su path while disabling Kasumi");
        return 1;
    }
    return 0;
}

int apply_sucompat_config(std::map<uint32_t, uint64_t>& features) {
    const auto [kasumi_enabled, kasumi_supported] = get_feature(KSU_FEATURE_KASUMI);
    if (kasumi_supported && kasumi_enabled == 0 &&
        (features.count(KSU_FEATURE_SU_COMPAT) || features.count(KSU_FEATURE_KASUMI_SUCOMPAT) ||
         features.count(KSU_FEATURE_MAGISK_COMPAT))) {
        if (disable_kasumi_sucompat() != 0)
            return -1;
        if (features[KSU_FEATURE_KASUMI_SUCOMPAT] != 0 || features[KSU_FEATURE_MAGISK_COMPAT] != 0)
            features[KSU_FEATURE_SU_COMPAT] = 1;
        features[KSU_FEATURE_KASUMI_SUCOMPAT] = 0;
        features[KSU_FEATURE_MAGISK_COMPAT] = 0;
    }
    const bool requested_ksm =
        (features.count(KSU_FEATURE_KASUMI_SUCOMPAT) &&
         features.at(KSU_FEATURE_KASUMI_SUCOMPAT) != 0) ||
        (features.count(KSU_FEATURE_MAGISK_COMPAT) && features.at(KSU_FEATURE_MAGISK_COMPAT) != 0);
    bool custom_path = false;
    if (requested_ksm && restore_su_path(&custom_path) != 0) {
        close_su_view_after_path_failure();
        return -1;
    }
    if (requested_ksm && custom_path &&
        (!get_feature(KSU_FEATURE_KASUMI_SUCOMPAT).second ||
         (features.count(KSU_FEATURE_MAGISK_COMPAT) &&
          features.at(KSU_FEATURE_MAGISK_COMPAT) != 0 &&
          !get_feature(KSU_FEATURE_MAGISK_COMPAT).second))) {
        close_su_view_after_path_failure();
        return -1;
    }
    normalize_sucompat_config(features);
    if (get_feature(KSU_FEATURE_KASUMI).second && !kagami::kasumi::is_available() &&
        ((features.count(KSU_FEATURE_KASUMI_SUCOMPAT) &&
          features.at(KSU_FEATURE_KASUMI_SUCOMPAT) != 0) ||
         (features.count(KSU_FEATURE_MAGISK_COMPAT) &&
          features.at(KSU_FEATURE_MAGISK_COMPAT) != 0))) {
        if (custom_path) {
            close_su_view_after_path_failure();
            return -1;
        }
        features[KSU_FEATURE_KASUMI_SUCOMPAT] = 0;
        features[KSU_FEATURE_MAGISK_COMPAT] = 0;
        features[KSU_FEATURE_SU_COMPAT] = 1;
        LOGW("Kasumi was not initialized; restoring classic su_compat");
    }
    const auto classic = features.find(KSU_FEATURE_SU_COMPAT);
    const auto magisk = features.find(KSU_FEATURE_MAGISK_COMPAT);
    const auto kasumi = features.find(KSU_FEATURE_KASUMI_SUCOMPAT);
    const bool has_classic = classic != features.end();
    const bool has_magisk = magisk != features.end();
    const bool has_kasumi = kasumi != features.end();
    const bool want_classic = has_classic && classic->second != 0;
    const bool want_magisk = has_magisk && magisk->second != 0;
    const bool want_kasumi = has_kasumi && kasumi->second != 0;
    int applied = 0;

    if (!has_classic && !has_magisk && !has_kasumi) {
        return 0;
    }

    if (want_magisk) {
        if (ensure_msud_running_locked() != 0) {
            LOGW("Failed to start msud before enabling magisk_compat");
            if (custom_path) {
                close_su_view_after_path_failure();
                return -1;
            }
            settle_sucompat_values(features);
            if (features[KSU_FEATURE_MAGISK_COMPAT] == 0) {
                kill_msud_locked();
            }
            return 0;
        }
        const int ret = set_feature(KSU_FEATURE_MAGISK_COMPAT, 1);
        if (ret >= 0) {
            features[KSU_FEATURE_SU_COMPAT] = 0;
            features[KSU_FEATURE_MAGISK_COMPAT] = 1;
            features[KSU_FEATURE_KASUMI_SUCOMPAT] = 1;
            LOGI("Set feature magisk_compat to 1 with kasumi_sucompat");
            return 1;
        }

        LOGW("Failed to enable magisk_compat: %d", ret);
        if (custom_path) {
            close_su_view_after_path_failure();
            return -1;
        }
        settle_sucompat_values(features);
        if (features[KSU_FEATURE_MAGISK_COMPAT] == 0) {
            kill_msud_locked();
        }
        return 0;
    }

    if (has_magisk) {
        const int ret = set_feature(KSU_FEATURE_MAGISK_COMPAT, 0);
        if (ret >= 0) {
            features[KSU_FEATURE_MAGISK_COMPAT] = 0;
            kill_msud_locked();
            applied++;
        } else {
            const auto [_, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
            if (!supported) {
                features[KSU_FEATURE_MAGISK_COMPAT] = 0;
                kill_msud_locked();
            } else {
                LOGW("Failed to disable magisk_compat: %d", ret);
                if (custom_path) {
                    close_su_view_after_path_failure();
                    return -1;
                }
                settle_sucompat_values(features);
                return applied;
            }
        }
    }

    if (want_kasumi) {
        const int ret = set_feature(KSU_FEATURE_KASUMI_SUCOMPAT, 1);
        if (ret >= 0) {
            features[KSU_FEATURE_SU_COMPAT] = 0;
            features[KSU_FEATURE_KASUMI_SUCOMPAT] = 1;
            LOGI("Set feature kasumi_sucompat to 1");
            return applied + 1;
        }

        if (custom_path) {
            close_su_view_after_path_failure();
            return -1;
        }
        LOGW("Failed to enable kasumi_sucompat: %d; restoring classic su_compat", ret);
        const int fallback = set_feature(KSU_FEATURE_SU_COMPAT, 1);
        features[KSU_FEATURE_KASUMI_SUCOMPAT] = 0;
        if (fallback >= 0) {
            features[KSU_FEATURE_SU_COMPAT] = 1;
            LOGI("Restored classic su_compat after Kasumi provider failure");
            return applied + 1;
        }
        LOGW("Failed to restore classic su_compat: %d", fallback);
    } else if (want_classic) {
        const int ret = set_feature(KSU_FEATURE_SU_COMPAT, 1);
        if (ret >= 0) {
            features[KSU_FEATURE_SU_COMPAT] = 1;
            features[KSU_FEATURE_KASUMI_SUCOMPAT] = 0;
            LOGI("Set feature su_compat to 1");
            return applied + 1;
        }
        LOGW("Failed to enable classic su_compat: %d", ret);
    } else {
        if (has_kasumi) {
            const int ret = set_feature(KSU_FEATURE_KASUMI_SUCOMPAT, 0);
            if (ret >= 0) {
                applied++;
            } else {
                LOGW("Failed to disable kasumi_sucompat: %d", ret);
            }
        }
        if (has_classic) {
            const int ret = set_feature(KSU_FEATURE_SU_COMPAT, 0);
            if (ret >= 0) {
                applied++;
            } else {
                LOGW("Failed to disable classic su_compat: %d", ret);
            }
        }
    }

    settle_sucompat_values(features);
    return applied;
}

std::pair<uint32_t, bool> parse_feature_id(const std::string& id) {
    // Try numeric first
    uint32_t num = 0;
    if (parse_uint32(id, &num)) {
        for (const auto& [name, fid] : get_feature_map()) {
            if (fid == num)
                return {num, true};
        }
        return {0, false};
    }

    // Try name lookup
    auto it = get_feature_map().find(id);
    if (it != get_feature_map().end()) {
        return {it->second, true};
    }

    return {0, false};
}

const char* feature_id_to_name(uint32_t id) {
    for (const auto& [name, fid] : get_feature_map()) {
        if (fid == id) {
            return name.c_str();
        }
    }
    return "unknown";
}

const char* feature_id_to_description(uint32_t id) {
    auto it = get_feature_descriptions().find(id);
    if (it != get_feature_descriptions().end()) {
        return it->second;
    }
    return "Unknown feature";
}

std::map<uint32_t, uint64_t> get_current_feature_values() {
    std::map<uint32_t, uint64_t> features;
    for (const auto& [_, id] : get_feature_map()) {
        auto [value, supported] = get_feature(id);
        if (supported) {
            features[id] = value;
        }
    }
    normalize_sucompat_config(features);
    return features;
}

int save_feature_config_files(const std::map<uint32_t, uint64_t>& features) {
    const std::string config_path = std::string(FEATURE_CONFIG_PATH);
    std::string text = "# KernelSU feature configuration\n";
    for (const auto& [name, id] : get_feature_map()) {
        auto it = features.find(id);
        if (it == features.end())
            continue;
        text += name;
        text += '=';
        text += std::to_string(it->second);
        text += '\n';
    }
    if (!write_file_atomic(config_path, text) || !sync_feature_directory()) {
        LOGE("Failed to persist text feature configuration");
        return 1;
    }

    if (save_binary_config(features) != 0) {
        LOGE("Failed to save feature binary config");
        return 1;
    }

    LOGI("Saved feature config to %s", config_path.c_str());
    return 0;
}

bool is_sucompat_feature_id(uint32_t feature_id) {
    return feature_id == KSU_FEATURE_SU_COMPAT || feature_id == KSU_FEATURE_MAGISK_COMPAT ||
           feature_id == KSU_FEATURE_KASUMI_SUCOMPAT;
}

int feature_save_config_locked() {
    return save_feature_config_files(get_current_feature_values());
}

int feature_set_impl(const std::string& id, uint32_t feature_id, uint64_t value) {
    const auto [kasumi_enabled, kasumi_supported] = get_feature(KSU_FEATURE_KASUMI);
    if (value != 0 &&
        (feature_id == KSU_FEATURE_MAGISK_COMPAT || feature_id == KSU_FEATURE_KASUMI_SUCOMPAT) &&
        kasumi_supported && (kasumi_enabled == 0 || !kagami::kasumi::is_available())) {
        LOGE("Kasumi is disabled or not initialized; enable the kasumi feature first");
        return 1;
    }
    if (value != 0 &&
        (feature_id == KSU_FEATURE_MAGISK_COMPAT || feature_id == KSU_FEATURE_KASUMI_SUCOMPAT) &&
        restore_su_path() != 0) {
        LOGE("Failed to restore su path before enabling KSM");
        return 1;
    }
    if (feature_id == KSU_FEATURE_KASUMI && value == 0 && disable_kasumi_sucompat() != 0) {
        return 1;
    }
    if (feature_id == KSU_FEATURE_MAGISK_COMPAT && value != 0 &&
        ensure_msud_running_locked() != 0) {
        const auto [current, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
        if (!supported || current == 0) {
            kill_msud_locked();
        }
        LOGE("Failed to start msud before enabling magisk_compat");
        return 1;
    }

    const int ret = set_feature(feature_id, value);
    if (ret < 0) {
        if (feature_id == KSU_FEATURE_MAGISK_COMPAT && value != 0) {
            const auto [current, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
            if (!supported || current == 0) {
                kill_msud_locked();
            }
        }
        LOGE("Failed to set feature %s to %" PRIu64, id.c_str(), value);
        return 1;
    }

    if (feature_id == KSU_FEATURE_SULOG && value != 0 && ensure_sulogd_running() != 0) {
        LOGW("Failed to ensure sulogd is running after enabling sulog");
    }

    if (feature_id == KSU_FEATURE_MAGISK_COMPAT && value == 0) {
        kill_msud_locked();
    }

    if (feature_id == KSU_FEATURE_YUKIZYGISK && refresh_yukizygisk_early_snapshot() != 0) {
        LOGW("Failed to refresh YukiZygisk early snapshot after feature change");
    }

    printf("Feature '%s' set to %" PRIu64 " (%s)\n", feature_id_to_name(feature_id), value,
           value != 0 ? "enabled" : "disabled");
    return 0;
}

bool restore_config_file(const std::string& path, const std::optional<std::string>& previous) {
    if (previous)
        return write_file_atomic(path, *previous) && sync_feature_directory();
    return (unlink(path.c_str()) == 0 || errno == ENOENT) && sync_feature_directory();
}

int feature_update_locked(const std::string& id, uint32_t feature_id, uint64_t value,
                          bool persist) {
    const bool disabling_kasumi = feature_id == KSU_FEATURE_KASUMI && value == 0;
    const bool coupled = is_sucompat_feature_id(feature_id) || disabling_kasumi;
    auto previous_sucompat = get_current_feature_values();
    const auto previous_feature = get_feature(feature_id);
    const auto previous_text_config = read_file(FEATURE_CONFIG_PATH);
    const auto previous_binary_config = read_file(get_feature_config_path());
    const auto previous_path_file =
        disabling_kasumi ? read_file(SU_PATH_CONFIG_PATH) : std::nullopt;
    ksu_su_path_config previous_path{};
    bool restore_path = false;
    if (disabling_kasumi) {
        const int ret = get_su_path_config(&previous_path);
        if (ret != 0 && ret != -ENOTTY && ret != -EOPNOTSUPP)
            return 1;
        restore_path = ret == 0;
        previous_path.flags = 0;
    }

    const auto restore_runtime = [&] {
        if (disabling_kasumi && previous_feature.second)
            (void)set_feature(feature_id, previous_feature.first);
        else if (!coupled && previous_feature.second)
            (void)feature_set_impl(id, feature_id, previous_feature.first);
        if (restore_path) {
            const bool file_restored = restore_config_file(SU_PATH_CONFIG_PATH, previous_path_file);
            if (set_su_path_config(previous_path) != 0 || !file_restored) {
                LOGE("Failed to restore the previous su path after disabling Kasumi failed");
                close_su_view_after_path_failure();
                return;
            }
        }
        if (coupled)
            (void)apply_sucompat_config(previous_sucompat);
    };

    if (feature_set_impl(id, feature_id, value) != 0) {
        if (coupled)
            restore_runtime();
        return 1;
    }
    if (!persist || feature_save_config_locked() == 0)
        return 0;

    LOGW("Failed to persist feature %s; restoring previous runtime state", id.c_str());
    restore_runtime();
    const bool text_restored = restore_config_file(FEATURE_CONFIG_PATH, previous_text_config);
    const bool binary_restored =
        restore_config_file(get_feature_config_path(), previous_binary_config);
    if (!text_restored || !binary_restored)
        LOGW("Failed to restore the previous feature configuration files");
    return 1;
}

}  // namespace

int feature_get(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);

    if (!supported) {
        printf("Feature '%s' is not supported by kernel\n", id.c_str());
        return 0;
    }

    printf("Feature: %s (%u)\n", feature_id_to_name(feature_id), feature_id);
    printf("Description: %s\n", feature_id_to_description(feature_id));
    printf("Value: %" PRIu64 "\n", value);
    printf("Status: ");
    terminal::status(stdout, value != 0 ? "enabled" : "disabled", value != 0);
    printf("\n");

    return 0;
}

int feature_set(const std::string& id, uint64_t value) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    if (!is_sucompat_feature_id(feature_id) && feature_id != KSU_FEATURE_KASUMI) {
        return feature_set_impl(id, feature_id, value);
    }

    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }
    if (feature_id == KSU_FEATURE_KASUMI && value == 0)
        return feature_update_locked(id, feature_id, value, false);
    return feature_set_impl(id, feature_id, value);
}

int feature_set_and_save(const std::string& id, uint64_t value) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    return feature_update_locked(id, feature_id, value, true);
}

void feature_list() {
    terminal::heading(stdout, "Available Features:\n");
    printf("================================================================================\n");

    for (const auto& [name, id] : get_feature_map()) {
        auto [value, supported] = get_feature(id);

        const char* status;
        if (!supported) {
            status = "NOT_SUPPORTED";
        } else if (value != 0) {
            status = "ENABLED";
        } else {
            status = "DISABLED";
        }

        printf("[");
        terminal::status(stdout, status, supported && value != 0);
        printf("] %s (ID=%u)\n", name.c_str(), id);
        printf("    %s\n", feature_id_to_description(id));
    }
}

int feature_check(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        printf("unsupported\n");
        return 1;
    }

    // TODO: Check if this feature is managed by any module
    // For now, just check kernel support

    auto [value, supported] = get_feature(feature_id);
    if (supported) {
        printf("supported\n");
        return 0;
    } else {
        printf("unsupported\n");
        return 1;
    }
}

namespace {

std::map<uint32_t, uint64_t> parse_text_feature_config(const std::string& content) {
    std::map<uint32_t, uint64_t> features;
    for_each_line(content, [&](std::string_view raw_line) {
        const std::string_view line = trim_view(raw_line);
        if (line.empty() || line[0] == '#')
            return;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return;
        const std::string key(trim_view(line.substr(0, eq)));
        const std::string val(trim_view(line.substr(eq + 1)));
        const auto [feature_id, valid] = parse_feature_id(key);
        if (!valid)
            return;
        uint64_t value = 0;
        if (!parse_uint64(val, &value)) {
            LOGW("Invalid value for feature %s: %s", key.c_str(), val.c_str());
            return;
        }
        features[feature_id] = value;
    });
    return features;
}

}  // namespace

int feature_load_config() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    const std::string config_path = std::string(FEATURE_CONFIG_PATH);
    auto content = read_file(config_path);
    if (!content) {
        LOGI("No feature config file found");
        return 0;
    }

    std::map<uint32_t, uint64_t> loaded_features;
    for_each_line(*content, [&](std::string_view raw_line) {
        const std::string_view line = trim_view(raw_line);
        if (line.empty() || line[0] == '#')
            return;

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return;

        const std::string key(trim_view(line.substr(0, eq)));
        const std::string val(trim_view(line.substr(eq + 1)));

        auto [feature_id, valid] = parse_feature_id(key);
        if (valid) {
            uint64_t value = 0;
            if (parse_uint64(val, &value)) {
                loaded_features[feature_id] = value;
                if (feature_id != KSU_FEATURE_SU_COMPAT &&
                    feature_id != KSU_FEATURE_MAGISK_COMPAT &&
                    feature_id != KSU_FEATURE_KASUMI_SUCOMPAT) {
                    set_feature(feature_id, value);
                    LOGI("Loaded feature %s = %" PRIu64, key.c_str(), value);
                }
            } else {
                LOGW("Invalid value for feature %s: %s", key.c_str(), val.c_str());
            }
        }
    });

    const auto requested_features = loaded_features;
    if (apply_sucompat_config(loaded_features) < 0)
        return 1;

    const int save_result = loaded_features != requested_features
                                ? save_feature_config_files(loaded_features)
                                : save_binary_config(loaded_features);
    if (save_result != 0) {
        LOGW("Failed to sync loaded feature configuration");
    }

    return 0;
}

int feature_save_config() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }
    return feature_save_config_locked();
}

int refresh_sucompat_vfs() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    const auto [kasumi_value, kasumi_supported] = get_feature(KSU_FEATURE_KASUMI_SUCOMPAT);
    if (!kasumi_supported || kasumi_value == 0) {
        return 0;
    }
    const int ret = set_feature(KSU_FEATURE_KASUMI_SUCOMPAT, 1);
    if (ret >= 0) {
        return 0;
    }

    const auto [magisk_value, magisk_supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
    if (magisk_supported && magisk_value != 0) {
        LOGW("Failed to refresh vnode-backed su while magisk_compat is active: %d", ret);
        return ret;
    }
    (void)kill_msud_locked();

    LOGW("Failed to rebind kasumi_sucompat: %d; restoring classic su_compat", ret);
    const int fallback = set_feature(KSU_FEATURE_SU_COMPAT, 1);
    if (fallback < 0) {
        LOGW("Failed to restore classic su_compat after rebind failure: %d", fallback);
        return ret;
    }
    if (feature_save_config_locked() != 0) {
        LOGW("Failed to persist classic su_compat fallback after rebind failure");
    }
    return 0;
}

std::optional<std::map<uint32_t, uint64_t>> load_binary_config() {
    std::map<uint32_t, uint64_t> features;

    const auto blob = read_file(get_feature_config_path());
    if (!blob) {
        LOGI("Feature binary config not found");
        return std::nullopt;
    }

    // Fixed-layout record file: magic, version, count, then count*(u32 id, u64
    // value). Every field is bounds-checked against the blob, which the stream
    // version only did loosely via ifs.good().
    size_t offset = 0;
    const auto take = [&blob, &offset](void* out, size_t size) {
        if (offset + size > blob->size())
            return false;
        std::memcpy(out, blob->data() + offset, size);
        offset += size;
        return true;
    };

    uint32_t magic = 0;
    if (!take(&magic, sizeof(magic)) || magic != FEATURE_MAGIC) {
        LOGW("Invalid feature config magic: expected 0x%08x, got 0x%08x", FEATURE_MAGIC, magic);
        return std::nullopt;
    }

    uint32_t version = 0;
    if (!take(&version, sizeof(version))) {
        LOGW("Feature config truncated before version");
        return std::nullopt;
    }
    if (version != FEATURE_VERSION) {
        LOGW("Feature config version mismatch: expected %u, got %u", FEATURE_VERSION, version);
        return std::nullopt;
    }

    uint32_t count = 0;
    if (!take(&count, sizeof(count))) {
        LOGW("Feature config truncated before count");
        return std::nullopt;
    }

    constexpr size_t record_size = sizeof(uint32_t) + sizeof(uint64_t);
    if (count != (blob->size() - offset) / record_size ||
        (blob->size() - offset) % record_size != 0) {
        LOGW("Feature config has an invalid record count or size");
        return std::nullopt;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = 0;
        uint64_t value = 0;
        if (!take(&id, sizeof(id)) || !take(&value, sizeof(value))) {
            LOGW("Feature config truncated at entry %u of %u", i, count);
            return std::nullopt;
        }
        features[id] = value;
    }

    LOGI("Loaded %zu features from binary config", features.size());
    return features;
}

int save_binary_config(const std::map<uint32_t, uint64_t>& features) {
    ensure_dir_exists(WORKING_DIR);

    std::string blob;
    blob.reserve((3 * sizeof(uint32_t)) +
                 (features.size() * (sizeof(uint32_t) + sizeof(uint64_t))));
    const auto put = [&blob](const void* data, size_t size) {
        blob.append(static_cast<const char*>(data), size);
    };

    const uint32_t magic = FEATURE_MAGIC;
    put(&magic, sizeof(magic));
    const uint32_t version = FEATURE_VERSION;
    put(&version, sizeof(version));
    const uint32_t count = static_cast<uint32_t>(features.size());
    put(&count, sizeof(count));
    for (const auto& [id, value] : features) {
        put(&id, sizeof(id));
        put(&value, sizeof(value));
    }

    if (!write_file_atomic(get_feature_config_path(), blob) || !sync_feature_directory()) {
        LOGE("Failed to persist binary feature configuration");
        return -1;
    }

    LOGI("Saved %zu features to binary config", features.size());
    return 0;
}

namespace {

int apply_config_locked(std::map<uint32_t, uint64_t>& features) {
    LOGI("Applying feature configuration to kernel...");

    int applied = 0;
    for (const auto& [id, value] : features) {
        if (id == KSU_FEATURE_SU_COMPAT || id == KSU_FEATURE_MAGISK_COMPAT ||
            id == KSU_FEATURE_KASUMI_SUCOMPAT) {
            continue;
        }
        const int ret = set_feature(id, value);
        if (ret >= 0) {
            if (id == KSU_FEATURE_SULOG && value != 0 && ensure_sulogd_running() != 0) {
                LOGW("Failed to ensure sulogd is running while applying config");
            }
            LOGI("Set feature %s to %" PRIu64, feature_id_to_name(id), value);
            applied++;
        } else {
            LOGW("Failed to set feature %u: %d", id, ret);
        }
    }

    const int su_applied = apply_sucompat_config(features);
    if (su_applied < 0)
        return 1;
    applied += su_applied;

    LOGI("Applied %d features successfully", applied);
    return 0;
}

}  // namespace

void apply_config(std::map<uint32_t, uint64_t>& features) {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return;
    }
    apply_config_locked(features);
}

int init_features() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    LOGI("Initializing features from config...");

    if (restore_su_path() != 0)
        LOGE("Persisted su path could not be restored");

    auto binary_features = load_binary_config();
    bool recovered_from_text = false;
    if (!binary_features) {
        const auto text_config = read_file(FEATURE_CONFIG_PATH);
        if (text_config) {
            auto text_features = parse_text_feature_config(*text_config);
            if (!text_features.empty()) {
                LOGW("Recovering feature configuration from %s", FEATURE_CONFIG_PATH);
                binary_features = std::move(text_features);
                recovered_from_text = true;
            }
        }
    }
    if (!binary_features) {
        if (access(get_feature_config_path().c_str(), F_OK) == 0) {
            LOGE("Invalid feature config without usable text backup");
            return 1;
        }
        LOGI("No persisted feature configuration found");
        binary_features.emplace();
    }
    auto features = std::move(*binary_features);

    // Versions before KSU_FEATURE_HIDE_BOOTLOADER used a standalone marker.
    // Import it only when the running kernel supports the formal feature, then
    // remove it after both feature config formats have been saved successfully.
    const bool has_legacy_hide_bootloader = access(LEGACY_HIDE_BOOTLOADER_CONFIG, F_OK) == 0;
    bool consume_legacy_hide_bootloader = false;
    if (has_legacy_hide_bootloader) {
        const auto [_, supported] = get_feature(KSU_FEATURE_HIDE_BOOTLOADER);
        if (supported) {
            consume_legacy_hide_bootloader = true;
            if (features.find(KSU_FEATURE_HIDE_BOOTLOADER) == features.end()) {
                features[KSU_FEATURE_HIDE_BOOTLOADER] = 1;
                LOGI("Migrating legacy hide-bootloader marker to feature %u",
                     KSU_FEATURE_HIDE_BOOTLOADER);
            }
        } else {
            LOGW("Keeping legacy hide-bootloader marker: feature %u is unsupported",
                 KSU_FEATURE_HIDE_BOOTLOADER);
        }
    }

    // Get managed features from active modules and skip them during init
    auto managed_features_map = get_managed_features();
    bool sucompat_group_managed = false;
    if (!managed_features_map.empty()) {
        LOGI("Found %zu modules managing features", managed_features_map.size());

        // Build a set of all managed feature IDs to skip
        for (const auto& [module_id, feature_list] : managed_features_map) {
            LOGI("Module '%s' manages %zu feature(s)", module_id.c_str(), feature_list.size());
            for (const auto& feature_name : feature_list) {
                auto [feature_id, valid] = parse_feature_id(feature_name);
                if (valid) {
                    if (feature_id == KSU_FEATURE_SU_COMPAT ||
                        feature_id == KSU_FEATURE_MAGISK_COMPAT ||
                        feature_id == KSU_FEATURE_KASUMI_SUCOMPAT) {
                        sucompat_group_managed = true;
                    }
                    // Remove managed features from config, let modules control them
                    auto it = features.find(feature_id);
                    if (it != features.end()) {
                        features.erase(it);
                        LOGI("  - Skipping managed feature '%s' (controlled by module: %s)",
                             feature_name.c_str(), module_id.c_str());
                    } else {
                        LOGI("  - Feature '%s' is managed by module '%s', skipping",
                             feature_name.c_str(), module_id.c_str());
                    }
                } else {
                    LOGW("  - Unknown managed feature '%s' from module '%s', ignoring",
                         feature_name.c_str(), module_id.c_str());
                }
            }
        }
    }
    if (sucompat_group_managed) {
        features.erase(KSU_FEATURE_SU_COMPAT);
        features.erase(KSU_FEATURE_MAGISK_COMPAT);
        features.erase(KSU_FEATURE_KASUMI_SUCOMPAT);
        LOGI("Skipping coupled sucompat features because a module manages one");
    }

    if (features.empty()) {
        LOGI("No features to apply, skipping initialization");
        return 0;
    }

    const auto requested_features = features;
    if (apply_config_locked(features) != 0)
        return 1;

    // Save the configuration (excluding managed features). A legacy migration
    // updates the human-readable config too, so a later `feature load` cannot
    // silently drop the migrated value.
    const int save_result =
        recovered_from_text || consume_legacy_hide_bootloader || features != requested_features
            ? save_feature_config_files(features)
            : save_binary_config(features);
    if (save_result != 0) {
        LOGW("Failed to save initialized feature configuration");
        return 1;
    }
    if (consume_legacy_hide_bootloader) {
        if (unlink(LEGACY_HIDE_BOOTLOADER_CONFIG) != 0 && errno != ENOENT) {
            LOGW("Failed to remove legacy hide-bootloader marker: %s", strerror(errno));
        } else {
            LOGI("Removed legacy hide-bootloader marker");
        }
    }
    LOGI("Saved feature configuration to file");

    return 0;
}

}  // namespace ksud
