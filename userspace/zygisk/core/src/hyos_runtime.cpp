#include "hyos_runtime.hpp"
#include "plt_lifecycle.hpp"
#include <yukiplt.hpp>

#include "log.hpp"

#include "process_maps.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace yukizygisk::hyos {
namespace {

constexpr char kSpawnerPath[] = "/system_ext/bin/hyos_spawner";
constexpr size_t kValueCapacity = 512;

using ForkFn = pid_t (*)();
using SetContextFn = int (*)(uid_t uid, bool is_system_server,
                             const char *se_info, const char *package_name);

#if ZYGISK_NEXT_API_VERSION != 4 || ZYGISK_NEXT_HYOS_API_VERSION != 1
#error "Unsupported ZygiskNext protocol version"
#endif
static_assert(ZN_RUNTIME_HYOS == 1);
static_assert(sizeof(ZygiskNextAPI) == 10 * sizeof(void *));
static_assert(offsetof(ZygiskNextAPI, getRuntime) == 9 * sizeof(void *));
static_assert(offsetof(ZygiskNextRuntime, type) == 0);
static_assert(offsetof(ZygiskNextRuntime, api_version) == sizeof(int));
static_assert(offsetof(ZygiskNextRuntime, registerModule) ==
              (((2 * sizeof(int)) + alignof(void *) - 1) &
               ~(alignof(void *) - 1)));
static_assert(sizeof(ZygiskNextRuntime) ==
              offsetof(ZygiskNextRuntime, registerModule) + sizeof(void *));
static_assert(offsetof(ZygiskNextHyosModule, onAppSpecialized) ==
              ((sizeof(int) + alignof(void *) - 1) & ~(alignof(void *) - 1)));
static_assert(sizeof(ZygiskNextHyosModule) ==
              offsetof(ZygiskNextHyosModule, onAppSpecialized) +
                  sizeof(void *));
static_assert(sizeof(ZnHyosAppSpecializeArgs) == 3 * sizeof(void *));
struct RegisteredModule {
  ZygiskNextHyosModule module;
  const uint32_t *module_index;
};

std::vector<RegisteredModule> g_modules;

yukiplt::NamedImage g_target_image{};
yukiplt::HookHandle g_hook_handle;
yukiplt::OperationDiagnostic g_install_diagnostic;
yukiplt::OperationDiagnostic g_restore_diagnostic;
bool g_recovery = false;
bool g_available = false;
bool g_registration_open = false;
bool g_hooks_ready = false;

yukiplt::OriginalSlot<ForkFn> g_original_fork;
yukiplt::OriginalSlot<SetContextFn> g_original_setcontext;
OpenControlSessionFn g_open_control_session = nullptr;
ReportCallbackFn g_report_callback = nullptr;
void (*g_recover_plt)() = nullptr;
const uint32_t *g_registering_module_index = nullptr;
int g_child_control_session = -1;
std::atomic_flag g_child_control_lock = ATOMIC_FLAG_INIT;
std::atomic_flag g_fork_lock = ATOMIC_FLAG_INIT;
bool g_in_specialized_child = false;

bool g_child_ready = false;
bool g_child_dispatched = false;
char g_process_name[kValueCapacity]{};
char g_package_name[kValueCapacity]{};
char g_se_info[kValueCapacity]{};

bool copy_value(char *destination, size_t capacity, const char *source) {
  if (source == nullptr || capacity == 0)
    return false;
  size_t length = 0;
  while (length < capacity && source[length] != '\0')
    ++length;
  if (length == capacity)
    return false;
  memcpy(destination, source, length + 1);
  return true;
}

bool read_process_name() {
  int fd;
  do {
    fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return false;

  size_t length = 0;
  bool complete = false;
  while (length + 1 < sizeof(g_process_name)) {
    char value = '\0';
    ssize_t result;
    do {
      result = read(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    if (result != sizeof(value))
      break;
    if (value == '\0') {
      complete = length != 0;
      break;
    }
    g_process_name[length++] = value;
  }
  close(fd);
  g_process_name[length] = '\0';
  return complete;
}

void reset_child_state(bool control_session_ready) {
  g_child_control_lock.clear(std::memory_order_release);
  g_in_specialized_child = true;
  g_child_ready = control_session_ready;
  g_child_dispatched = false;
  g_process_name[0] = '\0';
  g_package_name[0] = '\0';
  g_se_info[0] = '\0';
}

void dispatch_specialized(const char *se_info, const char *package_name) {
  if (!g_hooks_ready || !g_child_ready || g_child_dispatched)
    return;
  g_child_dispatched = true;

  if (!copy_value(g_package_name, sizeof(g_package_name), package_name) ||
      !copy_value(g_se_info, sizeof(g_se_info), se_info) ||
      !read_process_name())
    return;

  const ZnHyosAppSpecializeArgs args = {
      .process_name = g_process_name,
      .package_name = g_package_name,
      .se_info = g_se_info,
  };
  for (const auto &entry : g_modules) {
    entry.module.onAppSpecialized(&args);
    if (g_recover_plt != nullptr)
      g_recover_plt();
    if (g_report_callback != nullptr && entry.module_index != nullptr &&
        *entry.module_index != UINT32_MAX)
      (void)g_report_callback(*entry.module_index);
  }
}

pid_t replacement_fork() {
  if (g_original_fork.load() == nullptr)
    return -1;
  if (g_fork_lock.test_and_set(std::memory_order_acquire)) {
    errno = EBUSY;
    return -1;
  }
  const int control_session =
      g_open_control_session != nullptr ? g_open_control_session() : -1;
  const pid_t result =
      yukizygisk::plt_lifecycle::fork_process(g_original_fork.load());
  const int fork_error = errno;
  if (result == 0) {
    g_fork_lock.clear(std::memory_order_release);
    if (g_child_control_session >= 0)
      close(g_child_control_session);
    g_child_control_session = control_session;
    reset_child_state(control_session >= 0);
  } else if (control_session >= 0) {
    close(control_session);
  }
  if (result != 0)
    g_fork_lock.clear(std::memory_order_release);
  errno = fork_error;
  return result;
}

int replacement_setcontext(uid_t uid, bool is_system_server,
                           const char *se_info, const char *package_name) {
  if (g_original_setcontext.load() == nullptr)
    return -1;
  const int result = g_original_setcontext.load()(uid, is_system_server,
                                                  se_info, package_name);
  if (result == 0)
    dispatch_specialized(se_info, package_name);
  return result;
}

int register_module(const void *module_pointer) {
  if (!g_registration_open || module_pointer == nullptr)
    return ZN_FAILED;
  const auto *module =
      static_cast<const ZygiskNextHyosModule *>(module_pointer);
  if (module->target_api_version != ZYGISK_NEXT_HYOS_API_VERSION ||
      module->onAppSpecialized == nullptr)
    return ZN_FAILED;

  g_modules.push_back({*module, g_registering_module_index});
  return ZN_SUCCESS;
}

const ZygiskNextRuntime g_runtime = {
    .type = ZN_RUNTIME_HYOS,
    .api_version = ZYGISK_NEXT_HYOS_API_VERSION,
    .registerModule = register_module,
};

bool restore_runtime_hooks() {
  if (!g_hook_handle.valid())
    return true;
  yukiplt::SlotReport slots[2];
  const auto error =
      yukiplt::unhook_diagnosed(g_hook_handle, slots, 2, &g_restore_diagnostic);
  yukizygisk::plt_lifecycle::set_recovery(g_recovery, g_hook_handle.valid());
  if (!error.ok())
    ZLOGE("HyperOS restoration: %s cause=%s errno=%d consumed=%d",
          yukiplt::error_name(error.code),
          yukiplt::error_cause_name(g_restore_diagnostic.cause),
          error.sys_errno, g_restore_diagnostic.handle_consumed);
  return !g_hook_handle.valid();
}

} // namespace

bool initialize(OpenControlSessionFn open_control_session,
                ReportCallbackFn report_callback, void (*recover_plt)()) {
#ifndef __aarch64__
  (void)open_control_session;
  (void)report_callback;
  (void)recover_plt;
  return false;
#else
  g_available = false;
  g_registration_open = false;
  g_hooks_ready = false;
  g_modules.clear();
  g_open_control_session = open_control_session;
  g_report_callback = report_callback;
  g_recover_plt = recover_plt;
  g_registering_module_index = nullptr;

  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation)
    return false;
  try {
    auto images = yukiplt::find_images(
        {yukiplt::ImageMatch::kMainExecutable, nullptr, 0});
    if (!images || images.value().size() != 1)
      return false;
    char executable[512];
    const ssize_t length =
        readlink("/proc/self/exe", executable, sizeof(executable));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(executable) ||
        std::string_view(executable, static_cast<size_t>(length)) !=
            kSpawnerPath)
      return false;
    g_target_image = std::move(images.value().front());
    g_available = true;
    g_registration_open = true;
    ZLOGI("HyperOS Rust Runtime detected at %p",
          reinterpret_cast<void *>(g_target_image.image.load_bias));
    return true;
  } catch (...) {
    ZLOGE("HyperOS discovery failed");
    return false;
  }
#endif
}

bool available() { return g_available; }

const ZygiskNextRuntime *runtime() {
  return g_available ? &g_runtime : nullptr;
}

size_t registered_module_count() { return g_modules.size(); }

bool install_registered_hooks() {
  g_registration_open = false;
  const size_t count = registered_module_count();
  if (!g_available || count == 0)
    return true;
  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation)
    return false;
  if (g_hooks_ready)
    return true;
  if (g_hook_handle.valid() ||
      g_install_diagnostic.stage >= yukiplt::OperationStage::kCommit)
    return false;
  const yukiplt::HookRequest requests[] = {
      {"fork", reinterpret_cast<void *>(replacement_fork),
       g_original_fork.output_address()},
      {"selinux_android_setcontext",
       reinterpret_cast<void *>(replacement_setcontext),
       g_original_setcontext.output_address()}};
  yukiplt::SlotReport slots[2];
  auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
      g_target_image, requests, 2, slots, &g_install_diagnostic);
  g_hook_handle = std::move(result.handle);
  yukizygisk::plt_lifecycle::set_recovery(
      g_recovery, !result.ok() && g_hook_handle.valid());
  bool control_ready = false;
  if (result.ok() && g_open_control_session != nullptr) {
    const int session = g_open_control_session();
    control_ready = session >= 0;
    if (session >= 0)
      close(session);
  }
  if (!control_ready) {
    const bool restored = restore_runtime_hooks();
    ZLOGE("HyperOS installation: %s cause=%s errno=%d restored=%d",
          yukiplt::error_name(result.error.code),
          yukiplt::error_cause_name(g_install_diagnostic.cause),
          result.error.sys_errno, restored);
    return false;
  }
  g_hooks_ready = true;
  ZLOGI("HyperOS Rust Runtime hooks installed for %zu module(s)", count);
  return true;
}

bool original_output_allowed(void **output) {
  if (output == g_original_fork.output_address() ||
      output == g_original_setcontext.output_address())
    return false;
  if (!g_hook_handle.valid() || output == nullptr)
    return true;
  yukiplt::SlotReport slots[2];
  size_t count = 0;
  if (!yukiplt::query(g_hook_handle, slots, 2, &count).ok() || count != 2)
    return false;
  for (const auto &slot : slots)
    if (slot.address == reinterpret_cast<uintptr_t>(output))
      return false;
  return true;
}

int child_control_session() { return g_child_control_session; }

bool in_specialized_child() { return g_in_specialized_child; }

void lock_child_control_session() {
  while (g_child_control_lock.test_and_set(std::memory_order_acquire)) {
  }
}

void unlock_child_control_session() {
  g_child_control_lock.clear(std::memory_order_release);
}

void invalidate_child_control_session() {
  if (g_child_control_session >= 0)
    close(g_child_control_session);
  g_child_control_session = -1;
  g_child_ready = false;
}

void begin_module_registration(const uint32_t *module_index) {
  g_registering_module_index = module_index;
}

void end_module_registration() { g_registering_module_index = nullptr; }

} // namespace yukizygisk::hyos
