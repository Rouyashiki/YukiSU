#pragma once

#include <atomic>
#include <cstdint>

namespace yukizygisk {

class NativeModuleBinding {
public:
  struct Snapshot {
    uint32_t index;
    bool has_companion;
  };

  void publish(uint32_t index, bool has_companion) noexcept {
    binding_.store(uint64_t{index} | (uint64_t{has_companion} << 32),
                   std::memory_order_release);
  }

  Snapshot load() const noexcept {
    const uint64_t binding = binding_.load(std::memory_order_acquire);
    return {static_cast<uint32_t>(binding), (binding >> 32) != 0};
  }

private:
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  // Readers retain one daemon binding across availability checks and IPC.
  std::atomic<uint64_t> binding_{UINT32_MAX};
};

} // namespace yukizygisk
