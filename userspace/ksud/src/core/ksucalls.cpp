#include "ksucalls.hpp"
#include "../defs.hpp"
#include "../log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ksud {

// Magic constants
// NOTE: Avoid 0xDEAD/0xBEEF patterns - easily detected by root checkers

namespace {

struct PrctlGetFdCmd {
    int32_t result{};
    int32_t fd{};
};

int g_driver_fd = -1;
bool g_driver_fd_init = false;
GetInfoCmd g_info_cache{};
bool g_info_cached = false;

constexpr size_t kLinkPathSize = 64;
constexpr size_t kReadlinkBufSize = 256;
constexpr const char* kDriverFdName = "anon_inode:[ksu_driver]";
constexpr const char* kSuDriverFdName = "anon_inode:[ksu_driver_su]";
constexpr int kSysSeccomp = 1;

thread_local volatile sig_atomic_t g_reboot_fallback_in_flight = 0;
thread_local volatile sig_atomic_t g_reboot_fallback_trapped = 0;
struct sigaction g_previous_sigsys_action{};
volatile sig_atomic_t g_sigsys_handler_installed = 0;

bool set_syscall_permission_error(void* context) {
    if (context == nullptr) {
        return false;
    }

    auto* ucontext = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    ucontext->uc_mcontext.regs[0] = static_cast<unsigned long>(-EPERM);
    return true;
#elif defined(__x86_64__)
    ucontext->uc_mcontext.gregs[REG_RAX] = -EPERM;
    return true;
#else
    (void)ucontext;
    return false;
#endif
}

// POSIX sigaction exposes its handler and metadata through unions.
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
void forward_sigsys(int signal, siginfo_t* info, void* context) {
    const struct sigaction previous = g_previous_sigsys_action;
    if (previous.sa_handler == SIG_IGN)
        return;
    if (previous.sa_handler == SIG_DFL)
        _exit(128 + signal);
    if ((previous.sa_flags & SA_SIGINFO) != 0) {
        previous.sa_sigaction(signal, info, context);
    } else {
        previous.sa_handler(signal);
    }
}

void sigsys_handler(int signal, siginfo_t* info, void* context) {
    const bool is_reboot_seccomp_trap =
        signal == SIGSYS && info != nullptr && info->si_code == kSysSeccomp &&
        info->si_syscall == SYS_reboot && g_reboot_fallback_in_flight != 0;
    if (!is_reboot_seccomp_trap || !set_syscall_permission_error(context)) {
        forward_sigsys(signal, info, context);
        return;
    }

    g_reboot_fallback_trapped = 1;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

struct DriverFd {
    int fd{-1};
    bool su_session{false};
    int error{0};
};

auto scan_driver_fd() -> DriverFd {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return {-1, false, errno != 0 ? errno : EIO};
    }

    int found_fd = -1;
    std::array<char, kLinkPathSize> link_path{};
    std::array<char, kReadlinkBufSize> target{};

    // readdir is not thread-safe; we use it only during single-threaded init.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char* end = nullptr;
        const long fd_num = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || fd_num < 0) {
            continue;
        }

        const int snprintf_ret =
            snprintf(link_path.data(), link_path.size(), "/proc/self/fd/%ld", fd_num);
        if (snprintf_ret <= 0 || static_cast<size_t>(snprintf_ret) >= link_path.size()) {
            continue;
        }

        const ssize_t len = readlink(link_path.data(), target.data(), target.size() - 1);
        if (len > 0 && static_cast<size_t>(len) < target.size()) {
            target[static_cast<size_t>(len)] = '\0';
            if (strcmp(target.data(), kSuDriverFdName) == 0) {
                closedir(dir);
                return {static_cast<int>(fd_num), true};
            }
            if (strcmp(target.data(), kDriverFdName) == 0) {
                found_fd = static_cast<int>(fd_num);
            }
        }
    }

    closedir(dir);
    return {found_fd, false};
}

auto init_driver_fd() -> int {
    // Method 1: Check if we already have an inherited fd
    const DriverFd driver = scan_driver_fd();
    if (driver.fd >= 0) {
        LOGD("Found inherited driver fd: %d", driver.fd);
        return driver.fd;
    }

    // Method 2: Try prctl to get fd (SECCOMP-safe)
    PrctlGetFdCmd prctl_cmd = {-1, -1};
    prctl(KSU_PRCTL_GET_FD, &prctl_cmd, 0, 0, 0);
    if (prctl_cmd.result == 0 && prctl_cmd.fd >= 0) {
        LOGD("Got driver fd via prctl: %d", prctl_cmd.fd);
        return prctl_cmd.fd;
    }

    // Method 3: Fallback to reboot syscall (may be blocked by SECCOMP)
    setup_sigsys_handler();
    if (g_sigsys_handler_installed == 0) {
        LOGE("Skipping KernelSU driver fd fallback without a SIGSYS handler");
        return -1;
    }
    int fd_reboot = -1;  // NOLINT(misc-const-correctness) written via pointer by syscall
    g_reboot_fallback_trapped = 0;
    g_reboot_fallback_in_flight = 1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd_reboot);
    g_reboot_fallback_in_flight = 0;
    if (g_reboot_fallback_trapped != 0) {
        LOGE("KernelSU driver fd fallback was blocked by seccomp");
    }
    if (fd_reboot >= 0) {
        LOGD("Got driver fd via reboot syscall: %d", fd_reboot);
        return fd_reboot;
    }

    LOGE("Failed to get driver fd");
    return -1;
}

auto get_driver_fd() -> int {
    if (!g_driver_fd_init) {
        g_driver_fd = init_driver_fd();
        g_driver_fd_init = true;
    }
    return g_driver_fd;
}

}  // namespace

void setup_sigsys_handler() {
    if (g_sigsys_handler_installed != 0) {
        return;
    }

    struct sigaction action{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    action.sa_sigaction = sigsys_handler;
    action.sa_flags = SA_SIGINFO;
    (void)sigemptyset(&action.sa_mask);
    g_sigsys_handler_installed = 1;
    if (sigaction(SIGSYS, &action, &g_previous_sigsys_action) != 0) {
        g_sigsys_handler_installed = 0;
        LOGW("Failed to install SIGSYS handler: %s", strerror(errno));
        return;
    }
}

int claim_inherited_su_driver_fd() {
    const DriverFd driver = scan_driver_fd();
    if (driver.error != 0) {
        return -driver.error;
    }
    if (driver.fd < 0 || !driver.su_session) {
        return 0;
    }

    g_driver_fd = driver.fd;
    g_driver_fd_init = true;
    LOGD("Claimed inherited su-session driver fd: %d", driver.fd);
    return 1;
}

int ksuctl(int request, void* arg) {
    const int fd = get_driver_fd();
    if (fd < 0) {
        return -1;
    }

    const int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        LOGE("ioctl failed: request=0x%x, errno=%d (%s)", request, errno, strerror(errno));
        return -1;
    }

    return ret;
}

namespace {

int uts_ksuctl(int request, void* arg) {
    const int fd = get_driver_fd();
    if (fd < 0)
        return -ENODEV;

    const int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        const int error = errno;
        LOGE("UTS ioctl failed: request=0x%x, errno=%d (%s)", request, error, strerror(error));
        return -error;
    }
    return ret;
}

const GetInfoCmd& get_info() {
    if (!g_info_cached) {
        GetInfoCmd cmd{};
        ksuctl(KSU_IOCTL_GET_INFO, &cmd);
        g_info_cache = cmd;
        g_info_cached = true;
    }
    return g_info_cache;
}

void report_event(uint32_t event) {
    ReportEventCmd cmd = {event};
    ksuctl(KSU_IOCTL_REPORT_EVENT, &cmd);
}

}  // namespace

int32_t get_version() {
    return static_cast<int32_t>(get_info().version);
}

uint32_t get_flags() {
    return get_info().flags;
}

uint32_t get_uapi_version() {
    uint32_t v = 0;
    ksuctl(KSU_IOCTL_GET_UAPI_VERSION, &v);
    return v;
}

bool is_lkm() {
    return (get_flags() & KSU_GET_INFO_FLAG_LKM) != 0;
}

bool is_lkm_bundled() {
    const auto flags = get_flags();
    return (flags & KSU_GET_INFO_FLAG_LKM) != 0 && (flags & KSU_GET_INFO_FLAG_BUNDLED) != 0;
}

bool is_late_load() {
    return (get_flags() & KSU_GET_INFO_FLAG_LATE_LOAD) != 0;
}

const char* runtime_mode() {
    if (is_late_load()) {
        return "late-load";
    }
    // YukiSU has no built-in kernel mode; a normal load is always LKM.
    return "lkm";
}

bool ensure_uapi_version_matched(std::string* error) {
    const uint32_t kernel_uapi = get_uapi_version();
    const uint32_t userspace_uapi = uapi_version();
    if (kernel_uapi == userspace_uapi) {
        return true;
    }

    const std::string message = "UAPI version mismatch: kernel=" + std::to_string(kernel_uapi) +
                                ", ksud=" + std::to_string(userspace_uapi) +
                                ". Please update YukiSU!";
    if (error != nullptr) {
        *error = message;
    }
    LOGE("%s", message.c_str());
    return false;
}

int grant_root() {
    return ksuctl(KSU_IOCTL_GRANT_ROOT, nullptr);
}

int set_ksu_no_new_privs() {
    return ksuctl(KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT, nullptr);
}

void report_post_fs_data() {
    report_event(EVENT_POST_FS_DATA);
}

int report_services() {
    ReportEventCmd cmd = {EVENT_SERVICES};
    return ksuctl(KSU_IOCTL_REPORT_EVENT, &cmd);
}

void report_boot_complete() {
    report_event(EVENT_BOOT_COMPLETED);
}

void report_module_mounted() {
    report_event(EVENT_MODULE_MOUNTED);
}

bool check_kernel_safemode() {
    CheckSafemodeCmd cmd = {0};
    ksuctl(KSU_IOCTL_CHECK_SAFEMODE, &cmd);
    return cmd.in_safe_mode != 0;
}

int set_sepolicy(const void* payload, uint64_t payload_len) {
    SetSepolicyCmd ioctl_cmd = {payload_len, reinterpret_cast<uint64_t>(payload)};
    return ksuctl(KSU_IOCTL_SET_SEPOLICY, &ioctl_cmd);
}

std::pair<uint64_t, bool> get_feature(uint32_t feature_id) {
    GetFeatureCmd cmd = {feature_id, 0, 0};
    const int ret = ksuctl(KSU_IOCTL_GET_FEATURE, &cmd);
    if (ret < 0) {
        return {0, false};
    }
    return {cmd.value, cmd.supported != 0};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int set_feature(uint32_t feature_id, uint64_t value) {
    SetFeatureCmd cmd = {feature_id, value};
    return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd);
}

int get_uts_view_config(ksu_uts_view_config* config) {
    if (config == nullptr)
        return -EINVAL;
    memset(config, 0, sizeof(*config));
    return uts_ksuctl(KSU_IOCTL_GET_UTS_VIEW_CONFIG, config);
}

int get_su_path_config(ksu_su_path_config* config) {
    if (config == nullptr)
        return -EINVAL;
    *config = {};
    const int fd = get_driver_fd();
    if (fd < 0)
        return -ENODEV;
    return ioctl(fd, KSU_IOCTL_GET_SU_PATH, config) < 0 ? -errno : 0;
}

int set_su_path_config(const ksu_su_path_config& config) {
    auto request = config;
    const int fd = get_driver_fd();
    if (fd < 0)
        return -ENODEV;
    return ioctl(fd, KSU_IOCTL_SET_SU_PATH, &request) < 0 ? -errno : 0;
}

int set_uts_view_config(const ksu_uts_view_config& config) {
    ksu_uts_view_config request = config;
    return uts_ksuctl(KSU_IOCTL_SET_UTS_VIEW_CONFIG, &request);
}

int get_uts_view_status(ksu_uts_view_status* status) {
    if (status == nullptr)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    return uts_ksuctl(KSU_IOCTL_GET_UTS_VIEW_STATUS, status);
}

int get_manager_uid() {
    ksu_get_manager_uid_cmd cmd = {};
    if (ksuctl(KSU_IOCTL_GET_MANAGER_UID, &cmd) != 0) {
        return -1;
    }
    return static_cast<int>(cmd.uid);
}

int get_wrapped_fd(int fd) {
    GetWrapperFdCmd cmd = {static_cast<__u32>(fd), 0};
    return ksuctl(KSU_IOCTL_GET_WRAPPER_FD, &cmd);
}

int get_sulog_fd() {
    GetSulogFdCmd cmd = {0};
    return ksuctl(KSU_IOCTL_GET_SULOG_FD, &cmd);
}

int get_su_prompt_fd() {
    GetSuPromptFdCmd cmd = {0};
    return ksuctl(KSU_IOCTL_GET_SU_PROMPT_FD, &cmd);
}

uint32_t mark_get(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_GET, pid, 0};
    ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
    return cmd.result;
}

int mark_set(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_MARK, pid, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int mark_unset(int32_t pid) {
    ManageMarkCmd cmd = {KSU_MARK_UNMARK, pid, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int mark_refresh() {
    ManageMarkCmd cmd = {KSU_MARK_REFRESH, 0, 0};
    return ksuctl(KSU_IOCTL_MANAGE_MARK, &cmd);
}

int nuke_ext4_sysfs(const std::string& mnt) {
    NukeExt4SysfsCmd cmd = {reinterpret_cast<uint64_t>(mnt.c_str())};
    return ksuctl(KSU_IOCTL_NUKE_EXT4_SYSFS, &cmd);
}

int set_init_pgrp() {
    return ksuctl(KSU_IOCTL_SET_INIT_PGRP, nullptr);
}

int set_dynamic_managers(const std::vector<DynamicManagerSign>& signs) {
    DynamicManagerCmd cmd = {static_cast<uint32_t>(signs.size()),
                             reinterpret_cast<uint64_t>(signs.empty() ? nullptr : signs.data())};
    return ksuctl(KSU_IOCTL_SET_DYNAMIC_MANAGERS, &cmd);
}

int umount_list_wipe() {
    AddTryUmountCmd cmd = {0, 0, KSU_UMOUNT_WIPE};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int umount_list_add(const std::string& path, uint32_t flags) {
    AddTryUmountCmd cmd = {reinterpret_cast<uint64_t>(path.c_str()), flags, KSU_UMOUNT_ADD};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

int umount_list_del(const std::string& path) {
    AddTryUmountCmd cmd = {reinterpret_cast<uint64_t>(path.c_str()), 0, KSU_UMOUNT_DEL};
    return ksuctl(KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

std::optional<std::string> umount_list_list() {
    constexpr size_t kBufSize = 4096;
    std::array<char, kBufSize> buffer{};

    ListTryUmountCmd cmd = {reinterpret_cast<uint64_t>(buffer.data()),
                            static_cast<uint32_t>(buffer.size())};
    const int ret = ksuctl(KSU_IOCTL_LIST_TRY_UMOUNT, &cmd);
    if (ret < 0) {
        return std::nullopt;
    }

    return std::string(buffer.data());
}

}  // namespace ksud
