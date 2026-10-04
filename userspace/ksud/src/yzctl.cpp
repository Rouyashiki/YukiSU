#include "yzctl.hpp"
#include "userspace/zygisk/crash_protection.hpp"
#include "userspace/zygisk/settings.hpp"

#include "core/json.hpp"
#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "userspace/zygisk/crash_evidence.hpp"
#include "userspace/zygisk/daemon/native_modules.hpp"
#include "userspace/zygisk/native_exit_evidence.hpp"
#include "utils.hpp"
#include "yukizygisk_snapshot.hpp"

#include "uapi/yukizygisk.h"

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ksud {

namespace {

using yukizygisk::native::NativeModule;

constexpr const char* kModulesDir = "/data/adb/modules";
constexpr const char* kAbi32 = "armeabi-v7a";
constexpr const char* kAbi64 = "arm64-v8a";

struct RuntimeSnapshot {
    uint32_t generation = 0;
    bool enabled = false;
    bool safe_mode = false;
    uint32_t zygote_crashes = 0;
    uint32_t capabilities = 0;
    std::string safe_mode_zygote;
    std::vector<yz_runtime_record> records;
};

struct NativeDefinition {
    NativeModule module;
    uint8_t abi = YZ_RUNTIME_ABI_UNKNOWN;
};

struct ModuleInventory {
    std::vector<std::string> zygisk_modules;
    std::vector<NativeDefinition> native_modules;
};

struct NativeInjection {
    uint32_t pid = 0;
    uint32_t generation = 0;
    std::string process;
    std::string module_id;
    uint8_t target_type = 0;
    std::string target;
    uint8_t abi = YZ_RUNTIME_ABI_UNKNOWN;
    bool has_companion = false;
    std::string state;
    std::string process_state;
};

struct NativeModuleView {
    std::string module_id;
    uint8_t target_type = 0;
    std::string target;
    bool has_companion = false;
    std::string state;
};

template <size_t Size>
std::string bounded_string(const char (&value)[Size]) {
    return std::string(value, strnlen(value, Size));
}

const char* abi_name(uint8_t abi) {
    switch (abi) {
    case YZ_RUNTIME_ABI_32:
        return kAbi32;
    case YZ_RUNTIME_ABI_64:
        return kAbi64;
    default:
        return "unknown";
    }
}

const char* kind_name(uint8_t kind) {
    switch (kind) {
    case YZ_RUNTIME_KIND_ZYGOTE:
        return "zygote";
    case YZ_RUNTIME_KIND_NATIVE:
        return "native";
    default:
        return "unknown";
    }
}

const char* monitor_state_name(uint8_t state) {
    switch (state) {
    case YZ_RUNTIME_STATE_INJECTED:
        return "injected";
    case YZ_RUNTIME_STATE_SAFEMODE:
        return "crashed";
    case YZ_RUNTIME_STATE_DETECTED:
    case YZ_RUNTIME_STATE_REDIRECTED:
        return "unknown";
    case YZ_RUNTIME_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char* injection_state_name(const RuntimeSnapshot& snapshot, const yz_runtime_record& record) {
    uint8_t state = record.state;
    if (state == YZ_RUNTIME_STATE_EXITED) {
        if (!(snapshot.capabilities & YZ_RUNTIME_CAP_INJECTION_STATE))
            return "unknown";
        state = static_cast<uint8_t>((record.flags & YZ_RUNTIME_F_INJECTION_STATE_MASK) >>
                                     YZ_RUNTIME_F_INJECTION_STATE_SHIFT);
    }
    return monitor_state_name(state);
}

const char* process_state_name(uint8_t state) {
    switch (state) {
    case YZ_RUNTIME_STATE_EXITED:
        return "exited";
    case YZ_RUNTIME_STATE_DETECTED:
    case YZ_RUNTIME_STATE_REDIRECTED:
    case YZ_RUNTIME_STATE_INJECTED:
    case YZ_RUNTIME_STATE_FAILED:
    case YZ_RUNTIME_STATE_SAFEMODE:
        return "running";
    default:
        return "unknown";
    }
}

const char* runtime_state_name(uint8_t state) {
    switch (state) {
    case YZ_RUNTIME_STATE_DETECTED:
        return "detected";
    case YZ_RUNTIME_STATE_REDIRECTED:
        return "redirected";
    case YZ_RUNTIME_STATE_INJECTED:
        return "injected";
    case YZ_RUNTIME_STATE_FAILED:
        return "failed";
    case YZ_RUNTIME_STATE_SAFEMODE:
        return "safemode";
    case YZ_RUNTIME_STATE_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

const char* target_type_name(uint8_t type) {
    switch (type) {
    case YZ_NATIVE_TARGET_NAME:
        return "name";
    case YZ_NATIVE_TARGET_PATH:
        return "path";
    default:
        return "unknown";
    }
}

bool query_runtime(RuntimeSnapshot* snapshot) {
    if (snapshot == nullptr)
        return false;

    RuntimeSnapshot result;
    result.records.resize(YZ_RUNTIME_RECORD_MAX);

    yz_runtime_query_cmd command{};
    command.capacity = static_cast<uint32_t>(result.records.size());
    command.entries =
        static_cast<__aligned_u64>(reinterpret_cast<uintptr_t>(result.records.data()));
    if (ksuctl(KSU_IOCTL_YZ_GET_RUNTIME, &command) != 0)
        return false;

    if (command.count > result.records.size())
        return false;
    result.records.resize(command.count);
    result.generation = command.generation;
    result.enabled = yz_feature_enabled();
    result.safe_mode = command.safe_mode != 0;
    result.zygote_crashes = command.zygote_crashes;
    result.capabilities = command.capabilities;
    result.safe_mode_zygote = bounded_string(command.safe_mode_zygote);
    *snapshot = std::move(result);
    return true;
}

uint8_t elf_abi(const std::string& path) {
    std::array<unsigned char, EI_NIDENT> ident{};
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return YZ_RUNTIME_ABI_UNKNOWN;
    const ssize_t count = read(fd, ident.data(), ident.size());
    close(fd);
    if (count != static_cast<ssize_t>(ident.size()) || memcmp(ident.data(), ELFMAG, SELFMAG) != 0 ||
        ident[EI_DATA] != ELFDATA2LSB)
        return YZ_RUNTIME_ABI_UNKNOWN;
    if (ident[EI_CLASS] == ELFCLASS32)
        return YZ_RUNTIME_ABI_32;
    if (ident[EI_CLASS] == ELFCLASS64)
        return YZ_RUNTIME_ABI_64;
    return YZ_RUNTIME_ABI_UNKNOWN;
}

std::vector<std::string> active_module_ids() {
    std::vector<std::string> ids;
    DIR* directory = opendir(kModulesDir);
    if (directory == nullptr)
        return ids;

    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string id = entry->d_name;
        const std::string base = std::string(kModulesDir) + "/" + id;
        if (access((base + "/disable").c_str(), F_OK) == 0 ||
            access((base + "/remove").c_str(), F_OK) == 0)
            continue;
        ids.push_back(id);
    }
    closedir(directory);
    std::sort(ids.begin(), ids.end());
    return ids;
}

ModuleInventory scan_modules() {
    ModuleInventory inventory;
    std::set<std::tuple<std::string, uint8_t, std::string, uint8_t>> native_keys;

    for (const std::string& module_id : active_module_ids()) {
        const std::string base = std::string(kModulesDir) + "/" + module_id;
        if (yz_is_zygisk_module(base)) {
            inventory.zygisk_modules.push_back(module_id);
        }

        const auto manifest = read_file(base + "/zn_modules.txt");
        if (!manifest)
            continue;

        for_each_line(*manifest, [&](std::string_view raw_line) {
            NativeModule module;
            if (!yukizygisk::native::parse_native_module_line(module_id, base,
                                                              std::string(raw_line), &module)) {
                return;
            }
            const uint8_t abi = elf_abi(module.lib_path);
            if (abi == YZ_RUNTIME_ABI_UNKNOWN)
                return;
            const auto key =
                std::make_tuple(module.module_id, module.target_type, module.target, abi);
            if (native_keys.insert(key).second) {
                inventory.native_modules.push_back(NativeDefinition{std::move(module), abi});
            } else {
                const auto existing =
                    std::find_if(inventory.native_modules.begin(), inventory.native_modules.end(),
                                 [&](const NativeDefinition& definition) {
                                     return definition.abi == abi &&
                                            definition.module.module_id == module.module_id &&
                                            definition.module.target_type == module.target_type &&
                                            definition.module.target == module.target;
                                 });
                if (existing != inventory.native_modules.end()) {
                    existing->module.has_companion =
                        existing->module.has_companion || module.has_companion;
                }
            }
        });
    }
    std::sort(inventory.native_modules.begin(), inventory.native_modules.end(),
              [](const NativeDefinition& left, const NativeDefinition& right) {
                  return std::tie(left.module.module_id, left.module.target_type,
                                  left.module.target, left.abi) <
                         std::tie(right.module.module_id, right.module.target_type,
                                  right.module.target, right.abi);
              });
    return inventory;
}

const yz_runtime_record* find_native_report(const RuntimeSnapshot& snapshot,
                                            const yz_runtime_record& base,
                                            const std::string& module_id) {
    for (const yz_runtime_record& record : snapshot.records) {
        if (record.kind == YZ_RUNTIME_KIND_NATIVE && record.pid == base.pid &&
            record.generation == base.generation && record.abi == base.abi &&
            record.target_type == base.target_type &&
            bounded_string(record.target) == bounded_string(base.target) &&
            bounded_string(record.module_id) == module_id) {
            return &record;
        }
    }
    return nullptr;
}

std::vector<NativeInjection> build_native_injections(const RuntimeSnapshot& snapshot,
                                                     const ModuleInventory& inventory) {
    std::vector<NativeInjection> injections;
    using Scope = std::tuple<uint8_t, std::string, uint8_t>;
    struct Instances {
        std::vector<const yz_runtime_record*> active;
        const yz_runtime_record* latest_exited = nullptr;
    };
    std::map<Scope, Instances> scopes;

    for (const yz_runtime_record& base : snapshot.records) {
        if (base.kind != YZ_RUNTIME_KIND_NATIVE || base.module_id[0] != '\0')
            continue;
        Instances& instances = scopes[{base.target_type, bounded_string(base.target), base.abi}];
        if (base.state != YZ_RUNTIME_STATE_EXITED) {
            instances.active.push_back(&base);
        } else if (instances.latest_exited == nullptr ||
                   base.generation > instances.latest_exited->generation) {
            instances.latest_exited = &base;
        }
    }

    for (auto& [scope, instances] : scopes) {
        if (instances.active.empty() && instances.latest_exited != nullptr)
            instances.active.push_back(instances.latest_exited);
        const auto& [target_type, target, abi] = scope;
        for (const NativeDefinition& definition : inventory.native_modules) {
            const NativeModule& module = definition.module;
            if (definition.abi != abi || module.target_type != target_type ||
                module.target != target) {
                continue;
            }
            for (const yz_runtime_record* base : instances.active) {
                const yz_runtime_record* report =
                    find_native_report(snapshot, *base, module.module_id);
                const char* state = "unknown";
                if (report != nullptr) {
                    state = injection_state_name(snapshot, *report);
                } else {
                    const char* base_state = injection_state_name(snapshot, *base);
                    if (strcmp(base_state, "failed") == 0 || strcmp(base_state, "crashed") == 0)
                        state = base_state;
                }
                injections.push_back(NativeInjection{
                    base->pid,
                    base->generation,
                    bounded_string(base->process),
                    module.module_id,
                    target_type,
                    target,
                    abi,
                    module.has_companion,
                    state,
                    process_state_name(base->state),
                });
            }
        }
    }
    std::sort(
        injections.begin(), injections.end(),
        [](const NativeInjection& left, const NativeInjection& right) {
            return std::tie(left.pid, left.module_id, left.target_type, left.target, left.abi) <
                   std::tie(right.pid, right.module_id, right.target_type, right.target, right.abi);
        });
    return injections;
}

std::string native_module_state(const std::string& module_id, uint8_t target_type,
                                const std::string& target,
                                const std::vector<NativeInjection>& injections) {
    bool injected = false;
    bool failed = false;
    for (const NativeInjection& injection : injections) {
        if (injection.module_id != module_id || injection.target_type != target_type ||
            injection.target != target)
            continue;
        if (injection.state == "crashed")
            return "crashed";
        if (injection.state == "failed")
            failed = true;
        if (injection.state == "injected")
            injected = true;
    }
    if (failed)
        return "failed";
    if (injected)
        return "injected";

    return "unknown";
}

std::vector<NativeModuleView> build_native_module_views(
    const ModuleInventory& inventory, const std::vector<NativeInjection>& injections) {
    std::vector<NativeModuleView> views;
    for (const NativeDefinition& definition : inventory.native_modules) {
        const NativeModule& module = definition.module;
        const auto existing =
            std::find_if(views.begin(), views.end(), [&](const NativeModuleView& view) {
                return view.module_id == module.module_id &&
                       view.target_type == module.target_type && view.target == module.target;
            });
        if (existing == views.end()) {
            views.push_back(NativeModuleView{
                module.module_id,
                module.target_type,
                module.target,
                module.has_companion,
                native_module_state(module.module_id, module.target_type, module.target,
                                    injections),
            });
        } else {
            existing->has_companion = existing->has_companion || module.has_companion;
        }
    }
    return views;
}

size_t injected_target_count(const RuntimeSnapshot& snapshot) {
    std::set<std::tuple<uint32_t, uint32_t, uint8_t, uint8_t>> targets;
    for (const yz_runtime_record& record : snapshot.records) {
        if (record.module_id[0] == '\0' && record.state == YZ_RUNTIME_STATE_INJECTED &&
            (record.kind == YZ_RUNTIME_KIND_ZYGOTE || record.kind == YZ_RUNTIME_KIND_NATIVE)) {
            targets.emplace(record.pid, record.generation, record.kind, record.abi);
        }
    }
    return targets.size();
}

json::Value number(uint32_t value) {
    return {static_cast<double>(value)};
}

json::Value build_status_json(const RuntimeSnapshot& snapshot, const ModuleInventory& inventory) {
    const std::vector<NativeInjection> injections = build_native_injections(snapshot, inventory);
    const std::vector<NativeModuleView> native_modules =
        build_native_module_views(inventory, injections);
    json::Value root = json::Value::object();
    root["generation"] = number(snapshot.generation);
    root["enabled"] = json::Value(snapshot.enabled);
    root["count"] = json::Value(static_cast<double>(injected_target_count(snapshot)));
    root["safe_mode"] = json::Value(snapshot.safe_mode);
    root["zygisk_module_monitor"] =
        json::Value((snapshot.capabilities & YZ_RUNTIME_CAP_ZYGOTE_MODULE_REPORT) != 0);
    root["zygote_crashes"] = number(snapshot.zygote_crashes);
    root["safe_mode_zygote"] = json::Value(
        snapshot.safe_mode_zygote.empty() ? std::string("zygote") : snapshot.safe_mode_zygote);
    root["recent"] = json::Value::array();
    root["runtime"] = json::Value::array();
    root["zygotes"] = json::Value::array();
    root["zygote_monitor"] = json::Value::array();
    root["modules"] = json::Value::array();
    root["native_modules"] = json::Value::array();
    root["native_injections"] = json::Value::array();
    root["crash_evidence"] = yukizygisk::crash::read_evidence(YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR);
    root["exit_history_supported"] =
        json::Value((snapshot.capabilities & YZ_RUNTIME_CAP_EXIT_HISTORY) != 0);
    root["native_exit_evidence"] = json::Value::array();
    root["native_exit_journal"] = json::Value::array();
    for (const uint8_t abi : {uint8_t{1}, uint8_t{2}}) {
        const auto document =
            yukizygisk::crash::read_native_exit_document(YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR, abi);
        for (const auto& item : document.exits.a)
            root["native_exit_evidence"].push_back(item);
        if (document.journal_valid) {
            auto journal = yukizygisk::crash::native_exit_journal_json(document.journal);
            journal["abi_id"] = number(abi);
            journal["abi"] = json::Value(abi_name(abi));
            root["native_exit_journal"].push_back(journal);
        }
    }
    const bool protection = yukizygisk::settings::protection_enabled(YUKIZYGISK_CONFIG_PATH);
    root["crash_protection"] = protection;
    root["suspended_modules"] =
        protection ? yukizygisk::crash::suspended_modules(YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR)
                   : json::Value::array();

    for (const yz_runtime_record& record : snapshot.records) {
        json::Value raw = json::Value::object();
        raw["pid"] = number(record.pid);
        raw["generation"] = number(record.generation);
        raw["restarts"] = number(record.restarts);
        raw["kind"] = json::Value(kind_name(record.kind));
        raw["kind_id"] = number(record.kind);
        raw["state"] = json::Value(runtime_state_name(record.state));
        raw["state_id"] = number(record.state);
        raw["injection_state"] = json::Value(injection_state_name(snapshot, record));
        raw["process_state"] = json::Value(process_state_name(record.state));
        raw["abi"] = json::Value(abi_name(record.abi));
        raw["abi_id"] = number(record.abi);
        raw["target_type"] = json::Value(target_type_name(record.target_type));
        raw["target_type_id"] = number(record.target_type);
        raw["flags"] = number(record.flags);
        raw["process"] = json::Value(bounded_string(record.process));
        raw["target"] = json::Value(bounded_string(record.target));
        raw["module"] = json::Value(bounded_string(record.module_id));
        root["runtime"].push_back(raw);

        if (record.kind != YZ_RUNTIME_KIND_ZYGOTE || record.module_id[0] != '\0' ||
            record.state == YZ_RUNTIME_STATE_EXITED)
            continue;
        const char* state = injection_state_name(snapshot, record);

        const std::string target = bounded_string(record.target);
        json::Value entry = json::Value::object();
        entry["pid"] = number(record.pid);
        entry["generation"] = number(record.generation);
        entry["name"] = json::Value(target);
        entry["target"] = json::Value(target);
        entry["abi"] = json::Value(abi_name(record.abi));
        entry["state"] = json::Value(state);
        entry["injection_state"] = json::Value(state);
        entry["process_state"] = json::Value(process_state_name(record.state));
        root["zygote_monitor"].push_back(entry);
        if (record.state == YZ_RUNTIME_STATE_INJECTED)
            root["zygotes"].push_back(entry);
    }

    for (const std::string& module_id : inventory.zygisk_modules)
        root["modules"].push_back(json::Value(module_id));

    for (const NativeModuleView& module : native_modules) {
        json::Value entry = json::Value::object();
        entry["id"] = json::Value(module.module_id);
        entry["target_type"] = json::Value(target_type_name(module.target_type));
        entry["target"] = json::Value(module.target);
        entry["companion"] = json::Value(module.has_companion);
        entry["state"] = json::Value(module.state);
        root["native_modules"].push_back(entry);
    }

    for (const NativeInjection& injection : injections) {
        json::Value entry = json::Value::object();
        entry["pid"] = number(injection.pid);
        entry["generation"] = number(injection.generation);
        entry["process"] = json::Value(injection.process);
        entry["module"] = json::Value(injection.module_id);
        entry["target_type"] = json::Value(target_type_name(injection.target_type));
        entry["target"] = json::Value(injection.target);
        entry["abi"] = json::Value(abi_name(injection.abi));
        entry["companion"] = json::Value(injection.has_companion);
        entry["state"] = json::Value(injection.state);
        entry["injection_state"] = json::Value(injection.state);
        entry["process_state"] = json::Value(injection.process_state);
        root["native_injections"].push_back(entry);
    }
    return root;
}

void print_usage(FILE* stream) {
    (void)fprintf(stream, "Usage: yzctl <command>\n\n");
    (void)fprintf(stream, "Commands:\n");
    (void)fprintf(stream, "  status [--json]    Read the kernel runtime snapshot\n");
    (void)fprintf(stream, "  reload             Notify daemons to reload configuration\n");
    (void)fprintf(stream, "  config get         Read configuration as JSON\n");
    (void)fprintf(stream,
                  "  config set KEY VALUE [KEY VALUE ...]  Save and reload configuration\n");
    (void)fprintf(stream, "  refresh-snapshot   Rebuild the early native snapshot\n");
}

void print_human_status(const RuntimeSnapshot& snapshot) {
    printf("Generation: %u\n", snapshot.generation);
    printf("Enabled: %s\n", snapshot.enabled ? "yes" : "no");
    printf("Safe mode: %s\n", snapshot.safe_mode ? "yes" : "no");
    printf("Zygote crashes: %u\n", snapshot.zygote_crashes);
    printf("Injected targets: %zu\n", injected_target_count(snapshot));
    printf("PID\tGEN\tABI\tKIND\tSTATE\tINJECTION\tLIFETIME\tPROCESS\tTARGET\tMODULE\n");
    for (const yz_runtime_record& record : snapshot.records) {
        printf("%u\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", record.pid, record.generation,
               abi_name(record.abi), kind_name(record.kind), runtime_state_name(record.state),
               injection_state_name(snapshot, record), process_state_name(record.state),
               bounded_string(record.process).c_str(), bounded_string(record.target).c_str(),
               bounded_string(record.module_id).c_str());
    }
}

}  // namespace

bool yz_feature_enabled() {
    const auto [value, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    return supported && value != 0;
}

bool yz_is_zygisk_module(const std::string& module_path) {
    return access((module_path + "/zygisk/" + kAbi64 + ".so").c_str(), F_OK) == 0 ||
           access((module_path + "/zygisk/" + kAbi32 + ".so").c_str(), F_OK) == 0;
}

bool yz_has_native_modules(const std::string& module_path) {
    return access((module_path + "/zn_modules.txt").c_str(), F_OK) == 0;
}

std::set<std::string> yz_loaded_module_ids() {
    RuntimeSnapshot snapshot;
    if (!query_runtime(&snapshot))
        return {};

    const ModuleInventory inventory = scan_modules();
    const std::vector<NativeInjection> injections = build_native_injections(snapshot, inventory);

    std::set<std::string> loaded(inventory.zygisk_modules.begin(), inventory.zygisk_modules.end());
    for (const NativeInjection& injection : injections) {
        if (injection.state == "injected")
            loaded.insert(injection.module_id);
    }
    for (const NativeModuleView& view : build_native_module_views(inventory, injections)) {
        if (view.state == "injected")
            loaded.insert(view.module_id);
    }
    return loaded;
}

int yzctl_run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
        print_usage(stdout);
        return args.empty() ? 1 : 0;
    }

    if (args[0] == "config") {
        if (args.size() == 2 && args[1] == "get") {
            json::Value config;
            if (!yukizygisk::settings::read(YUKIZYGISK_CONFIG_PATH, &config)) {
                (void)fprintf(stderr, "yzctl: configuration read failed: %s\n", strerror(errno));
                return 1;
            }
            printf("%s\n", json::dump(config).c_str());
            return 0;
        }
        if (args.size() < 4 || args[1] != "set" || args.size() % 2 != 0) {
            (void)fprintf(stderr,
                          "yzctl: expected config get or config set KEY VALUE [KEY VALUE ...]\n");
            return 1;
        }
        if (!yukizygisk::settings::update(YUKIZYGISK_CONFIG_PATH, {args.begin() + 2, args.end()})) {
            (void)fprintf(stderr, "yzctl: configuration save failed: %s\n", strerror(errno));
            return 1;
        }
        return yzctl_run({"reload"});
    }

    if (args[0] == "status") {
        bool json_output = false;
        for (size_t index = 1; index < args.size(); ++index) {
            if (args[index] == "--json") {
                json_output = true;
            } else {
                (void)fprintf(stderr, "yzctl: unknown status option: %s\n", args[index].c_str());
                return 1;
            }
        }

        RuntimeSnapshot snapshot;
        if (!query_runtime(&snapshot)) {
            (void)fprintf(stderr, "yzctl: failed to read YukiZygisk runtime state: %s\n",
                          strerror(errno));
            return 1;
        }
        if (json_output) {
            const ModuleInventory inventory = scan_modules();
            printf("%s\n", json::dump(build_status_json(snapshot, inventory)).c_str());
        } else {
            print_human_status(snapshot);
        }
        return 0;
    }

    if (args[0] == "reload") {
        if (args.size() != 1) {
            (void)fprintf(stderr, "yzctl: reload takes no arguments\n");
            return 1;
        }
        if (ksuctl(KSU_IOCTL_YZ_RELOAD, nullptr) != 0) {
            (void)fprintf(stderr, "yzctl: reload failed: %s\n", strerror(errno));
            return 1;
        }
        if (refresh_yukizygisk_early_snapshot() != 0) {
            (void)fprintf(stderr, "yzctl: early snapshot refresh failed\n");
            return 1;
        }
        printf("YukiZygisk reload signalled\n");
        return 0;
    }

    if (args[0] == "refresh-snapshot") {
        if (args.size() != 1) {
            (void)fprintf(stderr, "yzctl: refresh-snapshot takes no arguments\n");
            return 1;
        }
        if (refresh_yukizygisk_early_snapshot() != 0) {
            (void)fprintf(stderr, "yzctl: early snapshot refresh failed\n");
            return 1;
        }
        printf("YukiZygisk early snapshot refreshed\n");
        return 0;
    }

    (void)fprintf(stderr, "yzctl: unknown command: %s\n", args[0].c_str());
    print_usage(stderr);
    return 1;
}

int yzctl_main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index)
        args.emplace_back(argv[index]);
    return yzctl_run(args);
}

}  // namespace ksud
