#include "module.hpp"
#include "../assets.hpp"
#include "../core/ksucalls.hpp"
#include "../core/restorecon.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../sepolicy/sepolicy.hpp"
#include "../utils.hpp"
#include "../yukizygisk_snapshot.hpp"
#include "../yzctl.hpp"
#include "metamodule.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...

namespace ksud {

struct ModuleInfo {
    std::string id;
    std::string dir_id;
    std::string name;
    std::string version;
    std::string version_code;
    std::string author;
    std::string description;
    bool enabled{};
    bool update{};
    bool remove{};
    bool web{};
    bool action{};
    bool mount{};
    bool metamodule{};
    std::string actionIcon;
    std::string webuiIcon;
    // How YukiZygisk would load this module: "zygisk", "native" or empty.
    std::string runtime;
    bool runtime_loaded{};
    bool yz_conflict{};
    // Remaining module.prop entries, forwarded verbatim (updateJson, support, ...).
    std::map<std::string, std::string> extra_props;
};

namespace {

constexpr const char* INSTALLER_SCRIPT_NAME = "installer.sh";
constexpr const char* METADATA_FILE_CON = "u:object_r:metadata_file:s0";

// Escape special characters for JSON string
std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buf{};
                const int snp =
                    snprintf(buf.data(), buf.size(), "\\u%04x", static_cast<unsigned char>(c));
                if (snp > 0 && static_cast<size_t>(snp) < buf.size()) {
                    result += buf.data();
                }
            } else {
                result += c;
            }
        }
    }
    return result;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::filesystem::path preinit_ksu_dir() {
    std::error_code error;
    if (std::filesystem::is_directory("/metadata/watchdog", error) && !error) {
        return PREINIT_DIR_WATCHDOG;
    }
    return PREINIT_DIR_DEFAULT;
}

bool has_rc_extension(const std::filesystem::path& path) {
    return path.extension() == ".rc";
}

bool write_all_fd(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool copy_path_to_fd(const std::filesystem::path& path, int out_fd, bool* source_opened) {
    *source_opened = false;
    const int in_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (in_fd < 0)
        return false;
    *source_opened = true;
    std::array<char, 65536> buffer{};
    bool ok = true;
    for (;;) {
        const ssize_t count = read(in_fd, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (!write_all_fd(out_fd, std::string_view(buffer.data(), static_cast<size_t>(count)))) {
            ok = false;
            break;
        }
    }
    close(in_fd);
    return ok;
}

bool collect_rc_files(const std::filesystem::path& dir, const std::string* module_id, int out_fd) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return true;

    std::vector<std::filesystem::directory_entry> entries;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         it != std::filesystem::directory_iterator() && !ec; it.increment(ec)) {
        entries.push_back(*it);
    }
    if (ec)
        return false;

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.path().filename().string() < rhs.path().filename().string();
    });

    for (const auto& entry : entries) {
        const std::filesystem::path path = entry.path();
        if (!std::filesystem::is_regular_file(path, ec) || !has_rc_extension(path))
            continue;
        if (module_id == nullptr && access(path.c_str(), X_OK) != 0)
            continue;

        std::string header = "# === from ";
        if (module_id != nullptr) {
            header += *module_id;
            header += ':';
        }
        header += path.string();
        header += " ===\n";
        if (!write_all_fd(out_fd, header))
            return false;
        bool source_opened = false;
        if (!copy_path_to_fd(path, out_fd, &source_opened)) {
            if (!source_opened) {
                LOGW("Failed to read init rc: %s", path.c_str());
                continue;
            }
            return false;
        }
        if (!write_all_fd(out_fd, "\n"))
            return false;
    }
    return true;
}

void warn_regenerate_preinit_rc_failed(int ret) {
    if (ret != 0) {
        LOGW("regenerate preinit rc failed: %d", ret);
    }
}

void warn_refresh_yukizygisk_early_snapshot_failed() {
    if (!yukizygisk_early_load_enabled()) {
        clear_yukizygisk_early_snapshot();
        return;
    }
    const int ret = refresh_yukizygisk_early_snapshot();
    if (ret != 0) {
        LOGW("refresh YukiZygisk early snapshot failed: %d", ret);
    }
}

// Resolve module icon path with security checks
std::string resolve_module_icon_path(
    const std::string& icon_value,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::string& module_id, const std::filesystem::path& module_path,
    const std::string& key_name) {
    if (icon_value.empty()) {
        return "";
    }

    // Reject absolute paths
    if (icon_value[0] == '/') {
        LOGW("Module %s: %s contains absolute path, rejected\n", module_id.c_str(),
             key_name.c_str());
        return "";
    }

    // Reject parent directory traversal
    if (icon_value.find("..") != std::string::npos) {
        LOGW("Module %s: %s contains parent directory traversal, rejected\n", module_id.c_str(),
             key_name.c_str());
        return "";
    }

    // Construct full path and verify it exists
    const std::string full_path = (module_path / icon_value).string();
    if (!file_exists(full_path)) {
        LOGW("Module %s: %s file does not exist: %s\n", module_id.c_str(), key_name.c_str(),
             full_path.c_str());
        return "";
    }

    // Return the relative path (icon_value) as it will be accessed via su://
    return icon_value;
}

std::map<std::string, std::string> parse_module_prop_content(std::string_view content) {
    std::map<std::string, std::string> props;
    for_each_line(content, [&props](std::string_view line) {
        const size_t eq = line.find('=');
        if (eq != std::string_view::npos) {
            props[std::string(trim_view(line.substr(0, eq)))] =
                std::string(trim_view(line.substr(eq + 1)));
        }
    });
    return props;
}

std::map<std::string, std::string> parse_module_prop(const std::string& path) {
    const auto content = read_file(path);
    return content ? parse_module_prop_content(*content) : std::map<std::string, std::string>{};
}

// Validate module ID like official ksud: ^[a-zA-Z][a-zA-Z0-9._-]+$
bool validate_module_id(const std::string& id) {
    if (id.size() < 2) {
        return false;
    }

    const auto is_valid_char = [](const char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '.' || c == '_' || c == '-';
    };

    if (std::isalpha(static_cast<unsigned char>(id.front())) == 0) {
        return false;
    }

    return std::all_of(id.begin(), id.end(), is_valid_char);
}

// Check if module is metamodule
bool is_metamodule(const std::map<std::string, std::string>& props) {
    auto it = props.find("metamodule");
    if (it == props.end())
        return false;
    const std::string val = it->second;
    return val == "1" || val == "true" || val == "TRUE";
}

std::string get_metamodule_path_impl() {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        std::array<char, PATH_MAX> target{};
        const ssize_t len = readlink(link_path.c_str(), target.data(), target.size() - 1);
        if (len > 0 && static_cast<size_t>(len) < target.size()) {
            target[static_cast<size_t>(len)] = '\0';
            std::filesystem::path resolved_path(target.data());
            if (resolved_path.is_relative()) {
                resolved_path = std::filesystem::path(link_path).parent_path() / resolved_path;
            }

            std::error_code ec;
            const std::filesystem::path normalized = resolved_path.lexically_normal();
            if (std::filesystem::is_directory(normalized, ec)) {
                return normalized.string();
            }
        }
    }

    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        return "";
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (entry->d_type != DT_DIR) {
            continue;
        }

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        const auto props = parse_module_prop(module_path + "/module.prop");
        if (is_metamodule(props)) {
            closedir(dir);
            return module_path;
        }
    }

    closedir(dir);
    return "";
}

// Get current metamodule ID if exists (internal impl)
std::string get_metamodule_id_impl() {
    const std::string metamodule_path = get_metamodule_path_impl();
    if (metamodule_path.empty()) {
        return "";
    }
    return std::filesystem::path(metamodule_path).filename().string();
}

// Check if it's safe to install module
// Returns: 0 = safe, 1 = disabled metamodule, 2 = pending changes
int check_install_safety(bool installing_metamodule) {
    if (installing_metamodule)
        return 0;

    const std::string metamodule_path = get_metamodule_path_impl();
    if (metamodule_path.empty()) {
        return 0;
    }

    const std::string metamodule_id = std::filesystem::path(metamodule_path).filename().string();
    const bool has_metainstall =
        file_exists(metamodule_path + "/" + METAMODULE_METAINSTALL_SCRIPT) ||
        file_exists(std::string(MODULE_UPDATE_DIR) + metamodule_id + "/" +
                    METAMODULE_METAINSTALL_SCRIPT);
    if (!has_metainstall) {
        return 0;
    }

    // Check for marker files
    const bool has_update = file_exists(metamodule_path + "/" + UPDATE_FILE_NAME);
    const bool has_remove = file_exists(metamodule_path + "/" + REMOVE_FILE_NAME);
    const bool has_disable = file_exists(metamodule_path + "/" + DISABLE_FILE_NAME);

    // Stable state - safe to install
    if (!has_update && !has_remove && !has_disable)
        return 0;

    // Return appropriate error code
    if (has_disable && !has_update && !has_remove)
        return 1;  // disabled
    return 2;      // pending changes
}

// Create metamodule symlink
bool create_metamodule_symlink(const std::string& module_id) {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);
    const std::string target_path = std::string(MODULE_DIR) + module_id;

    // Remove existing symlink/directory
    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            unlink(link_path.c_str());
        } else if (S_ISDIR(st.st_mode)) {
            std::error_code remove_error;
            std::filesystem::remove_all(link_path, remove_error);
            if (remove_error) {
                LOGW("Failed to remove %s: %s", link_path.c_str(), remove_error.message().c_str());
            }
        }
    }

    // Create symlink
    if (symlink(target_path.c_str(), link_path.c_str()) != 0) {
        LOGE("Failed to create metamodule symlink: %s", strerror(errno));
        return false;
    }

    LOGI("Created metamodule symlink: %s -> %s", link_path.c_str(), target_path.c_str());
    return true;
}

// Remove metamodule symlink
void remove_metamodule_symlink() {
    const std::string link_path =
        std::string(METAMODULE_DIR).substr(0, std::string(METAMODULE_DIR).length() - 1);

    struct stat st{};
    if (lstat(link_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(link_path.c_str());
        LOGI("Removed metamodule symlink");
    }
}

std::string build_install_wrapper_script(bool installing_metamodule) {
    const std::string installer_path = std::string(BINARY_DIR) + INSTALLER_SCRIPT_NAME;
    std::string script = "#!/system/bin/sh\n. ";
    script += installer_path;
    script += '\n';

    if (!installing_metamodule) {
        const std::string metamodule_path = get_metamodule_path_impl();
        const std::string metainstall_path = metamodule_path + "/" + METAMODULE_METAINSTALL_SCRIPT;
        if (!metamodule_path.empty() && !file_exists(metamodule_path + "/" + DISABLE_FILE_NAME) &&
            file_exists(metainstall_path)) {
            LOGI("Using metainstall.sh from metamodule: %s", metainstall_path.c_str());
            script += ". ";
            script += metainstall_path;
            script += "\nexit 0\n";
            return script;
        }
    }

    script += "install_module\nexit 0\n";
    return script;
}

bool exec_install_script(const std::string& zip_path, bool installing_metamodule,
                         const std::string& module_id) {
    std::array<char, PATH_MAX> realpath_buf{};
    if (realpath(zip_path.c_str(), realpath_buf.data()) == nullptr) {
        printf("! Invalid zip path: %s\n", zip_path.c_str());
        return false;
    }
    const std::string zipfile = realpath_buf.data();

    const std::string installer_path = std::string(BINARY_DIR) + INSTALLER_SCRIPT_NAME;
    if (!file_exists(installer_path)) {
        printf("! Missing installer script: %s\n", installer_path.c_str());
        return false;
    }

    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    char wrapper_path[] = "/dev/ksud_installer_XXXXXX";
    const int wrapper_fd = mkstemp(wrapper_path);
    if (wrapper_fd < 0) {
        printf("! Failed to create installer wrapper\n");
        return false;
    }

    const std::string wrapper_content = build_install_wrapper_script(installing_metamodule);
    FILE* wrapper_file = fdopen(wrapper_fd, "w");
    if (wrapper_file == nullptr) {
        close(wrapper_fd);
        unlink(wrapper_path);
        printf("! Failed to open installer wrapper\n");
        return false;
    }
    if (fputs(wrapper_content.c_str(), wrapper_file) == EOF || fclose(wrapper_file) != 0) {
        unlink(wrapper_path);
        printf("! Failed to write installer wrapper\n");
        return false;
    }
    chmod(wrapper_path, 0755);

    const CommonScriptEnv common_env = build_common_script_env();
    const pid_t pid = fork();
    if (pid < 0) {
        unlink(wrapper_path);
        return false;
    }

    if (pid == 0) {
        apply_common_script_env(common_env, module_id.c_str());
        setenv("OUTFD", "1", 1);
        setenv("ZIPFILE", zipfile.c_str(), 1);

        execl(busybox.c_str(), "sh", wrapper_path, nullptr);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    unlink(wrapper_path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

CommonScriptEnv build_common_script_env() {
    CommonScriptEnv env;
    env.kernel_ver_code = std::to_string(get_version());
    env.uapi_version = std::to_string(uapi_version());
    env.runtime_mode = runtime_mode();
    env.late_load = is_late_load();
    const auto [zygisk_value, zygisk_supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    env.zygisk_enabled = zygisk_supported && zygisk_value != 0;

    std::string binary_dir = std::string(BINARY_DIR);
    if (!binary_dir.empty() && binary_dir.back() == '/') {
        binary_dir.pop_back();
    }

    const char* old_path = getenv("PATH");
    if (old_path && old_path[0] != '\0') {
        env.path = std::string(old_path) + ":" + binary_dir;
    } else {
        env.path = binary_dir;
    }

    return env;
}

void apply_common_script_env(const CommonScriptEnv& env, const char* module_id,
                             bool set_magisk_compat) {
    setenv("ASH_STANDALONE", "1", 1);
    setenv("KSU", "true", 1);
    setenv("YUKISU", "1", 1);
    setenv("KSU_KERNEL_VER_CODE", env.kernel_ver_code.c_str(), 1);
    setenv("KSU_VER_CODE", VERSION_CODE, 1);
    setenv("KSU_VER", VERSION_NAME, 1);
    setenv("KSU_UAPI_VER", env.uapi_version.c_str(), 1);
    setenv("KSU_RUNTIME_MODE", env.runtime_mode.c_str(), 1);
    setenv("PATH", env.path.c_str(), 1);

    if (env.zygisk_enabled) {
        setenv("ZYGISK_ENABLED", "1", 1);
    } else {
        unsetenv("ZYGISK_ENABLED");
    }

    if (env.late_load) {
        setenv("KSU_LATE_LOAD", "1", 1);
    } else {
        unsetenv("KSU_LATE_LOAD");
    }

    if (set_magisk_compat) {
        setenv("MAGISK_VER", "25.2", 1);
        setenv("MAGISK_VER_CODE", "25200", 1);
    }

    if (module_id != nullptr && module_id[0] != '\0') {
        setenv("KSU_MODULE", module_id, 1);
    } else {
        unsetenv("KSU_MODULE");
    }
}

std::string get_metamodule_id() {
    return get_metamodule_id_impl();
}

int regenerate_preinit_rc() {
    const std::filesystem::path preinit_dir = preinit_ksu_dir();
    std::error_code ec;
    std::filesystem::create_directories(preinit_dir, ec);
    if (ec) {
        LOGW("Failed to create %s: %s", preinit_dir.c_str(), ec.message().c_str());
        return 1;
    }

    const std::filesystem::path tmp_path = preinit_dir / MODULES_RC_TMP_FILE;
    const std::filesystem::path out_path = preinit_dir / MODULES_RC_FILE;

    const int out_fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) {
        LOGW("Failed to create %s", tmp_path.c_str());
        return 1;
    }

    bool write_ok = collect_rc_files(std::filesystem::path(ADB_DIR) / "initrc.d", nullptr, out_fd);
    std::map<std::string, std::filesystem::path> modules;
    std::map<std::string, bool> skipped_modules;

    for (const char* root : {MODULE_UPDATE_DIR, MODULE_DIR}) {
        if (!std::filesystem::is_directory(root, ec))
            continue;
        for (auto it = std::filesystem::directory_iterator(root, ec);
             it != std::filesystem::directory_iterator() && !ec; it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_directory(ec))
                continue;
            const std::string id = entry.path().filename().string();
            if (id.empty())
                continue;
            if (file_exists((entry.path() / DISABLE_FILE_NAME).string()) ||
                file_exists((entry.path() / REMOVE_FILE_NAME).string())) {
                modules.erase(id);
                skipped_modules[id] = true;
                continue;
            }
            if (skipped_modules.count(id) == 0 && modules.count(id) == 0)
                modules.emplace(id, entry.path());
        }
    }

    for (const auto& [id, path] : modules) {
        if (!collect_rc_files(path / MODULE_INIT_RC_DIR, &id, out_fd)) {
            write_ok = false;
            break;
        }
    }

    if (close(out_fd) != 0)
        write_ok = false;
    if (!write_ok) {
        LOGW("Failed to write %s", tmp_path.c_str());
        unlink(tmp_path.c_str());
        return 1;
    }

    std::filesystem::rename(tmp_path, out_path, ec);
    if (ec) {
        LOGW("Failed to rename %s -> %s: %s", tmp_path.c_str(), out_path.c_str(),
             ec.message().c_str());
        std::filesystem::remove(tmp_path, ec);
        return 1;
    }

    (void)lsetfilecon(out_path, METADATA_FILE_CON);

    const std::filesystem::path stale_dir =
        (preinit_dir == PREINIT_DIR_WATCHDOG) ? PREINIT_DIR_DEFAULT : PREINIT_DIR_WATCHDOG;
    std::filesystem::remove(stale_dir / MODULES_RC_FILE, ec);

    return 0;
}

int module_install(const std::string& zip_path) {
    // Ensure stdout is unbuffered for real-time output
    if (setvbuf(stdout, nullptr, _IONBF, 0) != 0) {
        (void)0;  // best-effort
    }

    std::string uapi_error;
    if (!ensure_uapi_version_matched(&uapi_error)) {
        printf("! %s\n", uapi_error.c_str());
        return 1;
    }

    const auto boot_completed = getprop("sys.boot_completed");
    if (!boot_completed || *boot_completed != "1") {
        printf("! Android is Booting!\n");
        return 1;
    }

    printf("\n");
    printf("__   __ _   _  _  __ ___  ____   _   _ \n");
    printf("\\ \\ / /| | | || |/ /|_ _|/ ___| | | | |\n");
    printf(" \\ V / | | | || ' /  | | \\___ \\ | | | |\n");
    printf("  | |  | |_| || . \\  | |  ___) || |_| |\n");
    printf("  |_|   \\___/ |_|\\_\\|___||____/  \\___/ \n");
    printf("\n");
    if (fflush(stdout) != 0) {
        (void)0;  // best-effort
    }
    // Ensure banner is output before script execution

    // Refresh binary assets so installer.sh and busybox links stay in sync with the build.
    if (ensure_binaries(false) != 0) {
        printf("! Failed to extract binary assets\n");
        return 1;
    }

    LOGI("Installing module from %s", zip_path.c_str());

    // Check if zip file exists
    if (!file_exists(zip_path)) {
        printf("! Module file not found: %s\n", zip_path.c_str());
        return 1;
    }

    const auto prop_text = read_zip_entry(zip_path, "module.prop");
    if (!prop_text) {
        printf("! Unable to read module.prop from zip file\n");
        return 1;
    }
    const auto props = parse_module_prop_content(*prop_text);

    const std::string mod_id = props.count("id") ? trim(props.at("id")) : "";
    if (mod_id.empty()) {
        printf("! Module ID not found in module.prop\n");
        return 1;
    }
    if (!validate_module_id(mod_id)) {
        printf("! Invalid module ID: %s\n", mod_id.c_str());
        return 1;
    }

    const bool installing_metamodule = is_metamodule(props);

    if (!installing_metamodule) {
        const int safety = check_install_safety(false);
        if (safety != 0) {
            printf("\n❌ Installation Blocked\n");
            printf("┌────────────────────────────────\n");
            printf("│ A metamodule with custom installer is active\n");
            printf("│\n");
            if (safety == 1) {
                printf("│ Current state: Disabled\n");
                printf("│ Action required: Re-enable or uninstall it, then reboot\n");
            } else {
                printf("│ Current state: Pending changes\n");
                printf("│ Action required: Reboot to apply changes first\n");
            }
            printf("└─────────────────────────────────\n\n");
            return 1;
        }
    }

    if (installing_metamodule) {
        const std::string existing_id = get_metamodule_id_impl();
        if (!existing_id.empty() && existing_id != mod_id) {
            printf("\n❌ Installation Failed\n");
            printf("┌────────────────────────────────\n");
            printf("│ A metamodule is already installed\n");
            printf("│   Current metamodule: %s\n", existing_id.c_str());
            printf("│\n");
            printf("│ Only one metamodule can be active at a time.\n");
            printf("│\n");
            printf("│ To install this metamodule:\n");
            printf("│   1. Uninstall the current metamodule\n");
            printf("│   2. Reboot your device\n");
            printf("│   3. Install the new metamodule\n");
            printf("└─────────────────────────────────\n\n");
            return 1;
        }
    }

    // Use the embedded installer script (same as the official Rust ksud flow)
    if (!exec_install_script(zip_path, installing_metamodule, mod_id)) {
        printf("! Module installation failed\n");
        return 1;
    }

    const std::string final_module = std::string(MODULE_DIR) + mod_id;
    if (!ensure_dir_exists(std::string(MODULE_DIR)) || !ensure_dir_exists(final_module)) {
        printf("! Failed to create module directory\n");
        return 1;
    }
    if (!copy_file_data(std::string(MODULE_UPDATE_DIR) + mod_id + "/module.prop",
                        final_module + "/module.prop")) {
        printf("! Failed to stage module.prop\n");
        return 1;
    }
    if (!touch_file(final_module + "/" + std::string(UPDATE_FILE_NAME))) {
        printf("! Failed to mark module as updated\n");
        return 1;
    }

    if (installing_metamodule && !create_metamodule_symlink(mod_id)) {
        printf("! Failed to create metamodule symlink\n");
        return 1;
    }

    LOGI("Module installed successfully");
    warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
    warn_refresh_yukizygisk_early_snapshot_failed();
    return 0;
}

int module_uninstall(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    // Create remove flag
    const std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;
    if (!ensure_file_exists(remove_flag)) {
        LOGE("Failed to create remove flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s marked for removal\n", id.c_str());
    warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
    warn_refresh_yukizygisk_early_snapshot_failed();
    return 0;
}

int module_undo_uninstall(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string remove_flag = module_dir + "/" + REMOVE_FILE_NAME;

    if (!file_exists(remove_flag)) {
        printf("Module %s is not marked for removal\n", id.c_str());
        return 1;
    }

    if (unlink(remove_flag.c_str()) != 0) {
        LOGE("Failed to remove flag for %s", id.c_str());
        return 1;
    }

    printf("Undid uninstall for module %s\n", id.c_str());
    warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
    warn_refresh_yukizygisk_early_snapshot_failed();
    return 0;
}

int module_enable(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    if (file_exists(disable_flag)) {
        if (unlink(disable_flag.c_str()) != 0) {
            LOGE("Failed to enable module %s", id.c_str());
            return 1;
        }
    }

    printf("Module %s enabled\n", id.c_str());
    warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
    warn_refresh_yukizygisk_early_snapshot_failed();
    return 0;
}

int module_disable(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;

    if (!file_exists(module_dir)) {
        printf("Module %s not found\n", id.c_str());
        return 1;
    }

    const std::string disable_flag = module_dir + "/" + DISABLE_FILE_NAME;
    if (!ensure_file_exists(disable_flag)) {
        LOGE("Failed to create disable flag for %s", id.c_str());
        return 1;
    }

    printf("Module %s disabled\n", id.c_str());
    warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
    warn_refresh_yukizygisk_early_snapshot_failed();
    return 0;
}

int module_run_action(const std::string& id) {
    if (!validate_module_id(id)) {
        printf("Invalid module ID: %s\n", id.c_str());
        return 1;
    }

    std::string uapi_error;
    if (!ensure_uapi_version_matched(&uapi_error)) {
        printf("! %s\n", uapi_error.c_str());
        return 1;
    }

    const std::string module_dir = std::string(MODULE_DIR) + id;
    const std::string action_script = module_dir + "/" + MODULE_ACTION_SH;

    if (!file_exists(action_script)) {
        printf("Module %s has no action script\n", id.c_str());
        return 1;
    }

    // Run action script with module_id for KSU_MODULE env var
    return run_script(action_script, true, id);
}

namespace {

bool is_dedicated_prop_key(const std::string& key) {
    static constexpr std::array<std::string_view, 19> kDedicatedKeys = {
        "id",        "dir_id",      "name",          "version",    "versionCode",
        "author",    "description", "enabled",       "update",     "remove",
        "web",       "action",      "mount",         "metamodule", "actionIcon",
        "webuiIcon", "runtime",     "runtimeLoaded", "yzConflict"};
    return std::find(kDedicatedKeys.begin(), kDedicatedKeys.end(), key) != kDedicatedKeys.end();
}

// Third-party Zygisk implementations that cannot coexist with built-in YukiZygisk.
constexpr std::array<std::string_view, 3> kZygiskImplModuleIds = {"zygisksu", "rezygisk",
                                                                  "yukizygisk"};

bool is_zygisk_impl_module(const std::string& dir_id) {
    return std::find(kZygiskImplModuleIds.begin(), kZygiskImplModuleIds.end(), dir_id) !=
           kZygiskImplModuleIds.end();
}

// Create the disable flag when it is missing. Returns whether anything changed.
bool ensure_module_disabled(const std::string& module_path) {
    const std::string disable_flag = module_path + "/" + DISABLE_FILE_NAME;
    if (file_exists(disable_flag)) {
        return false;
    }
    if (!ensure_file_exists(disable_flag)) {
        LOGW("Failed to force-disable %s", module_path.c_str());
        return false;
    }
    LOGI("Force-disabled %s: conflicts with built-in YukiZygisk", module_path.c_str());
    return true;
}

// Disable the third-party Zygisk implementations before the modules are collected,
// so the listing already reports them off and nothing races the manager.
bool disable_zygisk_impl_modules() {
    bool changed = false;
    for (const char* root : {MODULE_DIR, MODULE_UPDATE_DIR}) {
        for (const std::string_view dir_id : kZygiskImplModuleIds) {
            const std::string module_path = std::string(root) + std::string(dir_id);
            if (!file_exists(module_path)) {
                continue;
            }
            changed = ensure_module_disabled(module_path) || changed;
        }
    }
    return changed;
}

bool load_module_info(const std::string& module_path, const std::string& dir_id,
                      bool pending_update, ModuleInfo& info) {
    const std::string prop_path = module_path + "/module.prop";
    if (!file_exists(prop_path)) {
        return false;
    }

    auto props = parse_module_prop(prop_path);

    // The directory name is the real identity: ksud and the manager address a
    // module by it, while module.prop's id is only a display/update key.
    info.dir_id = dir_id;
    info.id = props.count("id") && !props["id"].empty() ? props["id"] : dir_id;
    info.name = props.count("name") ? props["name"] : info.id;
    info.version = props.count("version") ? props["version"] : "";
    info.version_code = props.count("versionCode") ? props["versionCode"] : "";
    info.author = props.count("author") ? props["author"] : "";
    info.description = props.count("description") ? props["description"] : "";
    info.enabled = !file_exists(module_path + "/" + DISABLE_FILE_NAME);
    info.update = pending_update || file_exists(module_path + "/" + UPDATE_FILE_NAME);
    info.remove = file_exists(module_path + "/" + REMOVE_FILE_NAME);
    info.web = file_exists(module_path + "/" + MODULE_WEB_DIR);
    info.action = file_exists(module_path + "/" + MODULE_ACTION_SH);
    info.mount = file_exists(module_path + "/system") && !file_exists(module_path + "/skip_mount");

    const std::string metamodule_val = props.count("metamodule") ? props["metamodule"] : "";
    info.metamodule =
        (metamodule_val == "1" || metamodule_val == "true" || metamodule_val == "TRUE");

    if (props.count("actionIcon")) {
        info.actionIcon =
            resolve_module_icon_path(props["actionIcon"], info.id, module_path, "actionIcon");
    }
    if (props.count("webuiIcon")) {
        info.webuiIcon =
            resolve_module_icon_path(props["webuiIcon"], info.id, module_path, "webuiIcon");
    }

    if (yz_has_native_modules(module_path)) {
        info.runtime = "native";
    } else if (yz_is_zygisk_module(module_path)) {
        info.runtime = "zygisk";
    }

    for (auto& [key, value] : props) {
        if (key.empty() || is_dedicated_prop_key(key)) {
            continue;
        }
        info.extra_props.emplace(key, value);
    }

    return true;
}

void collect_module_infos(const std::string& root_dir, bool pending_update,
                          std::vector<ModuleInfo>& modules,
                          std::map<std::string, size_t>& module_index) {
    DIR* dir = opendir(root_dir.c_str());
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (entry->d_type != DT_DIR) {
            continue;
        }

        const std::string module_path = root_dir + entry->d_name;
        ModuleInfo info;
        if (!load_module_info(module_path, entry->d_name, pending_update, info)) {
            continue;
        }

        // Index by directory name: the pending copy under MODULE_UPDATE_DIR shares
        // it with the installed module even when the update changes module.prop's
        // id, and two distinct directories declaring the same id stay separate.
        const auto [it, inserted] = module_index.emplace(info.dir_id, modules.size());
        if (inserted) {
            modules.push_back(std::move(info));
            continue;
        }

        modules[it->second].update = modules[it->second].update || info.update;
    }

    closedir(dir);
}

}  // namespace

int module_list() {
    const bool yukizygisk_on = yz_feature_enabled();
    if (yukizygisk_on && disable_zygisk_impl_modules()) {
        warn_regenerate_preinit_rc_failed(regenerate_preinit_rc());
        warn_refresh_yukizygisk_early_snapshot_failed();
    }

    std::vector<ModuleInfo> modules;
    std::map<std::string, size_t> module_index;
    collect_module_infos(MODULE_DIR, false, modules, module_index);
    collect_module_infos(MODULE_UPDATE_DIR, true, modules, module_index);

    if (yukizygisk_on) {
        const std::set<std::string> loaded = yz_loaded_module_ids();
        for (ModuleInfo& module : modules) {
            module.runtime_loaded = loaded.count(module.dir_id) != 0;
            module.yz_conflict = is_zygisk_impl_module(module.dir_id);
        }
    }

    // Output JSON array
    printf("[\n");
    for (size_t i = 0; i < modules.size(); i++) {
        const auto& m = modules[i];
        printf("  {\n");
        printf("    \"id\": \"%s\",\n", escape_json(m.id).c_str());
        printf("    \"dir_id\": \"%s\",\n", escape_json(m.dir_id).c_str());
        printf("    \"name\": \"%s\",\n", escape_json(m.name).c_str());
        printf("    \"version\": \"%s\",\n", escape_json(m.version).c_str());
        printf("    \"versionCode\": \"%s\",\n", escape_json(m.version_code).c_str());
        printf("    \"author\": \"%s\",\n", escape_json(m.author).c_str());
        printf("    \"description\": \"%s\",\n", escape_json(m.description).c_str());
        printf("    \"enabled\": \"%s\",\n", m.enabled ? "true" : "false");
        printf("    \"update\": \"%s\",\n", m.update ? "true" : "false");
        printf("    \"remove\": \"%s\",\n", m.remove ? "true" : "false");
        printf("    \"web\": \"%s\",\n", m.web ? "true" : "false");
        printf("    \"action\": \"%s\",\n", m.action ? "true" : "false");
        printf("    \"mount\": \"%s\",\n", m.mount ? "true" : "false");
        printf("    \"metamodule\": \"%s\"", m.metamodule ? "true" : "false");
        if (!m.runtime.empty()) {
            printf(",\n    \"runtime\": \"%s\"", m.runtime.c_str());
        }
        printf(",\n    \"runtimeLoaded\": \"%s\"", m.runtime_loaded ? "true" : "false");
        printf(",\n    \"yzConflict\": \"%s\"", m.yz_conflict ? "true" : "false");
        if (!m.actionIcon.empty()) {
            printf(",\n    \"actionIcon\": \"%s\"", escape_json(m.actionIcon).c_str());
        }
        if (!m.webuiIcon.empty()) {
            printf(",\n    \"webuiIcon\": \"%s\"", escape_json(m.webuiIcon).c_str());
        }
        for (const auto& [key, value] : m.extra_props) {
            printf(",\n    \"%s\": \"%s\"", escape_json(key).c_str(), escape_json(value).c_str());
        }
        printf("\n  }%s\n", i < modules.size() - 1 ? "," : "");
    }
    printf("]\n");

    return 0;
}

int uninstall_all_modules() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        module_uninstall(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int prune_modules() {
    // Remove modules marked for removal
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;
        const std::string remove_flag = module_path + "/" + REMOVE_FILE_NAME;

        if (file_exists(remove_flag)) {
            const std::string module_id = entry->d_name;
            const auto props = parse_module_prop(module_path + "/module.prop");
            const bool removing_metamodule = is_metamodule(props);

            if (removing_metamodule) {
                remove_metamodule_symlink();
            } else {
                const int metauninstall_rc = metamodule_exec_uninstall_script(module_id);
                if (metauninstall_rc != 0) {
                    LOGW("metauninstall.sh failed for %s with code %d", module_id.c_str(),
                         metauninstall_rc);
                }
            }

            const std::string uninstall_script = module_path + "/uninstall.sh";
            if (file_exists(uninstall_script)) {
                const int uninstall_rc = run_script(uninstall_script, true, module_id);
                if (uninstall_rc != 0) {
                    LOGW("uninstall.sh failed for %s with code %d", module_id.c_str(),
                         uninstall_rc);
                }
            }

            std::error_code config_error;
            std::filesystem::remove_all(std::string(MODULE_CONFIG_DIR) + module_id, config_error);
            if (config_error) {
                LOGW("Failed to remove config for %s: %s", module_id.c_str(),
                     config_error.message().c_str());
            }

            std::error_code ec;
            std::filesystem::remove_all(module_path, ec);
            if (ec) {
                LOGW("Failed to remove module %s: %s", entry->d_name, ec.message().c_str());
            } else {
                LOGI("Removed module %s", entry->d_name);
            }
        }
    }

    closedir(dir);
    return 0;
}

int disable_all_modules() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        module_disable(entry->d_name);
    }

    closedir(dir);
    return 0;
}

int handle_updated_modules() {
    // Check modules_update directory and move updated modules
    const std::string update_dir = std::string(ADB_DIR) + "modules_update/";
    DIR* dir = opendir(update_dir.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string src = update_dir + entry->d_name;
        const std::string dst = std::string(MODULE_DIR) + entry->d_name;
        const bool disabled = file_exists(dst + "/" + DISABLE_FILE_NAME);
        const bool removed = file_exists(dst + "/" + REMOVE_FILE_NAME);

        // Remove old module if exists
        if (file_exists(dst)) {
            std::error_code remove_error;
            std::filesystem::remove_all(dst, remove_error);
            if (remove_error) {
                LOGW("Failed to remove old module %s: %s", dst.c_str(),
                     remove_error.message().c_str());
            }
        }

        // Move updated module
        if (rename(src.c_str(), dst.c_str()) == 0) {
            if (removed) {
                ensure_file_exists(dst + "/" + REMOVE_FILE_NAME);
            } else if (disabled) {
                ensure_file_exists(dst + "/" + DISABLE_FILE_NAME);
            }
            LOGI("Updated module: %s", entry->d_name);
        } else {
            LOGE("Failed to update module: %s", entry->d_name);
        }
    }

    closedir(dir);
    return 0;
}

int run_script(const std::string& script, bool block, const std::string& module_id,
               const char* extra_env_name, const char* extra_env_value) {
    if (!file_exists(script))
        return 0;

    LOGI("Running script: %s", script.c_str());

    // Use busybox for script execution (like Rust version)
    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    // Get the script's directory for current_dir
    std::string script_dir = script.substr(0, script.find_last_of('/'));
    if (script_dir.empty())
        script_dir = "/";

    // Prepare all environment variable values BEFORE fork
    // to avoid calling C++ library functions in child process
    const CommonScriptEnv common_env = build_common_script_env();

    // Make copies of string data that child process will use
    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();
    const char* script_dir_path = script_dir.c_str();
    const char* module_id_cstr = module_id.c_str();
    const char* extra_env_name_cstr = extra_env_name;
    const char* extra_env_value_cstr = extra_env_value;

    // Rust's Command::spawn waits for pre_exec and exec through an internal
    // CLOEXEC error pipe. Mirror that behavior: the parent must not return to
    // init until the child has detached, switched cgroups, and entered exec.
    int exec_status_pipe[2] = {-1, -1};
    if (pipe2(exec_status_pipe, O_CLOEXEC) != 0) {
        LOGE("Failed to create exec status pipe for script %s: %s", script.c_str(),
             strerror(errno));
        return -1;
    }

    const pid_t pid = fork();
    if (pid == 0) {
        // Child process
        close(exec_status_pipe[0]);

        // Match upstream's Command::pre_exec setup.
        detach_process_group(true);
        switch_cgroups();

        // Change to script directory (like Rust version)
        if (chdir(script_dir_path) != 0) {
            const int child_errno = errno;
            ssize_t ignored;
            do {
                ignored = write(exec_status_pipe[1], &child_errno, sizeof(child_errno));
            } while (ignored < 0 && errno == EINTR);
            _exit(127);
        }

        // Set environment variables (matching Rust version's get_common_script_envs)
        apply_common_script_env(common_env, module_id_cstr, true);
        if (extra_env_name_cstr != nullptr && extra_env_value_cstr != nullptr) {
            setenv(extra_env_name_cstr, extra_env_value_cstr, 1);
        }

        // Execute with busybox sh
        execl(busybox_path, "sh", script_path, nullptr);

        const int child_errno = errno;
        ssize_t ignored;
        do {
            ignored = write(exec_status_pipe[1], &child_errno, sizeof(child_errno));
        } while (ignored < 0 && errno == EINTR);
        _exit(127);
    }

    close(exec_status_pipe[1]);

    if (pid < 0) {
        close(exec_status_pipe[0]);
        LOGE("Failed to fork for script: %s", script.c_str());
        return -1;
    }

    int child_errno = 0;
    ssize_t received;
    do {
        received = read(exec_status_pipe[0], &child_errno, sizeof(child_errno));
    } while (received < 0 && errno == EINTR);
    close(exec_status_pipe[0]);

    if (received > 0) {
        int ignored_status;
        while (waitpid(pid, &ignored_status, 0) < 0 && errno == EINTR) {
        }
        LOGE("Failed to exec script %s: %s", script.c_str(), strerror(child_errno));
        return -1;
    }
    if (received < 0) {
        LOGE("Failed to receive exec status for script %s: %s", script.c_str(), strerror(errno));
        return -1;
    }

    if (block) {
        int status;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            LOGE("Failed to wait for script %s: %s", script.c_str(), strerror(errno));
            return -1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    return 0;
}

int exec_stage_script(const std::string& stage, bool block) {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    const std::string metamodule_id = get_metamodule_id_impl();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_id = entry->d_name;
        if (!metamodule_id.empty() && module_id == metamodule_id)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + module_id;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        // Skip modules marked for removal
        if (file_exists(module_path + "/" + REMOVE_FILE_NAME))
            continue;

        // Run stage script with module_id for KSU_MODULE env var
        std::string script;
        script.reserve(module_path.size() + 1U + stage.size() + 3U);
        script += module_path;
        script += "/";
        script += stage;
        script += ".sh";
        run_script(script, block, module_id);
    }

    closedir(dir);
    return 0;
}

int exec_common_scripts(const std::string& stage_dir, bool block) {
    const std::string dir_path = std::string(ADB_DIR) + stage_dir + "/";
    DIR* dir = opendir(dir_path.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string script = dir_path + entry->d_name;
        if (access(script.c_str(), X_OK) != 0)
            continue;

        run_script(script, block);
    }

    closedir(dir);
    return 0;
}

int load_sepolicy_rule() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string rule_file = module_path + "/sepolicy.rule";
        if (!file_exists(rule_file))
            continue;

        // Read and apply rules
        const auto content = read_file(rule_file);
        if (!content)
            continue;
        std::string all_rules;
        for_each_line(*content, [&all_rules](std::string_view line) {
            line = trim_view(line);
            if (line.empty() || line[0] == '#')
                return;
            all_rules.append(line);
            all_rules += '\n';
        });

        if (!all_rules.empty()) {
            LOGI("Applying sepolicy rules from %s", entry->d_name);
            const int ret = sepolicy_live_patch(all_rules);
            if (ret != 0) {
                LOGW("Failed to apply some sepolicy rules from %s", entry->d_name);
            }
        }
    }

    closedir(dir);
    return 0;
}

int load_system_prop() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

#if !defined(RESETPROP_ALONE_AVAILABLE) || !RESETPROP_ALONE_AVAILABLE
    // Only relevant without the built-in: the fallback path below execs this binary.
    if (!file_exists(RESETPROP_PATH)) {
        LOGW("resetprop not found at %s, skipping system.prop loading", RESETPROP_PATH);
        closedir(dir);
        return 0;
    }
#endif  // #if !defined(RESETPROP_ALONE_AVAILABLE) ...

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string prop_file = module_path + "/system.prop";
        if (!file_exists(prop_file))
            continue;

        LOGI("Loading system.prop from %s", entry->d_name);

        // Read and set properties. The built-in resetprop is safe to call
        // directly; forking once per property only duplicated ksud's page
        // tables and added a waitpid round-trip for no isolation benefit.
        const auto content = read_file(prop_file);
        if (!content)
            continue;
        for_each_line(*content, [](std::string_view line) {
            line = trim_view(line);
            if (line.empty() || line[0] == '#')
                return;

            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
                return;

            const std::string key(trim_view(line.substr(0, eq)));
            const std::string value(trim_view(line.substr(eq + 1)));
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
            std::array<char*, 5> argv_c = {
                const_cast<char*>("resetprop"),
                const_cast<char*>("-n"),
                const_cast<char*>(key.c_str()),
                const_cast<char*>(value.c_str()),
                nullptr,
            };
            if (resetprop_main(4, argv_c.data()) != 0)
                LOGW("Failed to apply property %s", key.c_str());
#else
            const pid_t pid = fork();
            if (pid == 0) {
                execl(RESETPROP_PATH, "resetprop", "-n", key.c_str(), value.c_str(), nullptr);
                _exit(127);
            }
            if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
        });
    }

    closedir(dir);
    return 0;
}

// Parse bool config value (true, yes, 1, on -> true)
bool parse_bool_config(const std::string& value) {
    std::string lower = value;
    for (char& c : lower)
        c = tolower(c);
    return lower == "true" || lower == "yes" || lower == "1" || lower == "on";
}

// Merge module configs (persist + temp, temp takes priority)
std::map<std::string, std::string> merge_module_configs(const std::string& module_id) {
    std::map<std::string, std::string> config;

    const std::string config_dir = std::string(MODULE_CONFIG_DIR) + module_id + "/";
    const std::string persist_path = config_dir + PERSIST_CONFIG_NAME;
    const std::string temp_path = config_dir + TEMP_CONFIG_NAME;

    const auto merge_content = [&config](const std::optional<std::string>& content) {
        if (!content)
            return;
        for_each_line(*content, [&config](std::string_view line) {
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos) {
                config[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
            }
        });
    };

    // Temp values override persisted ones.
    merge_content(read_file(persist_path));
    merge_content(read_file(temp_path));

    return config;
}

std::map<std::string, std::vector<std::string>> get_managed_features() {
    std::map<std::string, std::vector<std::string>> managed_features_map;

    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        return managed_features_map;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_id = entry->d_name;
        const std::string module_path = std::string(MODULE_DIR) + module_id;

        // Check if module is active (not disabled/removed)
        if (file_exists(module_path + "/disable"))
            continue;
        if (file_exists(module_path + "/remove"))
            continue;

        // Read module config
        auto config = merge_module_configs(module_id);

        // Extract manage.* config entries
        std::vector<std::string> feature_list;
        for (const auto& [key, value] : config) {
            // Check if key starts with "manage."
            if (key.size() > 7 && key.substr(0, 7) == "manage.") {
                const std::string feature_name = key.substr(7);
                if (parse_bool_config(value)) {
                    feature_list.push_back(feature_name);
                }
            }
        }

        if (!feature_list.empty()) {
            managed_features_map[module_id] = feature_list;
        }
    }

    closedir(dir);
    return managed_features_map;
}

}  // namespace ksud
