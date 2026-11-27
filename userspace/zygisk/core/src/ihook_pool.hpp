#pragma once

#include "ihook_lifecycle.hpp"
#include "ihook_sync.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif // #ifndef MAP_FIXED_NOREPLACE

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

#ifndef __NR_memfd_create
#if defined(__aarch64__)
#define __NR_memfd_create 279
#elif defined(__arm__)
#define __NR_memfd_create 385
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#endif
#endif // #ifndef __NR_memfd_create

namespace yuki::ihook {

namespace alloc_detail {

struct Gap {
  uintptr_t begin = 0;
  uintptr_t end = 0;
};

[[nodiscard]] inline int create_memfd(size_t page_size) {
#if defined(__NR_memfd_create) && defined(__linux__) &&                        \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  const int fd = static_cast<int>(lifecycle_detail::raw_call(
      __NR_memfd_create, reinterpret_cast<uintptr_t>("yuki_ihook"),
      MFD_CLOEXEC));
  if (fd < 0)
    return -1;
  if (ftruncate(fd, static_cast<off_t>(page_size)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
#else
  (void)page_size;
  return -1;
#endif
}

template <typename AcceptFn>
[[nodiscard]] inline void *try_map_page(uintptr_t hint, size_t page_size,
                                        AcceptFn &accept, int fd = -1,
                                        int prot = PROT_READ | PROT_WRITE) {
  if (!accept(hint))
    return nullptr;
  const int flags = (fd >= 0 ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS)) |
                    MAP_FIXED_NOREPLACE;
  void *p = mmap(reinterpret_cast<void *>(hint), page_size, prot, flags, fd, 0);
  if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint)
    return p;
  if (p != MAP_FAILED)
    munmap(p, page_size);
  return nullptr;
}

template <typename AcceptFn>
[[nodiscard]] inline void *
probe_gap_outward(const Gap &gap, uintptr_t preferred, size_t page_size,
                  AcceptFn &accept, int fd = -1,
                  int prot = PROT_READ | PROT_WRITE) {
  if (gap.end <= gap.begin || gap.end - gap.begin < page_size)
    return nullptr;
  const uintptr_t last_page = gap.end - page_size;
  uintptr_t anchor = preferred;
  if (anchor < gap.begin)
    anchor = gap.begin;
  else if (anchor > last_page)
    anchor = last_page;
  anchor -= anchor % page_size;

  if (void *p = try_map_page(anchor, page_size, accept, fd, prot))
    return p;

  const size_t max_steps = 64;
  for (size_t step = 1; step <= max_steps; ++step) {
    const uintptr_t delta = step * page_size;
    bool any = false;
    if (anchor >= gap.begin + delta) {
      any = true;
      if (void *p = try_map_page(anchor - delta, page_size, accept, fd, prot))
        return p;
    }
    if (anchor <= last_page - delta) {
      any = true;
      if (void *p = try_map_page(anchor + delta, page_size, accept, fd, prot))
        return p;
    }
    if (!any)
      break;
  }
  return nullptr;
}

template <typename AcceptFn>
[[nodiscard]] inline void *
alloc_near_gap_raw(uintptr_t min_addr, uintptr_t max_addr, uintptr_t preferred,
                   size_t page_size, AcceptFn accept, int fd = -1,
                   int prot = PROT_READ | PROT_WRITE) {
  if (page_size == 0 || min_addr > max_addr)
    return nullptr;
  const uintptr_t floor_addr = min_addr < page_size ? page_size : min_addr;
  const uintptr_t min_page =
      (floor_addr + page_size - 1U) / page_size * page_size;
  const uintptr_t max_page = max_addr - (max_addr % page_size);
  if (min_page == 0 || min_page > max_page)
    return nullptr;
  const uintptr_t search_end =
      max_page <= UINTPTR_MAX - page_size ? max_page + page_size : UINTPTR_MAX;

  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps != nullptr) {
    constexpr size_t kMaxGaps = 128;
    Gap gaps[kMaxGaps]{};
    size_t gap_count = 0;
    uintptr_t cursor = min_page;
    char line[512];
    bool truncated = false;
    while (fgets(line, sizeof(line), maps) != nullptr) {
      if (strchr(line, '\n') == nullptr) {
        int ch = 0;
        while ((ch = fgetc(maps)) != EOF && ch != '\n') {
        }
      }
      unsigned long long start = 0;
      unsigned long long end = 0;
      if (sscanf(line, "%llx-%llx", &start, &end) != 2 || start >= end)
        continue;
      if (end <= cursor)
        continue;
      if (start >= search_end)
        break;
      const uintptr_t map_start =
          static_cast<uintptr_t>(start > UINTPTR_MAX ? UINTPTR_MAX : start);
      if (map_start > cursor) {
        const uintptr_t g_end = map_start < search_end
                                    ? (map_start - (map_start % page_size))
                                    : search_end;
        if (g_end > cursor && g_end - cursor >= page_size) {
          if (gap_count < kMaxGaps)
            gaps[gap_count++] = {cursor, g_end};
          else
            truncated = true;
        }
      }
      const uintptr_t map_end =
          static_cast<uintptr_t>(end > UINTPTR_MAX ? UINTPTR_MAX : end);
      if (map_end >= search_end) {
        cursor = search_end;
        break;
      }
      const uintptr_t aligned_end =
          map_end <= UINTPTR_MAX - (page_size - 1U)
              ? (map_end + page_size - 1U) / page_size * page_size
              : UINTPTR_MAX;
      cursor = aligned_end > cursor ? aligned_end : cursor;
    }
    (void)fclose(maps);
    if (!truncated && cursor < search_end && search_end - cursor >= page_size &&
        gap_count < kMaxGaps) {
      gaps[gap_count++] = {cursor, search_end};
    }
    if (gap_count != 0) {
      bool used[kMaxGaps]{};
      for (size_t attempted = 0; attempted != gap_count; ++attempted) {
        size_t best = gap_count;
        uintptr_t best_dist = UINTPTR_MAX;
        for (size_t i = 0; i != gap_count; ++i) {
          if (used[i])
            continue;
          const Gap &g = gaps[i];
          const uintptr_t last_page = g.end - page_size;
          const uintptr_t dist =
              preferred < g.begin
                  ? g.begin - preferred
                  : (preferred > last_page ? preferred - last_page : 0);
          if (best == gap_count || dist < best_dist) {
            best = i;
            best_dist = dist;
          }
        }
        if (best == gap_count)
          break;
        used[best] = true;
        if (void *p = probe_gap_outward(gaps[best], preferred, page_size,
                                        accept, fd, prot))
          return p;
      }
    }
  }

  const uintptr_t base = preferred - (preferred % page_size);
  const uintptr_t step = (0x10000 + page_size - 1U) / page_size * page_size;
  const uintptr_t span_down = base > min_page ? base - min_page : 0;
  const uintptr_t span_up = max_page > base ? max_page - base : 0;
  const uintptr_t max_off = span_down > span_up ? span_down : span_up;
  for (uintptr_t off = 0; off <= max_off; off += step) {
    for (int up = 0; up < 2; ++up) {
      if ((!up && base < min_page + off) || (up && base > max_page - off))
        continue;
      const uintptr_t hint = up ? base + off : base - off;
      if (void *p = try_map_page(hint, page_size, accept, fd, prot))
        return p;
    }
  }
  return nullptr;
}

[[nodiscard]] inline uint64_t monotonic_ns() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL) +
         static_cast<uint64_t>(ts.tv_nsec);
}

[[nodiscard]] inline bool raw_mprotect(void *addr, size_t size, int prot) {
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  return lifecycle_detail::raw_call(__NR_mprotect,
                                    reinterpret_cast<uintptr_t>(addr), size,
                                    static_cast<uintptr_t>(prot)) == 0;
#else
  return mprotect(addr, size, prot) == 0;
#endif
}

inline void copy_executable_bytes(void *destination, const uint8_t *source,
                                  size_t size) {
  auto *dst = static_cast<volatile uint8_t *>(destination);
  for (size_t i = 0; i != size; ++i)
    dst[i] = source[i];
}

} // namespace alloc_detail

template <size_t SlotSize, size_t MaxPages> class SlotPool;

template <size_t SlotSize, size_t MaxPages> class ExecSlot {
public:
  ExecSlot() = default;
  ExecSlot(SlotPool<SlotSize, MaxPages> *pool, void *addr, void *page_base,
           void *write_base, size_t page_size, int exec_prot, bool pooled,
           bool fresh_page)
      : pool_(pool), addr_(addr), page_base_(page_base),
        write_base_(write_base), page_size_(page_size), exec_prot_(exec_prot),
        pooled_(pooled), fresh_page_(fresh_page) {}
  ExecSlot(const ExecSlot &) = delete;
  ExecSlot &operator=(const ExecSlot &) = delete;
  ExecSlot(ExecSlot &&other) noexcept
      : pool_(other.pool_), addr_(other.addr_), page_base_(other.page_base_),
        write_base_(other.write_base_), page_size_(other.page_size_),
        exec_prot_(other.exec_prot_), pooled_(other.pooled_),
        fresh_page_(other.fresh_page_) {
    other.clear_fields();
  }
  ExecSlot &operator=(ExecSlot &&other) noexcept {
    if (this != &other) {
      reset();
      pool_ = other.pool_;
      addr_ = other.addr_;
      page_base_ = other.page_base_;
      write_base_ = other.write_base_;
      page_size_ = other.page_size_;
      exec_prot_ = other.exec_prot_;
      pooled_ = other.pooled_;
      fresh_page_ = other.fresh_page_;
      other.clear_fields();
    }
    return *this;
  }
  ~ExecSlot() { reset(); }

  [[nodiscard]] explicit operator bool() const { return addr_ != nullptr; }
  [[nodiscard]] void *get() const { return addr_; }
  [[nodiscard]] void *page_base() const { return page_base_; }
  [[nodiscard]] void *write_base() const { return write_base_; }
  [[nodiscard]] size_t page_size() const { return page_size_; }
  [[nodiscard]] int exec_prot() const { return exec_prot_; }
  [[nodiscard]] bool is_pooled() const { return pooled_; }
  [[nodiscard]] bool is_fresh_page() const { return fresh_page_; }

  [[nodiscard]] void *release(bool *pooled_out = nullptr) {
    if (pooled_out != nullptr)
      *pooled_out = pooled_;
    void *result = addr_;
    clear_fields();
    return result;
  }

  void reset();

private:
  void clear_fields() {
    pool_ = nullptr;
    addr_ = nullptr;
    page_base_ = nullptr;
    write_base_ = nullptr;
    page_size_ = 0;
    exec_prot_ = 0;
    pooled_ = false;
    fresh_page_ = false;
  }

  SlotPool<SlotSize, MaxPages> *pool_ = nullptr;
  void *addr_ = nullptr;
  void *page_base_ = nullptr;
  void *write_base_ = nullptr;
  size_t page_size_ = 0;
  int exec_prot_ = 0;
  bool pooled_ = false;
  bool fresh_page_ = false;
};

template <size_t SlotSize, size_t MaxPages> class SlotPool {
public:
  static constexpr size_t kMaxSlotsPerPage = 256;
  static constexpr size_t kMaxRetired = 256;
  static constexpr uint64_t kDefaultGraceNs = 5000000000ULL;

  template <typename AcceptFn>
  [[nodiscard]] ExecSlot<SlotSize, MaxPages>
  alloc_near(uintptr_t min_addr, uintptr_t max_addr, uintptr_t preferred,
             int exec_prot, AcceptFn accept, bool allow_mprotect_reuse = true) {
    const size_t page_size = static_cast<size_t>(getpagesize());
    {
      pthread_mutex_lock(&mutex_);
      drain_retired_locked(false);
      size_t best_page = MaxPages;
      size_t best_slot = kMaxSlotsPerPage;
      uintptr_t best_dist = UINTPTR_MAX;
      for (size_t p = 0; p != MaxPages; ++p) {
        const PoolPage &page = pages_[p];
        if (page.base == nullptr || page.exec_prot != exec_prot)
          continue;
        if (!allow_mprotect_reuse && page.write_base == nullptr && page.is_rx)
          continue;
        const uintptr_t p_begin = reinterpret_cast<uintptr_t>(page.base);
        const uintptr_t p_end = p_begin + (page.slot_count * SlotSize);
        if (p_end <= min_addr || p_begin > max_addr)
          continue;
        for (size_t s = 0; s != page.slot_count; ++s) {
          if (slot_used(page, s))
            continue;
          const uintptr_t candidate = p_begin + (s * SlotSize);
          if (candidate < min_addr || candidate > max_addr)
            continue;
          const uintptr_t dist = candidate < preferred ? preferred - candidate
                                                       : candidate - preferred;
          if (dist < best_dist && accept(candidate)) {
            best_page = p;
            best_slot = s;
            best_dist = dist;
          }
        }
      }
      if (best_page != MaxPages) {
        PoolPage &page = pages_[best_page];
        mark_slot(page, best_slot, true);
        void *addr = static_cast<uint8_t *>(page.base) + (best_slot * SlotSize);
        void *write_base = page.write_base;
        pthread_mutex_unlock(&mutex_);
        return ExecSlot<SlotSize, MaxPages>(this, addr, page.base, write_base,
                                            page.page_size, exec_prot, true,
                                            false);
      }
      pthread_mutex_unlock(&mutex_);
    }

    void *fresh_exec = nullptr;
    void *fresh_write = nullptr;
    const int fd = alloc_detail::create_memfd(page_size);
    if (fd >= 0) {
      void *probe = mmap(nullptr, page_size, exec_prot, MAP_SHARED, fd, 0);
      if (probe != MAP_FAILED) {
        munmap(probe, page_size);
        fresh_exec = alloc_detail::alloc_near_gap_raw(
            min_addr, max_addr, preferred, page_size, accept, fd, exec_prot);
        if (fresh_exec != nullptr) {
          fresh_write = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
          if (fresh_write == MAP_FAILED) {
            munmap(fresh_exec, page_size);
            fresh_exec = nullptr;
            fresh_write = nullptr;
          }
        }
      }
      close(fd);
    }
    if (fresh_exec == nullptr) {
      fresh_exec = alloc_detail::alloc_near_gap_raw(
          min_addr, max_addr, preferred, page_size, accept);
      fresh_write = nullptr;
    }
    if (fresh_exec == nullptr)
      return {};
    return register_fresh_page(fresh_exec, fresh_write, page_size, exec_prot);
  }

  template <typename AcceptFn>
  [[nodiscard]] ExecSlot<SlotSize, MaxPages>
  alloc_any(int exec_prot, AcceptFn accept, bool allow_mprotect_reuse = true) {
    const size_t page_size = static_cast<size_t>(getpagesize());
    {
      pthread_mutex_lock(&mutex_);
      drain_retired_locked(false);
      for (size_t p = 0; p != MaxPages; ++p) {
        PoolPage &page = pages_[p];
        if (page.base == nullptr || page.exec_prot != exec_prot)
          continue;
        if (!allow_mprotect_reuse && page.write_base == nullptr && page.is_rx)
          continue;
        const uintptr_t p_begin = reinterpret_cast<uintptr_t>(page.base);
        for (size_t s = 0; s != page.slot_count; ++s) {
          if (slot_used(page, s))
            continue;
          const uintptr_t candidate = p_begin + (s * SlotSize);
          if (accept(candidate)) {
            mark_slot(page, s, true);
            void *addr = static_cast<uint8_t *>(page.base) + (s * SlotSize);
            void *write_base = page.write_base;
            pthread_mutex_unlock(&mutex_);
            return ExecSlot<SlotSize, MaxPages>(this, addr, page.base,
                                                write_base, page.page_size,
                                                exec_prot, true, false);
          }
        }
      }
      pthread_mutex_unlock(&mutex_);
    }

    void *fresh_exec = nullptr;
    void *fresh_write = nullptr;
    const int fd = alloc_detail::create_memfd(page_size);
    if (fd >= 0) {
      void *cand = mmap(nullptr, page_size, exec_prot, MAP_SHARED, fd, 0);
      if (cand != MAP_FAILED && accept(reinterpret_cast<uintptr_t>(cand))) {
        void *w =
            mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (w != MAP_FAILED) {
          fresh_exec = cand;
          fresh_write = w;
        } else {
          munmap(cand, page_size);
        }
      } else if (cand != MAP_FAILED) {
        munmap(cand, page_size);
      }
      close(fd);
    }
    if (fresh_exec == nullptr) {
      void *cand = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (cand == MAP_FAILED)
        return {};
      if (!accept(reinterpret_cast<uintptr_t>(cand))) {
        munmap(cand, page_size);
        return {};
      }
      fresh_exec = cand;
      fresh_write = nullptr;
    }
    return register_fresh_page(fresh_exec, fresh_write, page_size, exec_prot);
  }

  [[nodiscard]] bool commit_slot(const ExecSlot<SlotSize, MaxPages> &slot,
                                 const uint8_t *staging, size_t size) {
    if (!slot || staging == nullptr || size == 0 || size > SlotSize)
      return false;
    pthread_mutex_lock(&mutex_);
    PoolPage *pooled_page = nullptr;
    if (slot.is_pooled()) {
      for (size_t p = 0; p != MaxPages; ++p) {
        if (pages_[p].base == slot.page_base()) {
          pooled_page = &pages_[p];
          break;
        }
      }
    }
    void *write_base =
        pooled_page != nullptr ? pooled_page->write_base : slot.write_base();
    if (write_base != nullptr) {
      const size_t slot_off =
          static_cast<size_t>(static_cast<const uint8_t *>(slot.get()) -
                              static_cast<const uint8_t *>(slot.page_base()));
      auto *write_ptr = static_cast<uint8_t *>(write_base) + slot_off;
      alloc_detail::copy_executable_bytes(write_ptr, staging, size);
      __builtin___clear_cache(reinterpret_cast<char *>(write_ptr),
                              reinterpret_cast<char *>(write_ptr + size));
      const SyncResult synced =
          synchronize_instruction_view(reinterpret_cast<uintptr_t>(slot.get()),
                                       size, SyncOperation::Prepare);
      pthread_mutex_unlock(&mutex_);
      return synced == SyncResult::Confirmed;
    }

    const bool shared_rx =
        pooled_page != nullptr ? pooled_page->is_rx : !slot.is_fresh_page();
    if (shared_rx &&
        !alloc_detail::raw_mprotect(slot.page_base(), slot.page_size(),
                                    PROT_READ | PROT_WRITE)) {
      pthread_mutex_unlock(&mutex_);
      return false;
    }
    alloc_detail::copy_executable_bytes(slot.get(), staging, size);
    const SyncResult synced = synchronize_instruction_view(
        reinterpret_cast<uintptr_t>(slot.get()), size, SyncOperation::Prepare);
    const bool protected_rx = alloc_detail::raw_mprotect(
        slot.page_base(), slot.page_size(), slot.exec_prot());
    if (protected_rx && pooled_page != nullptr)
      pooled_page->is_rx = true;
    pthread_mutex_unlock(&mutex_);
    if (!protected_rx) {
      if (shared_rx)
        fail_indeterminate(nullptr);
      return false;
    }
    if (synced != SyncResult::Confirmed) {
      if (shared_rx)
        fail_indeterminate(nullptr);
      return false;
    }
    return true;
  }

  void retire_slot(void *addr, bool pooled, bool defer,
                   uint64_t grace_ns = kDefaultGraceNs) {
    if (addr == nullptr)
      return;
    pthread_mutex_lock(&mutex_);
    drain_retired_locked(false);
    if (!defer) {
      release_slot_locked(addr, pooled);
      pthread_mutex_unlock(&mutex_);
      return;
    }
    if (retired_count_ < kMaxRetired) {
      retired_[retired_count_++] = {addr, pooled,
                                    alloc_detail::monotonic_ns() + grace_ns};
    }
    pthread_mutex_unlock(&mutex_);
  }

  void release_slot(void *addr, bool pooled) {
    retire_slot(addr, pooled, false);
  }

  void drain_retired(bool force = false) {
    pthread_mutex_lock(&mutex_);
    drain_retired_locked(force);
    pthread_mutex_unlock(&mutex_);
  }

  [[nodiscard]] bool idle() {
    pthread_mutex_lock(&mutex_);
    drain_retired_locked(false);
    bool empty = retired_count_ == 0;
    if (empty) {
      for (size_t p = 0; p != MaxPages; ++p) {
        if (pages_[p].base != nullptr) {
          empty = false;
          break;
        }
      }
    }
    pthread_mutex_unlock(&mutex_);
    return empty;
  }

  [[nodiscard]] size_t active_page_count() {
    pthread_mutex_lock(&mutex_);
    size_t count = 0;
    for (size_t p = 0; p != MaxPages; ++p) {
      if (pages_[p].base != nullptr)
        ++count;
    }
    pthread_mutex_unlock(&mutex_);
    return count;
  }

  [[nodiscard]] bool has_dual_mapped_pages() {
    pthread_mutex_lock(&mutex_);
    bool dual = false;
    for (size_t p = 0; p != MaxPages; ++p) {
      if (pages_[p].base != nullptr && pages_[p].write_base != nullptr) {
        dual = true;
        break;
      }
    }
    pthread_mutex_unlock(&mutex_);
    return dual;
  }

private:
  struct PoolPage {
    void *base = nullptr;
    void *write_base = nullptr;
    size_t page_size = 0;
    uint64_t used_words[4] = {};
    int exec_prot = 0;
    size_t slot_count = 0;
    bool is_rx = false;
  };

  struct RetiredSlot {
    void *addr = nullptr;
    bool pooled = false;
    uint64_t ready_ns = 0;
  };

  [[nodiscard]] static bool slot_used(const PoolPage &page, size_t index) {
    return (page.used_words[index >> 6U] & (uint64_t{1} << (index & 63U))) != 0;
  }

  static void mark_slot(PoolPage &page, size_t index, bool used) {
    const uint64_t bit = uint64_t{1} << (index & 63U);
    if (used)
      page.used_words[index >> 6U] |= bit;
    else
      page.used_words[index >> 6U] &= ~bit;
  }

  [[nodiscard]] static bool page_empty(const PoolPage &page) {
    return (page.used_words[0] | page.used_words[1] | page.used_words[2] |
            page.used_words[3]) == 0;
  }

  void release_slot_locked(void *addr, bool pooled) {
    if (addr == nullptr)
      return;
    const size_t page_size = static_cast<size_t>(getpagesize());
    if (!pooled) {
      munmap(addr, page_size);
      return;
    }
    const uintptr_t raw = reinterpret_cast<uintptr_t>(addr);
    for (size_t p = 0; p != MaxPages; ++p) {
      PoolPage &page = pages_[p];
      if (page.base == nullptr)
        continue;
      const uintptr_t base = reinterpret_cast<uintptr_t>(page.base);
      if (raw < base || raw >= base + page.page_size)
        continue;
      const size_t slot = static_cast<size_t>(raw - base) / SlotSize;
      if (slot < page.slot_count)
        mark_slot(page, slot, false);
      if (page_empty(page)) {
        munmap(page.base, page.page_size);
        if (page.write_base != nullptr)
          munmap(page.write_base, page.page_size);
        page = PoolPage{};
      }
      return;
    }
  }

  void drain_retired_locked(bool force) {
    if (retired_count_ == 0)
      return;
    const uint64_t now = force ? UINT64_MAX : alloc_detail::monotonic_ns();
    size_t write_idx = 0;
    for (size_t i = 0; i != retired_count_; ++i) {
      const RetiredSlot item = retired_[i];
      if (force || now >= item.ready_ns) {
        release_slot_locked(item.addr, item.pooled);
      } else {
        retired_[write_idx++] = item;
      }
    }
    retired_count_ = write_idx;
  }

  [[nodiscard]] ExecSlot<SlotSize, MaxPages>
  register_fresh_page(void *fresh_exec, void *fresh_write, size_t page_size,
                      int exec_prot) {
    const size_t slots = std::min(page_size / SlotSize, kMaxSlotsPerPage);
    if (slots == 0) {
      munmap(fresh_exec, page_size);
      if (fresh_write != nullptr)
        munmap(fresh_write, page_size);
      return {};
    }
    pthread_mutex_lock(&mutex_);
    for (size_t p = 0; p != MaxPages; ++p) {
      if (pages_[p].base == nullptr) {
        PoolPage &page = pages_[p];
        page.base = fresh_exec;
        page.write_base = fresh_write;
        page.page_size = page_size;
        page.exec_prot = exec_prot;
        page.slot_count = slots;
        page.is_rx = fresh_write != nullptr;
        mark_slot(page, 0, true);
        pthread_mutex_unlock(&mutex_);
        return ExecSlot<SlotSize, MaxPages>(this, fresh_exec, fresh_exec,
                                            fresh_write, page_size, exec_prot,
                                            true, true);
      }
    }
    pthread_mutex_unlock(&mutex_);
    if (fresh_write != nullptr) {
      munmap(fresh_write, page_size);
      fresh_write = nullptr;
      (void)alloc_detail::raw_mprotect(fresh_exec, page_size,
                                       PROT_READ | PROT_WRITE);
    }
    return ExecSlot<SlotSize, MaxPages>(this, fresh_exec, fresh_exec, nullptr,
                                        page_size, exec_prot, false, true);
  }

  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  PoolPage pages_[MaxPages]{};
  RetiredSlot retired_[kMaxRetired]{};
  size_t retired_count_ = 0;
};

template <size_t SlotSize, size_t MaxPages>
inline void ExecSlot<SlotSize, MaxPages>::reset() {
  if (addr_ != nullptr) {
    if (pool_ != nullptr) {
      pool_->release_slot(addr_, pooled_);
    } else if (page_base_ != nullptr && page_size_ != 0) {
      munmap(page_base_, page_size_);
      if (write_base_ != nullptr)
        munmap(write_base_, page_size_);
    }
  }
  clear_fields();
}

using TrampolinePool = SlotPool<512, 32>;
using IslandPool = SlotPool<64, 16>;
using TrampolineSlot = ExecSlot<512, 32>;
using IslandSlot = ExecSlot<64, 16>;

static_assert(std::is_trivially_destructible_v<TrampolinePool>,
              "TrampolinePool must remain trivially destructible");
static_assert(std::is_trivially_destructible_v<IslandPool>,
              "IslandPool must remain trivially destructible");

[[nodiscard]] inline TrampolinePool &trampoline_pool() {
  static TrampolinePool pool;
  return pool;
}

[[nodiscard]] inline IslandPool &island_pool() {
  static IslandPool pool;
  return pool;
}

[[nodiscard]] inline bool pools_idle() {
  return trampoline_pool().idle() && island_pool().idle();
}

inline void drain_all_retired(bool force = false) {
  trampoline_pool().drain_retired(force);
  island_pool().drain_retired(force);
}

} // namespace yuki::ihook
