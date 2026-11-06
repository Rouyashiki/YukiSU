#include "../assets.hpp"
#include "restorecon.hpp"

#include <sys/stat.h>
#include <filesystem>
#include <string>

#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

namespace ksud {

// Hand-written asset helpers.
// Stage YukiZygisk payloads when embedded.
int ensure_yukizygisk(bool ignore_if_exist) {
    struct Payload {
        const char* asset;
        const char* dest;
    };
    static const Payload payload[] = {
        {"libzygisk64.so", ZCORE64_PATH},      {"libzygisk32.so", ZCORE32_PATH},
        {"libyukizncore64.so", ZNCORE64_PATH}, {"libyukizncore32.so", ZNCORE32_PATH},
        {"libyloader64.so", ZYLOADER64_PATH},  {"libyloader32.so", ZYLOADER32_PATH},
    };
    static constexpr const char* legacy_payload[] = {
        "/data/adb/ksu/lib/yukizygisk/libzygisk.so",
        "/data/adb/ksu/lib/yukizygisk/libyukizncore.so",
        "/data/adb/ksu/lib/yukizygisk/libyukilinker.so",
        "/data/adb/ksu/lib/yukizygisk/libyukilinker32.so",
        "/data/adb/ksu/lib/yukizygisk/libyukilinker64.so",
    };

    bool embedded = false;
    int result = 0;
    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (get_asset(p.asset, data, size)) {
            embedded = true;
            break;
        }
    }
    if (!embedded) {
        return 0;
    }

    if (!ensure_dir_exists(YUKIZYGISK_DIR)) {
        LOGE("yukizygisk: failed to create %s", YUKIZYGISK_DIR);
        return 1;
    }

    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!get_asset(p.asset, data, size)) {
            LOGE("yukizygisk: embedded payload missing: %s", p.asset);
            result = 1;
            continue;
        }

        (void)ignore_if_exist;
        if (!copy_asset_to_file(p.asset, p.dest)) {
            LOGE("yukizygisk: failed to stage %s", p.dest);
            result = 1;
            continue;
        }
        chmod(p.dest, 0644);
        lsetfilecon(p.dest, SYSTEM_LIB_CON);
        LOGI("yukizygisk: staged %s", p.dest);
    }
    if (result == 0) {
        for (const char* path : legacy_payload) {
            std::error_code ec;
            if (!std::filesystem::remove(path, ec) && ec) {
                LOGW("yukizygisk: failed to remove legacy payload %s: %s", path,
                     ec.message().c_str());
            }
        }
    }
    return result;
}

}  // namespace ksud
