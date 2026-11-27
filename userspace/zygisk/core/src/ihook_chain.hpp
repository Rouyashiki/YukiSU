#pragma once

#include "ihook_pool.hpp"
#include "inline_hook.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <type_traits>

namespace yuki::ihook {

namespace chain_detail {

[[nodiscard]] inline bool alloc_relay_stub(uintptr_t slot_target_addr,
                                           bool thumb_callable, void **stub_out,
                                           bool *pooled_out,
                                           bool allow_mprotect_reuse) {
  if (stub_out == nullptr || pooled_out == nullptr)
    return false;
  *stub_out = nullptr;
  *pooled_out = false;
#if defined(__aarch64__)
  (void)thumb_callable;
  uint8_t code[24]{};
  const uint32_t bti_jc = 0xd50324dfU;
  const uint32_t ldr_lit = 0x58000070U; // ldr x16, #12 (+16)
  const uint32_t ldr_ind = 0xc8dffe10U; // ldar x16, [x16]
  const uint32_t br_x16 = 0xd61f0200U;  // br x16
  const uint64_t ptr = static_cast<uint64_t>(slot_target_addr);
  memcpy(code + 0U, &bti_jc, sizeof(bti_jc));
  memcpy(code + 4U, &ldr_lit, sizeof(ldr_lit));
  memcpy(code + 8U, &ldr_ind, sizeof(ldr_ind));
  memcpy(code + 12U, &br_x16, sizeof(br_x16));
  memcpy(code + 16U, &ptr, sizeof(ptr));
  IslandSlot slot = island_pool().alloc_any(
      trampoline_protection(), [](uintptr_t hint) { return (hint & 7U) == 0; },
      allow_mprotect_reuse);
  if (!slot || !island_pool().commit_slot(slot, code, sizeof(code)))
    return false;
  *stub_out = slot.release(pooled_out);
  return true;
#elif defined(__arm__)
  uint8_t code[20]{};
  if (thumb_callable) {
    // 0: ldr.w r12, [pc, #12]; 4: ldr.w r12, [r12, #0]; 8: dmb ish;
    // 12: bx r12; 14: nop; 16: .word ptr
    const uint16_t hw[8] = {0xf8dfU, 0xc00cU, 0xf8dcU, 0xc000U,
                            0xf3bfU, 0x8f5bU, 0x4760U, 0xbf00U};
    memcpy(code, hw, sizeof(hw));
    const uint32_t ptr = static_cast<uint32_t>(slot_target_addr);
    memcpy(code + 16U, &ptr, sizeof(ptr));
  } else {
    // 0: ldr r12, [pc, #8]; 4: ldr r12, [r12]; 8: dmb ish; 12: bx r12; 16:
    // .word ptr
    const uint32_t insns[4] = {0xe59fc008U, 0xe59cc000U, 0xf57ff05bU,
                               0xe12fff1cU};
    memcpy(code, insns, sizeof(insns));
    const uint32_t ptr = static_cast<uint32_t>(slot_target_addr);
    memcpy(code + 16U, &ptr, sizeof(ptr));
  }
  IslandSlot slot = island_pool().alloc_any(
      PROT_READ | PROT_EXEC, [](uintptr_t hint) { return (hint & 3U) == 0; },
      allow_mprotect_reuse);
  if (!slot || !island_pool().commit_slot(slot, code, sizeof(code)))
    return false;
  void *raw = slot.release(pooled_out);
  *stub_out =
      thumb_callable
          ? reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(raw) | 1U)
          : raw;
  return true;
#else
  (void)slot_target_addr;
  (void)thumb_callable;
  (void)allow_mprotect_reuse;
  return false;
#endif
}

inline void retire_relay_stub(void *stub, bool pooled, bool defer) {
  if (stub == nullptr)
    return;
  void *raw = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stub) &
                                       ~uintptr_t{1});
  island_pool().retire_slot(raw, pooled, defer);
}

} // namespace chain_detail

struct HookNode {
  const void *owner = nullptr;
  std::atomic<uintptr_t> next_target{0};
  void *next_stub = nullptr;
  bool next_stub_pooled = false;
  HookNode *next = nullptr;
};

struct HookChain {
  void *target = nullptr;
  Hook hook{};
  void *trampoline = nullptr;
  std::atomic<uintptr_t> head_target{0};
  void *entry_stub = nullptr;
  bool entry_stub_pooled = false;
  HookNode *head = nullptr;
  HookChain *next = nullptr;
};

class HookChainManager {
public:
  [[nodiscard]] bool hook(void *target, void *handler, void **original_out,
                          bool prefer_relative = true,
                          InstallOptions options = {},
                          const char **error_out = nullptr,
                          const void *owner = nullptr) {
    const auto fail = [error_out](const char *msg) {
      if (error_out != nullptr)
        *error_out = msg;
      return false;
    };
    if (error_out != nullptr)
      *error_out = nullptr;
    if (target == nullptr || handler == nullptr)
      return fail("invalid_input");

    pthread_mutex_lock(&mutex_);

    const bool single_thread = single_threaded_process();
    const uintptr_t requested =
        reinterpret_cast<uintptr_t>(target) & ~uintptr_t{1};
#if defined(__arm__)
    const bool thumb_callable = (reinterpret_cast<uintptr_t>(target) & 1U) != 0;
#else
    const bool thumb_callable = false;
#endif

    HookChain *exact_chain = nullptr;
    size_t max_patch_size = sizeof(void *) == 8 ? 20U : 10U;
    for (HookChain *c = chains_; c != nullptr; c = c->next) {
      if (c->target == target) {
        exact_chain = c;
        continue;
      }
      const uintptr_t existing =
          (c->hook.target != nullptr
               ? reinterpret_cast<uintptr_t>(c->hook.target)
               : reinterpret_cast<uintptr_t>(c->target)) &
          ~uintptr_t{1};
      if (hook_ranges_overlap(requested, 4U, existing, c->hook.patched_size)) {
        pthread_mutex_unlock(&mutex_);
        return fail("overlapping_target");
      }
      if (existing > requested && existing - requested < max_patch_size)
        max_patch_size = existing - requested;
    }

    if (exact_chain != nullptr) {
      auto *node = new (std::nothrow) HookNode();
      if (node == nullptr) {
        pthread_mutex_unlock(&mutex_);
        return fail("out_of_memory");
      }
      node->owner = owner != nullptr ? owner : handler;
      node->next_target.store(
          exact_chain->head_target.load(std::memory_order_relaxed),
          std::memory_order_relaxed);
      if (!chain_detail::alloc_relay_stub(
              reinterpret_cast<uintptr_t>(&node->next_target), thumb_callable,
              &node->next_stub, &node->next_stub_pooled, single_thread)) {
        delete node;
        pthread_mutex_unlock(&mutex_);
        return fail("no_trampoline_mapping");
      }
      node->next = exact_chain->head;
      exact_chain->head = node;
      if (original_out != nullptr)
        __atomic_store_n(original_out, node->next_stub, __ATOMIC_RELEASE);
      exact_chain->head_target.store(reinterpret_cast<uintptr_t>(handler),
                                     std::memory_order_release);
      pthread_mutex_unlock(&mutex_);
      return true;
    }

    auto *chain = new (std::nothrow) HookChain();
    auto *node = new (std::nothrow) HookNode();
    if (chain == nullptr || node == nullptr) {
      delete chain;
      delete node;
      pthread_mutex_unlock(&mutex_);
      return fail("out_of_memory");
    }
    chain->target = target;
    chain->head_target.store(0U, std::memory_order_relaxed);
    if (!chain_detail::alloc_relay_stub(
            reinterpret_cast<uintptr_t>(&chain->head_target), thumb_callable,
            &chain->entry_stub, &chain->entry_stub_pooled, single_thread)) {
      delete chain;
      delete node;
      pthread_mutex_unlock(&mutex_);
      return fail("no_trampoline_mapping");
    }
    if (!chain_detail::alloc_relay_stub(
            reinterpret_cast<uintptr_t>(&node->next_target), thumb_callable,
            &node->next_stub, &node->next_stub_pooled, single_thread)) {
      chain_detail::retire_relay_stub(chain->entry_stub,
                                      chain->entry_stub_pooled, false);
      delete chain;
      delete node;
      pthread_mutex_unlock(&mutex_);
      return fail("no_trampoline_mapping");
    }

    InstallOptions opts = options;
    opts.pre_publish_trampoline_slot = &node->next_target;
    opts.pre_publish_entry_slot = &chain->head_target;

#if defined(__aarch64__)
    a64::Error reason = a64::Error::None;
    void *orig = install(target, chain->entry_stub, &chain->hook,
                         prefer_relative, false, &reason, max_patch_size, opts);
    const char *err_str = a64::error_name(reason);
#elif defined(__arm__)
    arm32::Error reason = arm32::Error::None;
    void *orig = install(target, chain->entry_stub, &chain->hook,
                         prefer_relative, false, &reason, max_patch_size, opts);
    const char *err_str = arm32::error_name(reason);
#else
    void *orig =
        install(target, chain->entry_stub, &chain->hook, prefer_relative);
    const char *err_str = "unsupported";
#endif
    if (orig == nullptr) {
      chain_detail::retire_relay_stub(node->next_stub, node->next_stub_pooled,
                                      false);
      chain_detail::retire_relay_stub(chain->entry_stub,
                                      chain->entry_stub_pooled, false);
      delete chain;
      delete node;
      pthread_mutex_unlock(&mutex_);
      return fail(err_str);
    }

    node->owner = owner != nullptr ? owner : handler;
    node->next_target.store(reinterpret_cast<uintptr_t>(orig),
                            std::memory_order_release);
    chain->trampoline = orig;
    chain->head = node;
    chain->next = chains_;
    chains_ = chain;
    if (original_out != nullptr)
      __atomic_store_n(original_out, node->next_stub, __ATOMIC_RELEASE);
    chain->head_target.store(reinterpret_cast<uintptr_t>(handler),
                             std::memory_order_release);
    pthread_mutex_unlock(&mutex_);
    return true;
  }

  [[nodiscard]] bool unhook(void *target, const void *owner = nullptr) {
    if (target == nullptr)
      return false;
    pthread_mutex_lock(&mutex_);
    HookChain *prev_chain = nullptr;
    HookChain *chain = chains_;
    while (chain != nullptr && chain->target != target) {
      prev_chain = chain;
      chain = chain->next;
    }
    if (chain == nullptr || chain->head == nullptr) {
      pthread_mutex_unlock(&mutex_);
      return false;
    }

    HookNode *prev_node = nullptr;
    HookNode *node = chain->head;
    if (owner == nullptr && node->next != nullptr) {
      pthread_mutex_unlock(&mutex_);
      return false;
    }
    if (owner != nullptr) {
      while (node != nullptr && node->owner != owner) {
        prev_node = node;
        node = node->next;
      }
      if (node == nullptr) {
        pthread_mutex_unlock(&mutex_);
        return false;
      }
    }

    // Multiple nodes remain: atomically bypass removed node without touching
    // target bytes.
    if (chain->head->next != nullptr) {
      const uintptr_t next_fn =
          node->next_target.load(std::memory_order_relaxed);
      if (prev_node == nullptr) {
        chain->head = node->next;
        chain->head_target.store(next_fn, std::memory_order_release);
      } else {
        prev_node->next = node->next;
        prev_node->next_target.store(next_fn, std::memory_order_release);
      }
      retire_locked(nullptr, node);
      pthread_mutex_unlock(&mutex_);
      return true;
    }

    // Last node on chain: fall back head_target to trampoline before restoring
    // prologue.
    const uintptr_t saved_head =
        chain->head_target.load(std::memory_order_relaxed);
    chain->head_target.store(reinterpret_cast<uintptr_t>(chain->trampoline),
                             std::memory_order_release);
    if (!uninstall(&chain->hook)) {
      chain->head_target.store(saved_head, std::memory_order_release);
      pthread_mutex_unlock(&mutex_);
      return false;
    }
    if (prev_chain == nullptr)
      chains_ = chain->next;
    else
      prev_chain->next = chain->next;
    retire_locked(chain, node);
    pthread_mutex_unlock(&mutex_);
    return true;
  }

  void reclaim_quiescent() {
    pthread_mutex_lock(&mutex_);
    if (chains_ == nullptr) {
      while (retired_nodes_ != nullptr) {
        HookNode *node = retired_nodes_;
        retired_nodes_ = node->next;
        chain_detail::retire_relay_stub(node->next_stub, node->next_stub_pooled,
                                        true);
        delete node;
      }
      while (retired_chains_ != nullptr) {
        HookChain *chain = retired_chains_;
        retired_chains_ = chain->next;
        chain_detail::retire_relay_stub(chain->entry_stub,
                                        chain->entry_stub_pooled, true);
        delete chain;
      }
    }
    pthread_mutex_unlock(&mutex_);
  }

  [[nodiscard]] bool idle() {
    pthread_mutex_lock(&mutex_);
    const bool empty = chains_ == nullptr && retired_nodes_ == nullptr &&
                       retired_chains_ == nullptr;
    pthread_mutex_unlock(&mutex_);
    return empty;
  }

private:
  void retire_locked(HookChain *chain, HookNode *node) {
    node->next = retired_nodes_;
    retired_nodes_ = node;
    if (chain != nullptr) {
      chain->next = retired_chains_;
      retired_chains_ = chain;
    }
  }

  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  HookChain *chains_ = nullptr;
  HookChain *retired_chains_ = nullptr;
  HookNode *retired_nodes_ = nullptr;
};

static_assert(std::is_trivially_destructible_v<HookChainManager>,
              "HookChainManager must remain trivially destructible");

[[nodiscard]] inline HookChainManager &chain_manager() {
  static HookChainManager manager;
  return manager;
}

} // namespace yuki::ihook
