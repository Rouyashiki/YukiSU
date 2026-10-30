#include "soinfo_layout.hpp"
#include "check.hpp"

#include <unistd.h>

#include <array>

using namespace yuki::solist::layout;

namespace {
size_t g_ctor_off = 0;
#include "ctor_writer_under_test.inc"

constexpr size_t kWord = sizeof(void *);
constexpr size_t kBase = sizeof(void *) == 8 ? 16 : 140;
constexpr size_t kNext = kBase + (3 * kWord);
constexpr size_t kLinkMap = sizeof(void *) == 8 ? 208 : 284;

struct ElfImage {
  unsigned char *data = nullptr;
  size_t size;
  ElfW(Ehdr) * eh;
  ElfW(Phdr) * ph;

  static unsigned char *allocate(size_t bytes) {
    auto *mapped = static_cast<unsigned char *>(
        mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(mapped != MAP_FAILED);
    return mapped;
  }

  explicit ElfImage(size_t bytes, uintptr_t vaddr = 0)
      : data(allocate(bytes)), size(bytes),
        eh(reinterpret_cast<ElfW(Ehdr) *>(data)),
        ph(reinterpret_cast<ElfW(Phdr) *>(data + sizeof(*eh))) {
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = kWord == 8 ? ELFCLASS64 : ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_version = EV_CURRENT;
    eh->e_type = ET_DYN;
    eh->e_ehsize = sizeof(*eh);
    eh->e_phoff = sizeof(*eh);
    eh->e_phentsize = sizeof(*ph);
    eh->e_phnum = 3;
    ph[0].p_type = PT_PHDR;
    ph[0].p_vaddr = vaddr + sizeof(*eh);
    ph[0].p_filesz = ph[0].p_memsz = 3 * sizeof(*ph);
    ph[1].p_type = PT_LOAD;
    ph[1].p_vaddr = vaddr;
    ph[1].p_filesz = ph[1].p_memsz = size;
    ph[1].p_align = 4096;
    ph[2].p_type = PT_DYNAMIC;
    ph[2].p_vaddr = vaddr + 4096;
    ph[2].p_filesz = ph[2].p_memsz = sizeof(ElfW(Dyn));
  }
  ~ElfImage() { CHECK(munmap(data, size) == 0); }
  ElfImage(const ElfImage &) = delete;
  ElfImage &operator=(const ElfImage &) = delete;
  ElfImage(ElfImage &&) = delete;
  ElfImage &operator=(ElfImage &&) = delete;
};

struct Fixture {
  ElfImage main{0x242000};
  ElfImage linker{0x1f0000, 0x4000};
  alignas(void *) std::array<unsigned char, 1024> main_si{}, linker_si{},
      vdso_si{};
  char path[32] = "/system/bin/linker64";
  std::vector<yukizygisk::MapInfo> maps;
  Image mi{}, li{};

  static uintptr_t address(const void *p) {
    return reinterpret_cast<uintptr_t>(p);
  }

  void region(const void *p, size_t size) {
    maps.push_back({address(p),
                    address(p) + size,
                    PROT_READ | PROT_WRITE,
                    true,
                    0,
                    0,
                    0,
                    {}});
    std::sort(maps.begin(), maps.end(),
              [](const auto &a, const auto &b) { return a.start < b.start; });
  }

  template <typename T>
  void set(std::array<unsigned char, 1024> &si, size_t off, const T &value) {
    CHECK(off <= si.size() && sizeof(T) <= si.size() - off);
    memcpy(si.data() + off, &value, sizeof(value));
  }

  Fixture() {
    region(main.data, main.size);
    region(linker.data, linker.size);
    region(main_si.data(), main_si.size());
    region(linker_si.data(), linker_si.size());
    region(vdso_si.data(), vdso_si.size());
    region(path, sizeof(path));
    CHECK(main_image({maps}, address(main.ph), 3, 4096, mi));
    CHECK(linker_image({maps}, address(linker.data), 4096, li));
    CHECK(mi.size == 0x242000 && li.base == address(linker.data));
    CHECK(li.bias == li.base - 0x4000);
    const size_t header = sizeof(void *) == 8 ? 0 : 128;
    set(main_si, header, mi.phdr);
    set(main_si, header + kWord, mi.phnum);
    set(linker_si, header, li.phdr);
    set(linker_si, header + kWord, li.phnum);
    set(main_si, kBase, mi.base);
    set(main_si, kBase + kWord, mi.size);
    set(linker_si, kBase, li.base);
    set(linker_si, kBase + kWord, li.size);
    set(main_si, 112, size_t{679});
    set(linker_si, kNext, address(vdso_si.data()));
    link_map mm{}, lm{}, vm{};
    mm.l_addr = mi.bias;
    mm.l_ld = reinterpret_cast<ElfW(Dyn) *>(mi.dynamic);
    lm.l_addr = li.bias;
    lm.l_ld = reinterpret_cast<ElfW(Dyn) *>(li.dynamic);
    lm.l_name = path;
    lm.l_next = reinterpret_cast<link_map *>(vdso_si.data() + kLinkMap);
    vm.l_prev = reinterpret_cast<link_map *>(linker_si.data() + kLinkMap);
    set(main_si, kLinkMap, mm);
    set(linker_si, kLinkMap, lm);
    set(vdso_si, kLinkMap, vm);
    set(main_si, kLinkMap + sizeof(link_map), static_cast<unsigned char>(1));
    set(linker_si, kLinkMap + sizeof(link_map), static_cast<unsigned char>(1));
  }

  Result check(Offsets &offsets) {
    return probe({maps}, address(main_si.data()), address(linker_si.data()),
                 address(vdso_si.data()), mi, li, path, offsets);
  }
  void rejected(Result expected) {
    Offsets offsets{31, 32, 33, 34};
    CHECK(check(offsets) == expected);
    CHECK(offsets.base == 31 && offsets.size == 32 && offsets.next == 33 &&
          offsets.ctor == 34);
  }
};

void layout_cases() {
  {
    Fixture f;
    Offsets o;
    CHECK(f.check(o) == Result::Valid);
    CHECK(o.base == kBase && o.size == kBase + kWord && o.next == kNext &&
          o.ctor == kLinkMap + sizeof(link_map));
    f.set(f.main_si, 64, size_t{512});
    CHECK(f.check(o) == Result::Valid);
    f.mi.size = 0x10000;
    f.set(f.main_si, kBase + kWord, f.mi.size);
    CHECK(f.check(o) == Result::Valid);
    g_ctor_off = o.ctor;
    f.linker_si[o.ctor + 1] = 0xa5;
    u_set_ctor(f.linker_si.data(), false);
    CHECK(f.linker_si[o.ctor] == 0 && f.linker_si[o.ctor + 1] == 0xa5);
    u_set_ctor(f.linker_si.data(), true);
    CHECK(f.linker_si[o.ctor] == 1 && f.linker_si[o.ctor + 1] == 0xa5);
  }
  {
    Fixture f;
    f.set(f.linker_si, kBase, uintptr_t{0});
    f.set(f.linker_si, kBase + kWord, size_t{0});
    Offsets o;
    CHECK(f.check(o) == Result::Valid);
    CHECK(o.base == kBase && o.size == kBase + kWord);
    f.set(f.linker_si, sizeof(void *) == 8 ? 0 : 128, uintptr_t{0});
    f.rejected(Result::ImageMismatch);
  }
  {
    Fixture f;
    f.set(f.main_si, 512, f.mi.base);
    f.set(f.main_si, 512 + kWord, f.mi.size);
    f.set(f.linker_si, 512, f.li.base);
    f.set(f.linker_si, 512 + kWord, f.li.size);
    f.rejected(Result::ImageMismatch);
  }
  {
    Fixture f;
    f.set(f.linker_si, kBase + kWord, size_t{679});
    f.rejected(Result::ImageMismatch);
  }
  {
    Fixture f;
    memcpy(f.main_si.data() + 512, f.main_si.data() + kLinkMap,
           sizeof(link_map) + 1);
    memcpy(f.linker_si.data() + 512, f.linker_si.data() + kLinkMap,
           sizeof(link_map) + 1);
    f.rejected(Result::LinkMapMismatch);
  }
  {
    Fixture f;
    f.set(f.linker_si, kLinkMap + offsetof(link_map, l_name), uintptr_t{1});
    f.rejected(Result::LinkMapMismatch);
  }
  {
    Fixture f;
    f.set(f.linker_si, kLinkMap + sizeof(link_map),
          static_cast<unsigned char>(2));
    f.rejected(Result::LinkMapMismatch);
  }
  {
    Fixture f;
    f.set(f.linker_si, 512, Fixture::address(f.vdso_si.data()));
    f.rejected(Result::ListMismatch);
  }
  {
    Fixture f;
    // The linker follows main in the debugger list but precedes it in solist.
    f.set(f.linker_si, kNext, Fixture::address(f.main_si.data()));
    f.set(f.main_si, kNext, Fixture::address(f.vdso_si.data()));
    Offsets o;
    CHECK(f.check(o) == Result::Valid);
    CHECK(o.next == kNext);
  }
  {
    Fixture f;
    for (auto &m : f.maps)
      if (m.start == Fixture::address(f.vdso_si.data()))
        m.perms = PROT_NONE;
    f.rejected(Result::ListMismatch);
  }
  {
    Fixture f;
    f.set(f.linker_si, kNext, uintptr_t{1});
    f.rejected(Result::ListMismatch);
  }
  {
    Fixture f;
    for (auto &m : f.maps)
      if (m.start == Fixture::address(f.linker_si.data()))
        m.end = m.start + kLinkMap;
    f.rejected(Result::LinkMapMismatch);
  }
  {
    Fixture f;
    f.maps.clear();
    f.rejected(Result::ImageMismatch);
  }
}

void elf_cases() {
  Fixture f;
  Image image;
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 129, 4096, image));
  CHECK(!main_image({f.maps}, UINTPTR_MAX - 3, 3, 4096, image));
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 3, 0, image));
  f.main.ph[0].p_type = PT_NULL;
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 3, 4096, image));
  f.main.ph[0].p_type = PT_PHDR;
  f.main.ph[1].p_memsz = 1;
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 3, 4096, image));
  f.main.ph[1].p_memsz = f.main.ph[1].p_filesz;
  f.main.ph[1].p_vaddr = UINTPTR_MAX - 8;
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 3, 4096, image));
  f.main.ph[1].p_vaddr = 0;
  for (auto &map : f.maps)
    if (map.start == Fixture::address(f.main.data))
      map.end -= 4096;
  CHECK(!main_image({f.maps}, Fixture::address(f.main.ph), 3, 4096, image));
  f.linker.eh->e_ident[EI_CLASS] = 0;
  CHECK(!linker_image({f.maps}, Fixture::address(f.linker.data), 4096, image));
  uintptr_t value = 0;
  CHECK(!Memory{f.maps}.field(UINTPTR_MAX - 4, 8, value));
}
} // namespace

int main() {
  layout_cases();
  elf_cases();
  (void)printf("soinfo layout: large ELF, false/ambiguous fields, bounds and "
               "failure isolation passed (%zu-bit)\n",
               sizeof(void *) * 8);
}
