#include "core_bindings.hpp"
#include "loader_handoff.hpp"
extern "C" bool zygisk_bootstrap_finish_handoff(bool);
#include <cerrno>
#include <new>
#include <yukiplt.h>
#include <yukiplt.hpp>

namespace {
bool fail_next_allocation = false;
yukiplt::OriginalSlot<int (*)()> original;
uintptr_t own_base = 0;
size_t own_span = 0;
int replacement() { return original.load()() + 1; }
extern "C" void yz_jni_entry_0();
extern "C" [[noreturn]] void yz_self_unmap_tail(void *, size_t,
                                                CoreUnmapFunction);
int retire_after_capture(void *) {
  if (!core_runtime_closed()) {
    if (yukiplt_c_trim_idle_storage().code != YUKIPLT_C_SUCCESS ||
        !yukizygisk::handoff::release_dependencies())
      return -1;
  }
  const int status = core_finish_runtime_retirement();
  if (!core_runtime_closed())
    return -2;
  yz_self_unmap_tail(reinterpret_cast<void *>(own_base), own_span,
                     status == 0 ? core_retirement_unmap() : nullptr);
}
} // namespace
#if defined(__LP64__)
extern "C" void *__real__Znwm(size_t);
extern "C" __attribute__((visibility("hidden"))) void *
__wrap__Znwm(size_t bytes) {
  if (fail_next_allocation) {
    fail_next_allocation = false;
    throw std::bad_alloc{};
  }
  return __real__Znwm(bytes);
}
#else
extern "C" void *__real__Znwj(size_t);
extern "C" __attribute__((visibility("hidden"))) void *
__wrap__Znwj(size_t bytes) {
  if (fail_next_allocation) {
    fail_next_allocation = false;
    throw std::bad_alloc{};
  }
  return __real__Znwj(bytes);
}
#endif
extern "C" __attribute__((visibility("hidden"))) void *g_yz_jni_targets[10]{};
extern "C" __attribute__((visibility("default"))) int
anonymous_hook_check(int (*call)(), bool exercise_oom) {
  auto images = yukiplt::find_images(
      {yukiplt::ImageMatch::kBasename, "libanon_target.so", 0});
  if (!images || images.value().size() != 1 || call() != 17)
    return 1;
  const yukiplt::HookRequest request{"fixture_value",
                                     reinterpret_cast<void *>(replacement),
                                     original.output_address()};
  yukiplt::OperationDiagnostic diagnostic;
  if (exercise_oom) {
    fail_next_allocation = true;
    auto failed = yukiplt::hook_batch_diagnosed_named(
        images.value()[0], &request, 1, nullptr, &diagnostic);
    if (failed.error.code != yukiplt::ErrorCode::kOutOfMemory ||
        failed.handle.valid() || original.load() != nullptr || call() != 17)
      return 2;
  }
  auto installed = yukiplt::hook_batch_diagnosed_named(
      images.value()[0], &request, 1, nullptr, &diagnostic);
  if (!installed.ok() || call() != 18)
    return 3;
  if (!yukiplt::unhook_diagnosed(installed.handle, nullptr, 0, &diagnostic)
           .ok() ||
      !diagnostic.handle_consumed || call() != 17)
    return 4;
  if (exercise_oom && !core_private_tsd_used())
    return 5;
  return 0;
}
extern "C" __attribute__((visibility("default"))) bool anonymous_close_stage() {
  return zygisk_bootstrap_finish_handoff(true) &&
         yukizygisk::handoff::close_loader_reference();
}
extern "C" __attribute__((visibility("default"))) void *
anonymous_close_entry(uintptr_t base, size_t span) {
  if (!core_runtime_profile_ready(base, span))
    return nullptr;
  own_base = base;
  own_span = span;
  g_yz_jni_targets[0] = reinterpret_cast<void *>(retire_after_capture);
  return reinterpret_cast<void *>(yz_jni_entry_0);
}
