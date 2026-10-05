#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#include "ihook_a64.hpp"
#include "ihook_arm32.hpp"
#include "ihook_lifecycle.hpp"
#include "ihook_sync.hpp"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif // #ifndef MAP_FIXED_NOREPLACE

namespace yuki::ihook {

extern "C" uint8_t yz_patch_text_checked(uintptr_t addr, const void *expected,
                                         const void *replacement,
                                         unsigned int len);

enum class PatchResult : uint8_t { Rejected, Applied, Indeterminate };

[[nodiscard]] inline PatchResult checked_patch_result(uintptr_t address,
                                                      const void *expected,
                                                      const void *replacement,
                                                      size_t size) {
  const uint8_t result = yz_patch_text_checked(address, expected, replacement,
                                               static_cast<unsigned int>(size));
  return result == 0   ? PatchResult::Rejected
         : result == 1 ? PatchResult::Applied
                       : PatchResult::Indeterminate;
}

inline bool within_page(uintptr_t address, size_t size) {
  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  return size != 0 && address <= UINTPTR_MAX - size &&
         address % page_size + size <= page_size;
}

inline bool hook_ranges_overlap(uintptr_t first, size_t first_size,
                                uintptr_t second, size_t second_size) {
  if (first_size == 0 || second_size == 0 || first > UINTPTR_MAX - first_size ||
      second > UINTPTR_MAX - second_size)
    return true;
  return first < second + second_size && second < first + first_size;
}

inline bool single_threaded_process() {
  DIR *tasks = opendir("/proc/self/task");
  if (tasks == nullptr)
    return false;
  unsigned int count = 0;
  while (const dirent *entry = readdir(tasks)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9' && ++count > 1)
      break;
  }
  closedir(tasks);
  return count == 1;
}

inline int64_t sign_extend(uint32_t value, unsigned int bits) {
  return static_cast<int32_t>(value << (32 - bits)) >> (32 - bits);
}

enum class UnhookMode : uint8_t {
  RestoreBytes,
  DiscardCowPages,
};

class ExecPage {
public:
  ExecPage() = default;
  explicit ExecPage(void *address) : addr_(address) {}
  ExecPage(const ExecPage &) = delete;
  ExecPage &operator=(const ExecPage &) = delete;
  ExecPage(ExecPage &&other) noexcept : addr_(other.release()) {}
  ExecPage &operator=(ExecPage &&other) noexcept {
    if (this != &other) {
      reset();
      addr_ = other.release();
    }
    return *this;
  }
  ~ExecPage() { reset(); }
  [[nodiscard]] void *get() const { return addr_; }
  [[nodiscard]] void *release() {
    void *result = addr_;
    addr_ = nullptr;
    return result;
  }

private:
  void reset() {
    if (addr_ != nullptr)
      munmap(addr_, static_cast<size_t>(getpagesize()));
    addr_ = nullptr;
  }
  void *addr_ = nullptr;
};

inline bool discard_cow_patch_pages(uintptr_t address, const void *patched,
                                    const void *saved, size_t patch_size,
                                    HookPhase *phase = nullptr) {
  if (!single_threaded_process() || !within_page(address, patch_size) ||
      memcmp(reinterpret_cast<const void *>(address), patched, patch_size) != 0)
    return false;
  const size_t page_size = static_cast<size_t>(getpagesize());
  const uintptr_t first_page = address - address % page_size;
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return false;
  char line[8192];
  char path[4096];
  unsigned long start = 0;
  unsigned long end = 0;
  unsigned long long offset = 0;
  char perms[5] = {};
  bool found = false;
  while (fgets(line, sizeof(line), maps) != nullptr) {
    path[0] = '\0';
    if (sscanf(line, "%lx-%lx %4s %llx %*s %*s  %4095[^\n]", &start, &end,
               perms, &offset, path) != 5)
      continue;
    if (first_page >= start && first_page < end &&
        end - first_page >= page_size && perms[3] == 'p' && path[0] == '/') {
      found = true;
      break;
    }
  }
  fclose(maps);
  if (!found)
    return false;

  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  auto *image = static_cast<uint8_t *>(malloc(page_size * 2));
  if (image == nullptr) {
    close(fd);
    return false;
  }
  auto *backing = image + page_size;
  memcpy(image, reinterpret_cast<const void *>(first_page), page_size);
  memcpy(image + address - first_page, saved, patch_size);
  const off_t file_offset = static_cast<off_t>(offset + first_page - start);
  const bool unchanged = pread(fd, backing, page_size, file_offset) ==
                             static_cast<ssize_t>(page_size) &&
                         memcmp(image, backing, page_size) == 0;
  free(image);
  close(fd);
  if (!unchanged)
    return false;
  if (madvise(reinterpret_cast<void *>(first_page), page_size, MADV_DONTNEED) !=
      0)
    return false;
  if (memcmp(reinterpret_cast<const void *>(address), saved, patch_size) != 0) {
    fail_indeterminate(phase);
  }
  return true;
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
  uint32_t *target = nullptr;
  uint32_t saved[2] = {};
  uint32_t patch[2] = {};
  void *trampoline = nullptr;
  bool active = false;
  bool restored = false;
  HookPhase phase = HookPhase::Released;
  InstructionSyncPlan sync_plan{};
};

[[nodiscard]] inline int trampoline_protection() {
  return PROT_READ | PROT_EXEC |
         ((getauxval(AT_HWCAP2) & HWCAP2_BTI) != 0 ? PROT_BTI : 0);
}

[[nodiscard]] inline uint32_t entry_landing(uint32_t instruction) {
  if (instruction == 0xd503245fU || instruction == 0xd503249fU ||
      instruction == 0xd50324dfU)
    return instruction;
  return 0xd503245fU;
}

[[nodiscard]] inline ExecPage alloc_near(uintptr_t target,
                                         const uint8_t *source,
                                         size_t original_offset,
                                         a64::Plan *plan) {
  constexpr uintptr_t reach = 0x7800000;
  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  const uintptr_t step = (0x10000 + page_size - 1) / page_size * page_size;
  const uintptr_t base = target - target % page_size;
  for (uintptr_t off = 0; off <= reach; off += step) {
    for (int up = 0; up < 2; ++up) {
      if ((!up && base < off) || (up && base > UINTPTR_MAX - off))
        continue;
      const uintptr_t hint = up ? base + off : base - off;
      if (hint > UINTPTR_MAX - original_offset ||
          a64::branch(target + 4, hint) == 0 ||
          a64::plan(source, 8, target, hint + original_offset,
                    page_size - original_offset, plan) != a64::Error::None)
        continue;
      void *p = mmap(reinterpret_cast<void *>(hint), page_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint)
        return ExecPage(p);
      if (p != MAP_FAILED)
        munmap(p, page_size);
    }
  }
  return {};
}

/* Patch target prologue and return call-original trampoline. */
inline void *install(void *target, void *replacement, Hook *out, bool = false,
                     bool capture_return = false,
                     a64::Error *reason = nullptr) {
  const auto reject = [reason](a64::Error error) -> void * {
    if (reason != nullptr)
      *reason = error;
    return nullptr;
  };
  if (reason != nullptr)
    *reason = a64::Error::None;
  if (target == nullptr || replacement == nullptr || out == nullptr ||
      out->active || out->phase != HookPhase::Released ||
      (reinterpret_cast<uintptr_t>(target) & 3U) != 0 ||
      !within_page(reinterpret_cast<uintptr_t>(target), 8))
    return reject(a64::Error::InvalidInput);
  auto *t = reinterpret_cast<uint32_t *>(target);

  const size_t cap_size = static_cast<size_t>(yz_cap_tmpl_end - yz_cap_tmpl);
  const size_t ctx_off = static_cast<size_t>(yz_cap_tmpl_ctx - yz_cap_tmpl);
  const size_t wrap_off = static_cast<size_t>(yz_cap_tmpl_wrap - yz_cap_tmpl);
  const size_t co_off = (cap_size + 3U) & ~static_cast<size_t>(3); // 4-aligned
  const size_t page_size = static_cast<size_t>(getpagesize());
  if (co_off > page_size - 96)
    return reject(a64::Error::Capacity);
  a64::Plan plan{};
  const a64::Error analyzed =
      a64::analyze(reinterpret_cast<uint8_t *>(target), 8,
                   reinterpret_cast<uintptr_t>(target), &plan);
  if (analyzed != a64::Error::None)
    return reject(analyzed);
  ExecPage page =
      alloc_near(reinterpret_cast<uintptr_t>(target),
                 reinterpret_cast<uint8_t *>(target), co_off + 4, &plan);
  if (page.get() == nullptr)
    return reject(a64::Error::Mapping);
  out->phase = HookPhase::Prepared;
  auto *base = static_cast<uint8_t *>(page.get());
  memcpy(base, yz_cap_tmpl, cap_size);
  *reinterpret_cast<uint64_t *>(base + ctx_off) =
      capture_return ? reinterpret_cast<uint64_t>(g_yz_ret_ctx) : 0;
  *reinterpret_cast<uint64_t *>(base + wrap_off) =
      reinterpret_cast<uint64_t>(replacement);
  auto *co = reinterpret_cast<uint32_t *>(base + co_off);
  co[0] = 0xd50324dfU;
  memcpy(out->saved, target, sizeof(out->saved));
  const a64::Error emitted =
      a64::emit(plan, reinterpret_cast<uintptr_t>(target),
                reinterpret_cast<uintptr_t>(co + 1),
                reinterpret_cast<uint8_t *>(co + 1), page_size - co_off - 4);
  if (emitted != a64::Error::None) {
    out->phase = HookPhase::Released;
    return reject(emitted);
  }
  if (synchronize_instruction_view(
          reinterpret_cast<uintptr_t>(base), co_off + 4 + plan.code_size,
          SyncOperation::Prepare) != SyncResult::Confirmed) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::Protect);
  }
  if (mprotect(page.get(), page_size, trampoline_protection()) != 0) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::Protect);
  }
  uint32_t patch[2] = {
      entry_landing(out->saved[0]),
      a64::branch(reinterpret_cast<uintptr_t>(target) + 4,
                  reinterpret_cast<uintptr_t>(page.get())),
  };
  if (!prepare_instruction_sync(reinterpret_cast<uintptr_t>(target),
                                sizeof(patch), &out->sync_plan)) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::Protect);
  }
  out->target = t;
  memcpy(out->patch, patch, sizeof(patch));
  const PatchResult committed = checked_patch_result(
      reinterpret_cast<uintptr_t>(target), out->saved, patch, sizeof(patch));
  if (committed == PatchResult::Rejected) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::PatchRejected);
  }
  out->trampoline = page.release();
  out->active = true;
  out->restored = false;
  out->phase = HookPhase::PublishedPendingSync;
  if (committed == PatchResult::Indeterminate ||
      synchronize_instruction_view(out->sync_plan, SyncOperation::Publish) !=
          SyncResult::Confirmed)
    fail_indeterminate(&out->phase);
  out->phase = HookPhase::Installed;
  return co;
}

inline bool uninstall(Hook *h, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (h == nullptr)
    return false;
  if (h->phase == HookPhase::Released)
    return true;
  if (h->phase != HookPhase::Installed &&
      h->phase != HookPhase::RestoredPendingReclaim)
    fail_indeterminate(&h->phase);
  if (h->phase == HookPhase::Installed) {
    if (!instruction_sync_still_supported(h->sync_plan))
      return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(h->target);
    const bool discarded = mode == UnhookMode::DiscardCowPages &&
                           discard_cow_patch_pages(target, h->patch, h->saved,
                                                   sizeof(h->saved), &h->phase);
    const PatchResult restored =
        discarded ? PatchResult::Applied
                  : checked_patch_result(target, h->patch, h->saved,
                                         sizeof(h->saved));
    if (restored == PatchResult::Rejected)
      return false;
    if (restored == PatchResult::Indeterminate)
      fail_indeterminate(&h->phase);
    h->restored = true;
    h->phase = HookPhase::RestoredPendingSync;
    if (synchronize_instruction_view(h->sync_plan, SyncOperation::Restore) !=
        SyncResult::Confirmed)
      fail_indeterminate(&h->phase);
    h->phase = HookPhase::RestoredPendingReclaim;
  }
  if (h->trampoline != nullptr &&
      munmap(h->trampoline, static_cast<size_t>(getpagesize())) != 0) {
    return false;
  }
  h->trampoline = nullptr;
  h->active = false;
  h->phase = HookPhase::Released;
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
  uint8_t patch[10] = {};
  void *trampoline = nullptr;
  uint8_t patched_size = 0;
  bool active = false;
  bool restored = false;
  HookPhase phase = HookPhase::Released;
  InstructionSyncPlan sync_plan{};
};

inline bool thumb_is_32bit(uint16_t instruction) {
  uint16_t prefix = instruction >> 11;
  return prefix == 0x1d || prefix == 0x1e || prefix == 0x1f;
}

inline bool thumb_inside_it(uintptr_t address) {
  const size_t available =
      (address % static_cast<uintptr_t>(getpagesize())) / sizeof(uint16_t);
  if (available < 8)
    return true;
  const size_t limit = available < 8 ? available : 8;
  for (size_t back = 1; back <= limit; ++back) {
    uint16_t previous = 0;
    memcpy(&previous, reinterpret_cast<const void *>(address - back * 2),
           sizeof(previous));
    const uint16_t mask = previous & 0xfu;
    if ((previous & 0xff00u) == 0xbf00u && mask != 0)
      return true;
  }
  return false;
}

inline bool thumb_is_wide_branch(const uint16_t *instruction) {
  if ((instruction[0] & 0xf800u) != 0xf000u)
    return false;
  const uint16_t kind = instruction[1] & 0xd000u;
  return kind == 0x9000u;
}

inline bool thumb_is_pcrel(const uint16_t *instruction, bool wide) {
  uint16_t first = instruction[0];
  if (!wide) {
    if ((first & 0xff00u) == 0xbf00u && (first & 0x000fu) != 0)
      return true; // IT block must not be split or copied out of context.
    if ((first & 0xf800u) == 0x4800u || // LDR literal
        (first & 0xf800u) == 0xa000u || // ADR
        (first & 0xf000u) == 0xd000u || // B.cond / SVC
        (first & 0xf800u) == 0xe000u || // B
        (first & 0xf500u) == 0xb100u)   // CBZ / CBNZ
      return true;
    if ((first & 0xff00u) == 0xbd00u) // POP including PC
      return true;
    if ((first & 0xfc00u) == 0x4400u) {
      unsigned int rm = (first >> 3) & 0xf;
      unsigned int rd = (first & 7) | ((first >> 4) & 8);
      return rm == 15 || rd == 15;
    }
    return false;
  }

  uint16_t second = instruction[1];
  if ((first & 0xfe00u) == 0xea00u &&
      (((first & 0xfu) == 15) || ((second & 0xfu) == 15) ||
       (((second >> 8) & 0xfu) == 15)))
    return true; // Thumb-2 data processing uses PC as a register.
  if ((first & 0xfu) == 15 &&
      (((first & 0xff00u) == 0xed00u) || ((first & 0xf800u) == 0xf800u)))
    return true;                      // VFP and load/store literal forms.
  if ((first & 0xff7fu) == 0xf85fu || // LDR literal
      (first & 0xfbffu) == 0xf20fu || // ADR / ADR.W
      (first & 0xfbffu) == 0xf2afu)   // SUBW from PC
    return true;
  if ((first & 0xf800u) == 0xf000u && (second & 0x8000u) != 0)
    return true; // B.W / BL / BLX and conditional branches
  if ((first & 0xfff0u) == 0xe8d0u && (second & 0xfff0u) == 0xf000u)
    return true; // TBB / TBH
  if ((first & 0xfff0u) == 0xe8b0u && (second & 0x8000u) != 0)
    return true; // LDM.W including PC
  return false;
}

inline bool arm_is_pcrel(uint32_t instruction) {
  if ((instruction >> 28) == 0xfu)
    return true;
  if ((instruction & 0x0e000000u) == 0x08000000u &&
      (instruction & 0x8000u) != 0)
    return true; // Load/store multiple with PC in the register list.
  if ((instruction & 0x0ffffff0u) == 0x012fff10u ||
      (instruction & 0x0ffffff0u) == 0x012fff30u)
    return true; // BX / BLX register changes control flow.
  if ((instruction & 0x0e000000u) == 0x0a000000u)
    return true; // B / BL
  if ((instruction & 0x0f000000u) == 0x0f000000u)
    return true; // SVC and unconditional instructions.
  unsigned int rn = (instruction >> 16) & 0xf;
  unsigned int group = instruction & 0x0c000000u;
  if (group == 0 || group == 0x04000000u) {
    if (rn == 15 || ((instruction >> 12) & 0xf) == 15)
      return true;
    if ((instruction & 0x02000000u) == 0 && (instruction & 0xf) == 15)
      return true; // Register operand uses PC.
    if (group == 0 && (instruction & 0x02000010u) == 0x10u &&
        ((instruction >> 8) & 0xf) == 15)
      return true; // Register shift amount comes from PC.
  }
  return group == 0x0c000000u;
}

inline bool arm_is_branch(uint32_t instruction) {
  return (instruction >> 28) != 0xfu &&
         (instruction & 0x0f000000u) == 0x0a000000u;
}

inline size_t arm32_copy_size(const void *target, bool thumb,
                              size_t minimum_size) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(target);
  if (!within_page(address, minimum_size))
    return 0;
  arm32::Plan plan{};
  const size_t page_remaining = static_cast<size_t>(getpagesize()) -
                                address % static_cast<size_t>(getpagesize());
  if (arm32::analyze(static_cast<const uint8_t *>(target), page_remaining,
                     address, minimum_size, thumb, &plan) != arm32::Error::None)
    return 0;
  return plan.copied_size;
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

inline void emit_relative_jump(uint8_t *where, uintptr_t from, uintptr_t to,
                               bool thumb);

inline bool relocate_arm32_copy(const uint8_t *source, uint8_t *destination,
                                uintptr_t source_address,
                                uintptr_t destination_address, size_t size,
                                bool thumb) {
  size_t offset = 0;
  while (offset < size) {
    if (!thumb) {
      uint32_t insn = 0;
      memcpy(&insn, source + offset, sizeof(insn));
      if (arm_is_pcrel(insn) && !arm_is_branch(insn))
        return false;
      if (arm_is_branch(insn)) {
        const int64_t old_delta = sign_extend(insn & 0x00ffffffu, 24) * 4;
        const uintptr_t branch_target = static_cast<uintptr_t>(
            static_cast<int64_t>(source_address + offset + 8) + old_delta);
        if ((branch_target >= source_address &&
             branch_target - source_address < size) ||
            !relative_jump_reachable(destination_address + offset,
                                     branch_target, false))
          return false;
        const int64_t new_delta =
            static_cast<int64_t>(branch_target) -
            static_cast<int64_t>(destination_address + offset + 8);
        const uint32_t relocated =
            (insn & 0xff000000u) |
            ((static_cast<uint32_t>(new_delta >> 2)) & 0x00ffffffu);
        memcpy(destination + offset, &relocated, sizeof(relocated));
      }
      offset += 4;
      continue;
    }

    uint16_t first = 0;
    memcpy(&first, source + offset, sizeof(first));
    const bool wide = thumb_is_32bit(first);
    if (wide) {
      uint16_t second = 0;
      memcpy(&second, source + offset + 2, sizeof(second));
      const uint16_t pair[] = {first, second};
      if (thumb_is_pcrel(pair, true) && !thumb_is_wide_branch(pair))
        return false;
      if (thumb_is_wide_branch(pair)) {
        const uint32_t sign = (first >> 10) & 1u;
        const uint32_t j1 = (second >> 13) & 1u;
        const uint32_t j2 = (second >> 11) & 1u;
        const uint32_t i1 = (~(j1 ^ sign)) & 1u;
        const uint32_t i2 = (~(j2 ^ sign)) & 1u;
        const uint32_t encoded = (sign << 24) | (i1 << 23) | (i2 << 22) |
                                 ((first & 0x03ffu) << 12) |
                                 ((second & 0x07ffu) << 1);
        const uintptr_t branch_target = static_cast<uintptr_t>(
            static_cast<int64_t>(source_address + offset + 4) +
            sign_extend(encoded, 25));
        if ((branch_target >= source_address &&
             branch_target - source_address < size) ||
            !relative_jump_reachable(destination_address + offset,
                                     branch_target, true))
          return false;
        emit_relative_jump(destination + offset, destination_address + offset,
                           branch_target, true);
        if ((second & 0xd000u) == 0xd000u)
          destination[offset + 3] |= 0x40u; // B.W -> BL.
      }
      offset += 4;
    } else {
      if (thumb_is_pcrel(&first, false))
        return false;
      offset += 2;
    }
  }
  return true;
}

inline void *alloc_arm32_near(uintptr_t target, bool thumb) {
  constexpr uintptr_t kReach = 0x00f00000U;
  constexpr uintptr_t kStep = 0x00010000U;
  const size_t kPageSize = static_cast<size_t>(getpagesize());
  uintptr_t base = target & ~static_cast<uintptr_t>(kPageSize - 1U);

  void *page = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE,
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
                  PROT_READ | PROT_WRITE,
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
  if (target == nullptr || replacement == nullptr || out == nullptr ||
      out->active || out->phase != HookPhase::Released)
    return nullptr;
  uintptr_t callable = reinterpret_cast<uintptr_t>(target);
  bool thumb = (callable & 1U) != 0;
  uintptr_t target_address = callable & ~uintptr_t{1};
  if (!thumb && (target_address & 3U) != 0)
    return nullptr;
  if (thumb && thumb_inside_it(target_address))
    return nullptr;
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
    page = mmap(nullptr, static_cast<size_t>(getpagesize()),
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  if (!within_page(target_address, patch_size)) {
    if (page != nullptr && page != MAP_FAILED)
      munmap(page, static_cast<size_t>(getpagesize()));
    return nullptr;
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
  const size_t page_size = static_cast<size_t>(getpagesize());
  if (page == nullptr || page == MAP_FAILED || trampoline_size > page_size) {
    if (page != nullptr && page != MAP_FAILED)
      munmap(page, page_size);
    return nullptr;
  }

  ExecPage mapping(page);
  out->phase = HookPhase::Prepared;

  auto *base = static_cast<uint8_t *>(page);
  memcpy(base, capture, capture_size);
  uint32_t context =
      capture_return
          ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_yz_ret_ctx))
          : 0;
  uint32_t wrapper =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
  memcpy(base + (capture_ctx - capture), &context, sizeof(context));
  memcpy(base + (capture_wrap - capture), &wrapper, sizeof(wrapper));

  auto *call_original = base + call_original_offset;
  memcpy(call_original, target_bytes, copy_size);
  if (!relocate_arm32_copy(target_bytes, call_original, target_address,
                           reinterpret_cast<uintptr_t>(call_original),
                           copy_size, thumb)) {
    out->phase = HookPhase::Released;
    return nullptr;
  }
  emit_absolute_jump(call_original + copy_size,
                     reinterpret_cast<uintptr_t>(call_original + copy_size),
                     target_address + copy_size + (thumb ? 1 : 0), thumb);
  if (synchronize_instruction_view(reinterpret_cast<uintptr_t>(base),
                                   trampoline_size, SyncOperation::Prepare) !=
      SyncResult::Confirmed) {
    out->phase = HookPhase::Released;
    return nullptr;
  }
  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
    out->phase = HookPhase::Released;
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
  if (!prepare_instruction_sync(target_address, patch_size, &out->sync_plan)) {
    out->phase = HookPhase::Released;
    return nullptr;
  }
  out->target = reinterpret_cast<uint32_t *>(target_address);
  memcpy(out->patch, patch, patch_size);
  out->patched_size = static_cast<uint8_t>(patch_size);
  const PatchResult committed =
      checked_patch_result(target_address, out->saved, patch, patch_size);
  if (committed == PatchResult::Rejected) {
    out->phase = HookPhase::Released;
    out->patched_size = 0;
    return nullptr;
  }
  out->trampoline = mapping.release();
  out->active = true;
  out->restored = false;
  out->phase = HookPhase::PublishedPendingSync;
  if (committed == PatchResult::Indeterminate ||
      synchronize_instruction_view(out->sync_plan, SyncOperation::Publish) !=
          SyncResult::Confirmed)
    fail_indeterminate(&out->phase);
  out->phase = HookPhase::Installed;
  return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(call_original) +
                                  (thumb ? 1 : 0));
}

inline bool uninstall(Hook *hook, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (hook == nullptr)
    return false;
  if (hook->phase == HookPhase::Released)
    return true;
  if (hook->phase != HookPhase::Installed &&
      hook->phase != HookPhase::RestoredPendingReclaim)
    fail_indeterminate(&hook->phase);
  if (hook->phase == HookPhase::Installed) {
    if (!instruction_sync_still_supported(hook->sync_plan))
      return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(hook->target);
    const bool discarded =
        mode == UnhookMode::DiscardCowPages &&
        discard_cow_patch_pages(target, hook->patch, hook->saved,
                                hook->patched_size, &hook->phase);
    const PatchResult restored =
        discarded ? PatchResult::Applied
                  : checked_patch_result(target, hook->patch, hook->saved,
                                         hook->patched_size);
    if (restored == PatchResult::Rejected)
      return false;
    if (restored == PatchResult::Indeterminate)
      fail_indeterminate(&hook->phase);
    hook->restored = true;
    hook->phase = HookPhase::RestoredPendingSync;
    if (synchronize_instruction_view(hook->sync_plan, SyncOperation::Restore) !=
        SyncResult::Confirmed)
      fail_indeterminate(&hook->phase);
    hook->phase = HookPhase::RestoredPendingReclaim;
  }
  if (hook->trampoline != nullptr &&
      munmap(hook->trampoline, static_cast<size_t>(getpagesize())) != 0) {
    return false;
  }
  hook->trampoline = nullptr;
  hook->patched_size = 0;
  hook->active = false;
  hook->phase = HookPhase::Released;
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
