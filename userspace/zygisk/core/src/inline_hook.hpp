#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif // #ifndef MAP_FIXED_NOREPLACE

namespace yuki::ihook {

enum class UnhookMode : uint8_t {
  RestoreBytes,
  DiscardCowPages,
};

inline bool discard_cow_patch_pages(uintptr_t address, size_t patch_size) {
  if (patch_size == 0 ||
      address > UINTPTR_MAX - static_cast<uintptr_t>(patch_size - 1))
    return false;
  const auto page_size = static_cast<uintptr_t>(getpagesize());
  const uintptr_t first_page = address - (address % page_size);
  const uintptr_t last = address + patch_size - 1;
  const uintptr_t last_page = last - (last % page_size);
  if (last_page > UINTPTR_MAX - page_size)
    return false;
  return madvise(reinterpret_cast<void *>(first_page),
                 static_cast<size_t>(last_page - first_page + page_size),
                 MADV_DONTNEED) == 0;
}

} // namespace yuki::ihook

#if defined(__aarch64__)
extern "C" {
extern uint8_t yz_cap_tmpl[];
extern uint8_t yz_cap_tmpl_ctx[];
extern uint8_t yz_cap_tmpl_wrap[];
extern uint8_t yz_cap_tmpl_end[];
extern uint64_t g_yz_ret_ctx[];
/* COW patch through zygiskd/kernel. */
bool yz_patch_text(uintptr_t addr, const void *bytes, unsigned int len);
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr; // patched function start
  uint32_t saved[2] = {};     // original 2 instructions (paciasp + sub-sp)
  void *trampoline =
      nullptr; // R-X near page: capture stub + [2 insns + B back]
  bool active = false;
};

/* PC-relative instructions cannot be copied. */
inline bool is_pcrel(uint32_t i) {
  if ((i & 0x1F000000u) == 0x10000000u) // ADR / ADRP
    return true;
  if ((i & 0x7C000000u) == 0x14000000u) // B / BL
    return true;
  if ((i & 0xFF000010u) == 0x54000000u) // B.cond
    return true;
  if ((i & 0x7E000000u) == 0x34000000u) // CBZ / CBNZ
    return true;
  if ((i & 0x7E000000u) == 0x36000000u) // TBZ / TBNZ
    return true;
  if ((i & 0x3B000000u) == 0x18000000u) // LDR/LDRSW/PRFM literal
    return true;
  return false;
}

/* Encode a direct branch within +-128MB. */
inline uint32_t enc_b(uintptr_t from, uintptr_t to) {
  int64_t off = static_cast<int64_t>(to) - static_cast<int64_t>(from);
  return 0x14000000u | (static_cast<uint32_t>(off >> 2) & 0x03FFFFFFu);
}

struct ExecPage {
  void *addr = nullptr;
};

/* Allocate a reachable trampoline page. */
inline ExecPage alloc_near(uintptr_t target) {
  const uintptr_t reach = 0x7800000; // ~120MB, comfortably under B's +-128MB
  uintptr_t base = target & ~static_cast<uintptr_t>(0xFFF);
  for (uintptr_t off = 0x10000; off <= reach; off += 0x10000) {
    for (int up = 0; up < 2; ++up) {
      uintptr_t hint = up ? base + off : base - off;
      void *p = mmap(reinterpret_cast<void *>(hint), 0x1000,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint)
        return {p};
      if (p != MAP_FAILED)
        munmap(p, 0x1000);
    }
  }
  return {};
}

/* Patch target prologue and return call-original trampoline. */
inline void *install(void *target, void *replacement, Hook *out, bool = false,
                     bool capture_return = false) {
  auto *t = reinterpret_cast<uint32_t *>(target);
  for (int i = 0; i < 2; ++i)
    if (is_pcrel(t[i]))
      return nullptr; // un-relocatable prologue -> bail

  const size_t cap_size = static_cast<size_t>(yz_cap_tmpl_end - yz_cap_tmpl);
  const size_t ctx_off = static_cast<size_t>(yz_cap_tmpl_ctx - yz_cap_tmpl);
  const size_t wrap_off = static_cast<size_t>(yz_cap_tmpl_wrap - yz_cap_tmpl);
  const size_t co_off = (cap_size + 3U) & ~static_cast<size_t>(3); // 4-aligned

  // Trampoline must be branch-reachable.
  ExecPage page = alloc_near(reinterpret_cast<uintptr_t>(target));
  if (page.addr == nullptr)
    return nullptr;
  auto *base = reinterpret_cast<uint8_t *>(page.addr);
  auto *mapped_base = reinterpret_cast<uint8_t *>(page.addr);
  // Capture stub.
  memcpy(base, yz_cap_tmpl, cap_size);
  *reinterpret_cast<uint64_t *>(base + ctx_off) =
      capture_return ? reinterpret_cast<uint64_t>(g_yz_ret_ctx) : 0;
  *reinterpret_cast<uint64_t *>(base + wrap_off) =
      reinterpret_cast<uint64_t>(replacement);
  // Call-original trampoline.
  auto *co = reinterpret_cast<uint32_t *>(base + co_off);
  auto *mapped_co = reinterpret_cast<uint32_t *>(mapped_base + co_off);
  for (int i = 0; i < 2; ++i) {
    out->saved[i] = t[i];
    co[i] = t[i];
  }
  co[2] = enc_b(reinterpret_cast<uintptr_t>(mapped_co + 2),
                reinterpret_cast<uintptr_t>(target) + 8);
  __builtin___clear_cache(reinterpret_cast<char *>(page.addr),
                          reinterpret_cast<char *>(mapped_base + co_off + 12));
  if (mprotect(page.addr, 0x1000, PROT_READ | PROT_EXEC) != 0) {
    munmap(page.addr, 0x1000);
    return nullptr;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(page.addr),
                          reinterpret_cast<char *>(mapped_base + co_off + 12));

  // BTI landing pad + direct branch to capture stub.
  uint32_t patch[2] = {
      0xD503245F, // BTI c
      enc_b(reinterpret_cast<uintptr_t>(target) + 4,
            reinterpret_cast<uintptr_t>(page.addr)), // B <capture stub>
  };
  if (!yz_patch_text(reinterpret_cast<uintptr_t>(target), patch,
                     sizeof(patch))) {
    munmap(page.addr, 0x1000);
    return nullptr;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(target),
                          reinterpret_cast<char *>(target) + 8);

  out->target = t;
  out->trampoline = page.addr;
  out->active = true;
  return mapped_co; // wrapper reaches the ORIGINAL native via this trampoline
}

inline bool uninstall(Hook *h, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (!h->active)
    return true;
  const uintptr_t target = reinterpret_cast<uintptr_t>(h->target);
  if (mode == UnhookMode::DiscardCowPages) {
    if (!discard_cow_patch_pages(target, sizeof(h->saved)))
      return false;
  } else if (!yz_patch_text(target, h->saved, sizeof(h->saved))) {
    return false;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(h->target),
                          reinterpret_cast<char *>(h->target) + 8);
  if (h->trampoline != nullptr && munmap(h->trampoline, 0x1000) != 0)
    return false;
  h->trampoline = nullptr;
  h->active = false;
  return true;
}

} // namespace yuki::ihook
#elif defined(__arm__)
extern "C" {
extern uint8_t yz_cap_arm_tmpl[];
extern uint8_t yz_cap_arm_tmpl_ctx[];
extern uint8_t yz_cap_arm_tmpl_wrap[];
extern uint8_t yz_cap_arm_tmpl_end[];
extern uint8_t yz_cap_thumb_tmpl[];
extern uint8_t yz_cap_thumb_tmpl_ctx[];
extern uint8_t yz_cap_thumb_tmpl_wrap[];
extern uint8_t yz_cap_thumb_tmpl_end[];
extern uint32_t g_yz_ret_ctx[];
bool yz_patch_text(uintptr_t addr, const void *bytes, unsigned int len);
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr;
  uint32_t saved[3] = {};
  void *trampoline = nullptr;
  uint8_t patched_size = 0;
  bool active = false;
};

inline bool thumb_is_32bit(uint16_t instruction) {
  uint16_t prefix = instruction >> 11;
  return prefix == 0x1d || prefix == 0x1e || prefix == 0x1f;
}

inline bool thumb_is_pcrel(const uint16_t *instruction, bool wide) {
  uint16_t first = instruction[0];
  if (!wide) {
    if ((first & 0xf800u) == 0x4800u || // LDR literal
        (first & 0xf800u) == 0xa000u || // ADR
        (first & 0xf000u) == 0xd000u || // B.cond / SVC
        (first & 0xf800u) == 0xe000u || // B
        (first & 0xf500u) == 0xb100u)   // CBZ / CBNZ
      return true;
    if ((first & 0xfc00u) == 0x4400u) {
      unsigned int rm = (first >> 3) & 0xf;
      unsigned int rd = (first & 7) | ((first >> 4) & 8);
      return rm == 15 || rd == 15;
    }
    return false;
  }

  uint16_t second = instruction[1];
  if ((first & 0xff7fu) == 0xf85fu || // LDR literal
      (first & 0xfbf0u) == 0xf20fu || // ADR / ADR.W
      (first & 0xfbf0u) == 0xf2afu)   // SUBW from PC
    return true;
  if ((first & 0xf800u) == 0xf000u && (second & 0x8000u) != 0)
    return true; // B.W / BL / BLX and conditional branches
  return false;
}

inline bool arm_is_pcrel(uint32_t instruction) {
  if ((instruction & 0x0ffffff0u) == 0x012fff10u ||
      (instruction & 0x0ffffff0u) == 0x012fff30u)
    return false; // BX / BLX register
  if ((instruction & 0x0e000000u) == 0x0a000000u)
    return true; // B / BL
  unsigned int rn = (instruction >> 16) & 0xf;
  if (rn != 15)
    return false;
  unsigned int group = instruction & 0x0c000000u;
  return group == 0 || group == 0x04000000u || group == 0x0c000000u;
}

inline size_t arm32_copy_size(const void *target, bool thumb,
                              size_t minimum_size) {
  if (!thumb) {
    auto *words = static_cast<const uint32_t *>(target);
    size_t words_needed = (minimum_size + 3U) / 4U;
    for (size_t i = 0; i < words_needed; ++i)
      if (arm_is_pcrel(words[i]))
        return 0;
    return words_needed * 4U;
  }

  auto *half = static_cast<const uint16_t *>(target);
  size_t bytes = 0;
  while (bytes < minimum_size) {
    bool wide = thumb_is_32bit(half[bytes / 2]);
    size_t instruction_size = wide ? 4 : 2;
    if (bytes + instruction_size > 12 || thumb_is_pcrel(half + bytes / 2, wide))
      return 0;
    bytes += instruction_size;
  }
  return bytes;
}

inline size_t absolute_jump_size(uintptr_t instruction_address, bool thumb) {
  return thumb && (instruction_address & 2U) != 0 ? 10U : 8U;
}

inline bool relative_jump_reachable(uintptr_t from, uintptr_t to, bool thumb) {
  int64_t offset =
      static_cast<int64_t>(to) - static_cast<int64_t>(from + (thumb ? 4U : 8U));
  int64_t limit = thumb ? 0x01000000LL : 0x02000000LL;
  unsigned int alignment = thumb ? 2U : 4U;
  return offset >= -limit && offset < limit &&
         (offset & static_cast<int64_t>(alignment - 1U)) == 0;
}

inline void *alloc_arm32_near(uintptr_t target, bool thumb) {
  constexpr uintptr_t kReach = 0x00f00000U;
  constexpr uintptr_t kStep = 0x00010000U;
  constexpr size_t kPageSize = 0x1000U;
  uintptr_t base = target & ~static_cast<uintptr_t>(kPageSize - 1U);

  void *page = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page != MAP_FAILED) {
    if (relative_jump_reachable(target, reinterpret_cast<uintptr_t>(page),
                                thumb))
      return page;
    munmap(page, kPageSize);
  }

  for (uintptr_t offset = kStep; offset <= kReach; offset += kStep) {
    for (unsigned int above = 0; above < 2; ++above) {
      if ((!above && base < offset) || (above && base > UINTPTR_MAX - offset))
        continue;
      uintptr_t hint = above ? base + offset : base - offset;
      page = mmap(reinterpret_cast<void *>(hint), kPageSize,
                  PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (page != MAP_FAILED && reinterpret_cast<uintptr_t>(page) == hint &&
          relative_jump_reachable(target, hint, thumb))
        return page;
      if (page != MAP_FAILED)
        munmap(page, kPageSize);
    }
  }
  return nullptr;
}

inline void emit_relative_jump(uint8_t *where, uintptr_t from, uintptr_t to,
                               bool thumb) {
  int64_t offset =
      static_cast<int64_t>(to) - static_cast<int64_t>(from + (thumb ? 4U : 8U));
  uint32_t encoded = static_cast<uint32_t>(offset);
  if (!thumb) {
    uint32_t branch = 0xea000000u | ((encoded >> 2) & 0x00ffffffu);
    memcpy(where, &branch, sizeof(branch));
    return;
  }

  uint32_t sign = (encoded >> 24) & 1U;
  uint32_t i1 = (encoded >> 23) & 1U;
  uint32_t i2 = (encoded >> 22) & 1U;
  uint32_t j1 = (~i1 ^ sign) & 1U;
  uint32_t j2 = (~i2 ^ sign) & 1U;
  uint16_t branch[] = {
      static_cast<uint16_t>(0xf000u | (sign << 10) |
                            ((encoded >> 12) & 0x03ffu)),
      static_cast<uint16_t>(0x9000u | (j1 << 13) | (j2 << 11) |
                            ((encoded >> 1) & 0x07ffu)),
  };
  memcpy(where, branch, sizeof(branch));
}

inline size_t emit_absolute_jump(uint8_t *where, uintptr_t instruction_address,
                                 uintptr_t destination, bool thumb) {
  if (thumb) {
    bool needs_padding = (instruction_address & 2U) != 0;
    uint16_t jump[] = {
        0xf8dfu, static_cast<uint16_t>(0xf000u | (needs_padding ? 4U : 0U))};
    memcpy(where, jump, sizeof(jump));
    size_t literal_offset = 4;
    if (needs_padding) {
      const uint16_t nop = 0xbf00u;
      memcpy(where + literal_offset, &nop, sizeof(nop));
      literal_offset += sizeof(nop);
    }
    uint32_t address = static_cast<uint32_t>(destination);
    memcpy(where + literal_offset, &address, sizeof(address));
    return literal_offset + sizeof(address);
  } else {
    const uint32_t jump = 0xe51ff004u; // ldr pc, [pc, #-4]
    memcpy(where, &jump, sizeof(jump));
    uint32_t address = static_cast<uint32_t>(destination);
    memcpy(where + 4, &address, sizeof(address));
    return 8;
  }
}

inline const uint8_t *arm32_code_bytes(uint8_t *symbol) {
  return reinterpret_cast<const uint8_t *>(reinterpret_cast<uintptr_t>(symbol) &
                                           ~uintptr_t{1});
}

inline void *install(void *target, void *replacement, Hook *out,
                     bool prefer_relative = false,
                     bool capture_return = false) {
  uintptr_t callable = reinterpret_cast<uintptr_t>(target);
  bool thumb = (callable & 1U) != 0;
  uintptr_t target_address = callable & ~uintptr_t{1};
  auto *target_bytes = reinterpret_cast<uint8_t *>(target_address);
  size_t patch_size = 0;
  size_t copy_size = 0;
  bool relative = false;
  void *page = nullptr;
  if (prefer_relative) {
    patch_size = 4;
    copy_size = arm32_copy_size(target_bytes, thumb, patch_size);
    if (copy_size != 0) {
      page = alloc_arm32_near(target_address, thumb);
      relative = page != nullptr;
    }
  }
  if (page == nullptr) {
    patch_size = absolute_jump_size(target_address, thumb);
    copy_size = arm32_copy_size(target_bytes, thumb, patch_size);
    if (copy_size == 0)
      return nullptr;
    page = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  const uint8_t *capture =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl : yz_cap_arm_tmpl);
  const uint8_t *capture_end =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_end : yz_cap_arm_tmpl_end);
  const uint8_t *capture_ctx =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_ctx : yz_cap_arm_tmpl_ctx);
  const uint8_t *capture_wrap =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_wrap : yz_cap_arm_tmpl_wrap);
  size_t capture_size = static_cast<size_t>(capture_end - capture);
  size_t call_original_offset = (capture_size + 3U) & ~size_t{3};
  size_t trampoline_size = call_original_offset + copy_size + 10;
  if (page == MAP_FAILED || trampoline_size > 0x1000) {
    if (page != MAP_FAILED)
      munmap(page, 0x1000);
    return nullptr;
  }

  auto *base = static_cast<uint8_t *>(page);
  memcpy(base, capture, capture_size);
  uint32_t context =
      capture_return ? reinterpret_cast<uint32_t>(g_yz_ret_ctx) : 0;
  uint32_t wrapper =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
  memcpy(base + (capture_ctx - capture), &context, sizeof(context));
  memcpy(base + (capture_wrap - capture), &wrapper, sizeof(wrapper));

  auto *call_original = base + call_original_offset;
  memcpy(call_original, target_bytes, copy_size);
  emit_absolute_jump(call_original + copy_size,
                     reinterpret_cast<uintptr_t>(call_original + copy_size),
                     target_address + copy_size + (thumb ? 1 : 0), thumb);
  __builtin___clear_cache(reinterpret_cast<char *>(base),
                          reinterpret_cast<char *>(base + trampoline_size));
  if (mprotect(page, 0x1000, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, 0x1000);
    return nullptr;
  }

  uint8_t patch[10] = {};
  if (relative)
    emit_relative_jump(patch, target_address, reinterpret_cast<uintptr_t>(page),
                       thumb);
  else
    emit_absolute_jump(patch, target_address,
                       reinterpret_cast<uintptr_t>(page) + (thumb ? 1 : 0),
                       thumb);
  memcpy(out->saved, target_bytes, copy_size);
  if (!yz_patch_text(target_address, patch,
                     static_cast<unsigned int>(patch_size))) {
    munmap(page, 0x1000);
    return nullptr;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(target_bytes),
                          reinterpret_cast<char *>(target_bytes + patch_size));

  out->target = reinterpret_cast<uint32_t *>(target_address);
  out->trampoline = page;
  out->patched_size = static_cast<uint8_t>(patch_size);
  out->active = true;
  return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(call_original) +
                                  (thumb ? 1 : 0));
}

inline bool uninstall(Hook *hook, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (!hook->active)
    return true;
  const uintptr_t target = reinterpret_cast<uintptr_t>(hook->target);
  if (mode == UnhookMode::DiscardCowPages) {
    if (!discard_cow_patch_pages(target, hook->patched_size))
      return false;
  } else if (!yz_patch_text(target, hook->saved, hook->patched_size)) {
    return false;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(hook->target),
                          reinterpret_cast<char *>(hook->target) +
                              hook->patched_size);
  if (hook->trampoline != nullptr && munmap(hook->trampoline, 0x1000) != 0)
    return false;
  hook->trampoline = nullptr;
  hook->patched_size = 0;
  hook->active = false;
  return true;
}

} // namespace yuki::ihook
#else
namespace yuki::ihook {

struct Hook {
  bool active = false;
};

inline void *install(void *, void *, Hook *, bool = false, bool = false) {
  return nullptr;
}
inline bool uninstall(Hook *, UnhookMode = UnhookMode::RestoreBytes) {
  return false;
}

} // namespace yuki::ihook
#endif // #if defined(__aarch64__)
