#include "yukizygisk_snapshot.hpp"
#include "assets.hpp"
#include "core/json.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "userspace/zygisk/daemon/native_modules.hpp"
#include "userspace/zygisk/load_policy.hpp"
#include "utils.hpp"
#include "yukizygisk_diagnostics.hpp"

#include "uapi/yukizygisk.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ksud {

namespace {

namespace fs = std::filesystem;
using yukizygisk::native::NativeModule;

constexpr const char* kSnapshotDirName = "yukizygisk";
constexpr const char* kManifestName = "native_snapshot.bin";
constexpr const char* kModulesDirName = "modules";
constexpr const char* kLoader64Name = "libyloader64.so";
constexpr const char* kLoader32Name = "libyloader32.so";
constexpr const char* kNativeCore64Name = "libyukizncore64.so";
constexpr const char* kNativeCore32Name = "libyukizncore32.so";
constexpr const char* kSystemLinker64 = "/system/bin/linker64";
constexpr const char* kSystemLinker32 = "/system/bin/linker";
constexpr const char* kMetadataFileCon = "u:object_r:metadata_file:s0";

fs::path preinit_ksu_dir() {
    std::error_code ec;
    if (fs::is_directory("/metadata/watchdog", ec)) {
        return PREINIT_DIR_WATCHDOG;
    }
    return PREINIT_DIR_DEFAULT;
}

bool range_ok(size_t offset, size_t size, size_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool string_table_equals(const char* table, size_t table_size, uint32_t offset, const char* want) {
    if (offset >= table_size)
        return false;
    size_t const want_len = strlen(want);
    if (want_len >= table_size - offset)
        return false;
    return memcmp(table + offset, want, want_len + 1) == 0;
}

bool safe_component(const std::string& s) {
    if (s.empty() || s.size() >= YZ_NATIVE_MODULE_ID_MAX)
        return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
    });
}

void remove_snapshot_dir(const fs::path& base) {
    std::error_code ec;
    fs::remove_all(base / kSnapshotDirName, ec);
}

bool copy_regular_file(const fs::path& src, const fs::path& dst) {
    return copy_file_data(src, dst);
}

bool stage_asset_or_file(const char* asset, const fs::path& fallback, const fs::path& dst) {
    if (copy_asset_to_file(asset, dst.string())) {
        chmod(dst.c_str(), 0644);
        (void)lsetfilecon(dst, kMetadataFileCon);
        return true;
    }
    if (!copy_regular_file(fallback, dst))
        return false;
    chmod(dst.c_str(), 0644);
    (void)lsetfilecon(dst, kMetadataFileCon);
    return true;
}

uint64_t resolve_linker_sym64(const char* path, const char* want) {
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
        close(fd);
        return 0;
    }

    void* map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return 0;

    const auto* base = static_cast<const uint8_t*>(map);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    uint64_t result = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64 &&
        eh->e_shoff > 0 && eh->e_shnum > 0) {
        size_t const file_size = static_cast<size_t>(st.st_size);
        size_t const sh_size = static_cast<size_t>(eh->e_shnum) * sizeof(Elf64_Shdr);
        if (eh->e_shentsize != sizeof(Elf64_Shdr) ||
            !range_ok(static_cast<size_t>(eh->e_shoff), sh_size, file_size)) {
            munmap(map, static_cast<size_t>(st.st_size));
            return 0;
        }

        const auto* sh = reinterpret_cast<const Elf64_Shdr*>(base + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum && result == 0; i++) {
            if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum)
                continue;
            if (!range_ok(static_cast<size_t>(sh[i].sh_offset), static_cast<size_t>(sh[i].sh_size),
                          file_size))
                continue;
            const Elf64_Shdr& str_sh = sh[sh[i].sh_link];
            if (!range_ok(static_cast<size_t>(str_sh.sh_offset),
                          static_cast<size_t>(str_sh.sh_size), file_size))
                continue;
            const auto* syms = reinterpret_cast<const Elf64_Sym*>(base + sh[i].sh_offset);
            const char* strs = reinterpret_cast<const char*>(base + str_sh.sh_offset);
            size_t const n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t j = 0; j < n; j++) {
                if (string_table_equals(strs, static_cast<size_t>(str_sh.sh_size), syms[j].st_name,
                                        want)) {
                    result = syms[j].st_value;
                    break;
                }
            }
        }
    }
    munmap(map, static_cast<size_t>(st.st_size));
    return result;
}

uint64_t resolve_linker_sym32(const char* path, const char* want) {
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Elf32_Ehdr))) {
        close(fd);
        return 0;
    }

    void* map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return 0;

    const auto* base = static_cast<const uint8_t*>(map);
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(base);
    uint64_t result = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS32 &&
        eh->e_shoff > 0 && eh->e_shnum > 0) {
        size_t const file_size = static_cast<size_t>(st.st_size);
        size_t const sh_size = static_cast<size_t>(eh->e_shnum) * sizeof(Elf32_Shdr);
        if (eh->e_shentsize == sizeof(Elf32_Shdr) &&
            range_ok(static_cast<size_t>(eh->e_shoff), sh_size, file_size)) {
            const auto* sh = reinterpret_cast<const Elf32_Shdr*>(base + eh->e_shoff);
            for (int i = 0; i < eh->e_shnum && result == 0; i++) {
                if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum)
                    continue;
                if (!range_ok(sh[i].sh_offset, sh[i].sh_size, file_size))
                    continue;
                const Elf32_Shdr& str_sh = sh[sh[i].sh_link];
                if (!range_ok(str_sh.sh_offset, str_sh.sh_size, file_size))
                    continue;
                const auto* syms = reinterpret_cast<const Elf32_Sym*>(base + sh[i].sh_offset);
                const char* strs = reinterpret_cast<const char*>(base + str_sh.sh_offset);
                size_t const n = sh[i].sh_size / sizeof(Elf32_Sym);
                for (size_t j = 0; j < n; j++) {
                    if (string_table_equals(strs, str_sh.sh_size, syms[j].st_name, want)) {
                        result = syms[j].st_value;
                        break;
                    }
                }
            }
        }
    }
    munmap(map, static_cast<size_t>(st.st_size));
    return result;
}

uint64_t resolve_first(const char* path, bool compat, const char* const* cands, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint64_t const off =
            compat ? resolve_linker_sym32(path, cands[i]) : resolve_linker_sym64(path, cands[i]);
        if (off != 0)
            return off;
    }
    return 0;
}

int elf_class(const fs::path& path) {
    unsigned char ident[EI_NIDENT]{};
    int const fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return ELFCLASSNONE;
    ssize_t const n = read(fd, ident, sizeof(ident));
    close(fd);
    if (n != static_cast<ssize_t>(sizeof(ident)) || memcmp(ident, ELFMAG, SELFMAG) != 0 ||
        ident[EI_DATA] != ELFDATA2LSB)
        return ELFCLASSNONE;
    return ident[EI_CLASS];
}

std::vector<std::pair<std::string, fs::path>> collect_active_module_roots() {
    std::map<std::string, fs::path> modules;
    std::map<std::string, bool> skipped_modules;
    std::error_code ec;

    for (const char* root : {MODULE_UPDATE_DIR, MODULE_DIR}) {
        if (!fs::is_directory(root, ec))
            continue;
        for (auto it = fs::directory_iterator(root, ec); it != fs::directory_iterator() && !ec;
             it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_directory(ec))
                continue;
            const std::string id = entry.path().filename().string();
            if (!safe_component(id))
                continue;
            if (fs::exists(entry.path() / DISABLE_FILE_NAME, ec) ||
                fs::exists(entry.path() / REMOVE_FILE_NAME, ec)) {
                modules.erase(id);
                skipped_modules[id] = true;
                continue;
            }
            if (skipped_modules.count(id) == 0 && modules.count(id) == 0)
                modules.emplace(id, entry.path());
        }
    }

    return {modules.begin(), modules.end()};
}

std::vector<NativeModule> scan_early_native_modules() {
    std::vector<NativeModule> out;

    for (const auto& active_module : collect_active_module_roots()) {
        const auto& module_id = active_module.first;
        const std::string base_path = active_module.second.string();
        const auto manifest = read_file(base_path + "/zn_modules.txt");
        if (!manifest)
            continue;

        for_each_line(*manifest, [&](std::string_view line) {
            NativeModule module{};
            if (!yukizygisk::native::parse_native_module_line(module_id, base_path,
                                                              std::string(line), &module))
                return true;
            if (module.has_companion || module.lib_path.size() >= YZ_NATIVE_MODULE_PATH_MAX)
                return true;
            const int module_class = elf_class(module.lib_path);
            if (module_class != ELFCLASS32 && module_class != ELFCLASS64)
                return true;
            out.push_back(std::move(module));
            return out.size() < YZ_NATIVE_TARGET_MAX;
        });
        if (out.size() >= YZ_NATIVE_TARGET_MAX)
            return out;
    }

    return out;
}

bool has_native_abi32_target() {
    for (const auto& active_module : collect_active_module_roots()) {
        const auto& module_id = active_module.first;
        const std::string base_path = active_module.second.string();
        const auto manifest = read_file(base_path + "/zn_modules.txt");
        if (!manifest)
            continue;

        bool found = false;
        for_each_line(*manifest, [&](std::string_view line) {
            NativeModule module{};
            if (yukizygisk::native::parse_native_module_line(module_id, base_path,
                                                             std::string(line), &module) &&
                elf_class(module.lib_path) == ELFCLASS32) {
                found = true;
                return false;
            }
            return true;
        });
        if (found)
            return true;
    }
    return false;
}

void fill_cstr(char* dst, size_t dst_size, const std::string& src) {
    if (dst_size == 0)
        return;
    const size_t copy_size = std::min(dst_size - 1, src.size());
    std::memcpy(dst, src.data(), copy_size);
    dst[copy_size] = '\0';
}

bool stage_module_entry(const NativeModule& m, size_t idx, const fs::path& module_dir,
                        yz_early_native_entry* entry) {
    char file_name[128];
    const int file_name_len =
        snprintf(file_name, sizeof(file_name), "%03zu-%s.so", idx, m.module_id.c_str());
    if (file_name_len < 0 || static_cast<size_t>(file_name_len) >= sizeof(file_name)) {
        LOGW("yukizygisk early: module filename too long for %s", m.module_id.c_str());
        return false;
    }
    fs::path const dst = module_dir / file_name;
    if (!copy_regular_file(m.lib_path, dst)) {
        LOGW("yukizygisk early: failed to copy native module %s from %s", m.module_id.c_str(),
             m.lib_path.c_str());
        return false;
    }
    chmod(dst.c_str(), 0644);
    (void)lsetfilecon(dst, kMetadataFileCon);

    *entry = {};
    entry->target_type = m.target_type;
    entry->flags = elf_class(m.lib_path) == ELFCLASS32 ? YZ_EARLY_NATIVE_ENTRY_ABI32
                                                       : YZ_EARLY_NATIVE_ENTRY_ABI64;
    fill_cstr(entry->module_id, sizeof(entry->module_id), m.module_id);
    fill_cstr(entry->target, sizeof(entry->target), m.target);
    fill_cstr(entry->lib_path, sizeof(entry->lib_path), dst.string());
    return true;
}

bool write_manifest(const fs::path& path, const yz_early_native_snapshot_header& header,
                    const std::vector<yz_early_native_entry>& entries) {
    std::string blob;
    blob.reserve(sizeof(header) + (entries.size() * sizeof(yz_early_native_entry)));
    blob.append(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& entry : entries)
        blob.append(reinterpret_cast<const char*>(&entry), sizeof(entry));
    return write_file(path, blob);
}

bool yukizygisk_enabled_for_next_boot() {
    if (is_safe_mode())
        return false;
    auto [value, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    return supported && value != 0;
}

bool early_load_enabled() {
    const auto contents = read_file(YUKIZYGISK_CONFIG_PATH);
    if (!contents)
        return false;
    const auto root = json::parse(*contents);
    return root.type == json::Type::Object && root.at("early_load").type == json::Type::Bool &&
           root.at("early_load").as_bool();
}

__u16 configured_load_flags() {
    yz_config config = yukizygisk::config::defaults;
    const auto contents = read_file(YUKIZYGISK_CONFIG_PATH);
    if (contents) {
        const auto root = json::parse(*contents);
        if (root.type == json::Type::Object) {
            if (root.at("yukilinker").type == json::Type::Bool)
                config.yukilinker = root.at("yukilinker").as_bool() ? 1 : 0;
            if (root.at("anonymous_memory").type == json::Type::Bool)
                config.memory_type =
                    root.at("anonymous_memory").as_bool() ? YZ_MEMORY_ANONYMOUS : YZ_MEMORY_FILE;
        }
    }
    return yukizygisk::config::load_flags(config);
}

}  // namespace

bool yukizygisk_early_load_enabled() {
    return early_load_enabled();
}

void clear_yukizygisk_early_snapshot() {
    remove_snapshot_dir(PREINIT_DIR_WATCHDOG);
    remove_snapshot_dir(PREINIT_DIR_DEFAULT);
}

bool yukizygisk_has_native_abi32_target() {
    return has_native_abi32_target();
}

int refresh_yukizygisk_early_snapshot() {
    if (!yukizygisk_enabled_for_next_boot() || !early_load_enabled()) {
        clear_yukizygisk_early_snapshot();
        LOGI("yukizygisk early: snapshot cleared (disabled)");
        return 0;
    }

    yz_runtime_query_cmd capabilities{};
    if (ksuctl(KSU_IOCTL_YZ_GET_RUNTIME, &capabilities) != 0 ||
        !(capabilities.capabilities & YZ_RUNTIME_CAP_NATIVE_LIBC_BOOTSTRAP_V1)) {
        LOGW("yukizygisk early: running LKM lacks native libc bootstrap v1; "
             "staged snapshot requires the matching LKM on next boot");
    }

    const fs::path preinit_dir = preinit_ksu_dir();
    const fs::path snapshot_dir = preinit_dir / kSnapshotDirName;
    const fs::path module_dir = snapshot_dir / kModulesDirName;
    const fs::path manifest_tmp = snapshot_dir / ".native_snapshot.tmp";
    const fs::path manifest = snapshot_dir / kManifestName;
    std::error_code ec;

    fs::create_directories(module_dir, ec);
    if (ec) {
        LOGW("yukizygisk early: failed to create %s: %s", module_dir.c_str(), ec.message().c_str());
        return 1;
    }

    fs::remove_all(module_dir, ec);
    fs::create_directories(module_dir, ec);
    if (ec) {
        LOGW("yukizygisk early: failed to reset %s: %s", module_dir.c_str(), ec.message().c_str());
        return 1;
    }

    const fs::path loader_path = snapshot_dir / kLoader64Name;
    const fs::path loader32_path = snapshot_dir / kLoader32Name;
    const fs::path native_core_path = snapshot_dir / kNativeCore64Name;
    const fs::path native_core32_path = snapshot_dir / kNativeCore32Name;
    if (!stage_asset_or_file(kLoader64Name, ZYLOADER64_PATH, loader_path) ||
        !stage_asset_or_file(kLoader32Name, ZYLOADER32_PATH, loader32_path) ||
        !stage_asset_or_file(kNativeCore64Name, ZNCORE64_PATH, native_core_path) ||
        !stage_asset_or_file(kNativeCore32Name, ZNCORE32_PATH, native_core32_path)) {
        clear_yukizygisk_early_snapshot();
        LOGW("yukizygisk early: payload unavailable, snapshot cleared");
        return 1;
    }

    for (const char* legacy_name :
         {"libyukilinker.so", "libyukilinker32.so", "libyukilinker64.so", "libyukizncore.so"}) {
        const fs::path legacy_path = snapshot_dir / legacy_name;
        if (!fs::remove(legacy_path, ec) && ec) {
            LOGW("yukizygisk early: failed to remove legacy payload %s: %s", legacy_path.c_str(),
                 ec.message().c_str());
        }
        ec.clear();
    }

    std::vector<yz_early_native_entry> entries;
    std::vector<NativeModule> modules = scan_early_native_modules();
    entries.reserve(modules.size());
    for (size_t i = 0; i < modules.size(); i++) {
        yz_early_native_entry entry{};
        if (stage_module_entry(modules[i], i, module_dir, &entry))
            entries.push_back(entry);
    }

    static const char* const kDlopen[] = {
        "__loader_android_dlopen_ext",
        "android_dlopen_ext",
    };
    static const char* const kDlsym[] = {
        "__loader_dlsym",
        "dlsym",
    };

    bool has_abi32 = false;
    bool has_abi64 = false;
    for (const auto& entry : entries) {
        has_abi32 = has_abi32 || (entry.flags & YZ_EARLY_NATIVE_ENTRY_ABI32) != 0;
        has_abi64 = has_abi64 || (entry.flags & YZ_EARLY_NATIVE_ENTRY_ABI64) != 0;
    }

    struct stat linker_stat{};
    struct stat linker32_stat{};
    bool const linker_stat_ok = !has_abi64 || stat(kSystemLinker64, &linker_stat) == 0;
    bool const linker32_stat_ok = !has_abi32 || stat(kSystemLinker32, &linker32_stat) == 0;

    yz_early_native_snapshot_header header{};
    header.magic = YZ_EARLY_NATIVE_MAGIC;
    header.version = YZ_EARLY_NATIVE_VERSION;
    header.header_size = sizeof(header);
    header.entry_size = sizeof(yz_early_native_entry);
    header.load_flags = configured_load_flags();
    header.flags = YZ_EARLY_NATIVE_FLAG_ENABLED;
    if (has_abi32)
        header.flags |= YZ_EARLY_NATIVE_FLAG_ABI32;
    if (has_abi64)
        header.flags |= YZ_EARLY_NATIVE_FLAG_ABI64;
    header.count = static_cast<uint32_t>(entries.size());
    header.dlopen_offset = has_abi64 ? resolve_first(kSystemLinker64, false, kDlopen, 2) : 0;
    header.dlsym_offset = has_abi64 ? resolve_first(kSystemLinker64, false, kDlsym, 2) : 0;
    header.linker_size = has_abi64 ? static_cast<uint64_t>(linker_stat.st_size) : 0;
    header.dlopen32_offset = has_abi32 ? resolve_first(kSystemLinker32, true, kDlopen, 2) : 0;
    header.dlsym32_offset = has_abi32 ? resolve_first(kSystemLinker32, true, kDlsym, 2) : 0;
    header.linker32_size = has_abi32 ? static_cast<uint64_t>(linker32_stat.st_size) : 0;

    if (!entries.empty() &&
        (!linker_stat_ok || !linker32_stat_ok ||
         (has_abi64 &&
          (header.dlopen_offset == 0 || header.dlsym_offset == 0 || header.linker_size == 0)) ||
         (has_abi32 && (header.dlopen32_offset == 0 || header.dlsym32_offset == 0 ||
                        header.linker32_size == 0)))) {
        clear_yukizygisk_early_snapshot();
        LOGW("yukizygisk early: linker offsets unavailable, snapshot cleared");
        return 1;
    }

    if (!write_manifest(manifest_tmp, header, entries)) {
        fs::remove(manifest_tmp, ec);
        LOGW("yukizygisk early: failed to write %s", manifest_tmp.c_str());
        return 1;
    }

    chmod(manifest_tmp.c_str(), 0644);
    (void)lsetfilecon(manifest_tmp, kMetadataFileCon);
    fs::rename(manifest_tmp, manifest, ec);
    if (ec) {
        fs::remove(manifest_tmp, ec);
        LOGW("yukizygisk early: failed to rename snapshot: %s", ec.message().c_str());
        return 1;
    }
    record_yukizygisk_early_linker(header);

    const fs::path stale_dir =
        (preinit_dir == PREINIT_DIR_WATCHDOG) ? PREINIT_DIR_DEFAULT : PREINIT_DIR_WATCHDOG;
    remove_snapshot_dir(stale_dir);

    LOGI("yukizygisk early: snapshot refreshed modules=%zu", entries.size());
    return 0;
}

}  // namespace ksud
