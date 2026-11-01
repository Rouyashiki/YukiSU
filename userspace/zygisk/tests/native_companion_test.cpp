#include "native_module_binding.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t kHandleMagic = 0x595a4e31u;
#include "native_companion_types.inc"
std::vector<ModuleHandle *> g_loaded_modules;
uint32_t matching_index = 0;
bool companion_available = false;
bool specialized_child = false;
bool pause_snapshot = false;
bool pause_ipc = false;
std::atomic<bool> snapshot_entered{false}, resume_snapshot{false};
std::atomic<bool> ipc_entered{false}, resume_ipc{false};
uint32_t requested_index = UINT32_MAX;
unsigned requests = 0;

void wait_for(const std::atomic<bool> &flag) {
  while (!flag.load(std::memory_order_acquire))
    std::this_thread::yield();
}

enum class ZdRequest { ConnectNativeCompanion };
namespace zygiskd {
struct NativeModuleInfo {
  std::string module_id;
  bool has_companion = false;
};
} // namespace zygiskd
} // namespace

namespace yukizygisk::hyos {
bool available() { return false; }
bool in_specialized_child() {
  if (pause_snapshot) {
    snapshot_entered.store(true, std::memory_order_release);
    wait_for(resume_snapshot);
  }
  return specialized_child;
}
} // namespace yukizygisk::hyos

namespace yukizygisk::plt_lifecycle {
bool dispatch_allowed() { return true; }
} // namespace yukizygisk::plt_lifecycle

namespace {
bool request_native_module_count(uint32_t *count, bool) {
  *count = 8;
  return true;
}
bool request_native_info(uint32_t index, zygiskd::NativeModuleInfo *info) {
  info->module_id = index == matching_index ? "module" : "other";
  info->has_companion = companion_available;
  return true;
}
std::string module_id_of(const zygiskd::NativeModuleInfo &info) {
  return info.module_id;
}
bool report_native_injection(uint32_t) { return false; }
int request_fd(ZdRequest request, uint32_t index) {
  assert(request == ZdRequest::ConnectNativeCompanion);
  requested_index = index;
  ++requests;
  if (pause_ipc) {
    ipc_entered.store(true, std::memory_order_release);
    wait_for(resume_ipc);
  }
  return 17;
}
int request_fd_from_hyos_session(ZdRequest request, uint32_t index) {
  return request_fd(request, index);
}
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
#include "native_companion_methods.inc"

void publish(uint32_t index, bool available) {
  matching_index = index;
  companion_available = available;
  assert(!sync_early_native_reports_once());
}

void check_companion_snapshot(bool child) {
  ModuleHandle module;
  module.module_id = "module";
  module.early = true;
  g_loaded_modules = {&module};
  specialized_child = child;
  requests = 0;
  pause_snapshot = false;
  pause_ipc = false;
  snapshot_entered.store(false);
  resume_snapshot.store(false);
  ipc_entered.store(false);
  resume_ipc.store(false);
  assert(api_connect_companion(&module) == -1 && requests == 0);

  publish(1, true);
  pause_snapshot = true;
  int result = -1;
  std::thread reader([&] { result = api_connect_companion(&module); });
  wait_for(snapshot_entered);
  publish(2, false);
  resume_snapshot.store(true, std::memory_order_release);
  reader.join();
  assert(result == 17 && requests == 1 && requested_index == 1);
  assert(api_connect_companion(&module) == -1 && requests == 1);

  publish(3, true);
  pause_snapshot = false;
  pause_ipc = true;
  reader = std::thread([&] { result = api_connect_companion(&module); });
  wait_for(ipc_entered);
  publish(4, false);
  resume_ipc.store(true, std::memory_order_release);
  reader.join();
  assert(result == 17 && requests == 2 && requested_index == 3);
  assert(api_connect_companion(&module) == -1 && requests == 2);
  g_loaded_modules.clear();
}
} // namespace

int main() {
  alarm(10);
  check_companion_snapshot(false);
  check_companion_snapshot(true);
  puts("PASS: native companion snapshot across deferred publication and IPC");
}
