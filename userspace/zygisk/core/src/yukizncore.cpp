#include "core_bindings.hpp"
#include "hyos_runtime.hpp"
#include "inline_hook.hpp"
#include "load_config.hpp"
#include "loader_handoff.hpp"
#include "log.hpp"
#include "native_bionic.hpp"
#include "plt_lifecycle.hpp"
#include "solist.hpp"
#include "userspace/zygisk/endpoint.hpp"
#include "yukilinker.hpp"
#include "zygiskd.hpp"
#include <yukiplt.hpp>

#include "uapi/yukizygisk.h"
#include "zygisk_next_api.h"

#include "process_maps.hpp"

#include <android/dlext.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "xz.h"
}

extern "C" void xz_crc32_init(void) {}

extern "C" uint32_t xz_crc32(const uint8_t *buf, size_t size, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= buf[i];
    for (int bit = 0; bit < 8; ++bit) {
      uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

struct ResolvedSymbol {
  std::string name;
  void *addr = nullptr;
  size_t size = 0;
};

struct ZnSymbolResolver {
  std::string path;
  uintptr_t base = 0;
  std::vector<ResolvedSymbol> symbols;
};

namespace {

#define LOGE(...) ZLOGE(__VA_ARGS__)
#define LOGI(...) ZLOGI(__VA_ARGS__)

constexpr int kApiVersion3 = 3;
constexpr int kApiVersion4 = ZYGISK_NEXT_API_VERSION;
constexpr int kSuccess = ZN_SUCCESS;
constexpr int kFailed = ZN_FAILED;
constexpr uint32_t kHandleMagic = 0x595a4e31u;
constexpr size_t kMaxGnuDebugDataSize = size_t{32} * 1024 * 1024;
constexpr int kHyosSessionTimeoutMs = 2000;
constexpr int kHyosProbeTimeoutMs = 7000;
constexpr int kHyosProbeRetryMs = 50;

using ZdRequest = zygiskd::Request;

struct ModuleHandle {
  uint32_t magic = kHandleMagic;
  uint32_t index = 0;
  void *so = nullptr;
  uint64_t device = 0;
  uint64_t inode = 0;
  std::string module_id;
  bool early = false;
  bool has_companion = false;
  bool reported = false;
  bool hyos_registered = false;
};

struct InlineHookRecord {
  void *target = nullptr;
  yuki::ihook::Hook hook{};
};

std::vector<InlineHookRecord> g_inline_hooks;
pthread_mutex_t g_inline_hooks_mutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<ModuleHandle *> g_loaded_modules;
yz_config g_yz_config = yukizygisk::config::defaults;
uint32_t g_runtime_generation = 0;
uintptr_t g_loader_base = 0;
uintptr_t g_self_base = 0;
size_t g_self_size = 0;
bool g_loader_unmap_safe = false;
int g_early_packet_fd = -1;
bool g_hyos_probe_complete = false;

bool read_all(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool write_all(int fd, const void *buf, size_t n) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0)
      return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

int recv_fd(int sock, bool *message_received = nullptr) {
  char data = 0;
  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  iovec io{&data, 1};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  ssize_t result;
  do {
    result = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
  } while (result < 0 && errno == EINTR);
  if (result <= 0)
    return -1;
  if (message_received != nullptr)
    *message_received = true;
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      memcpy(&fd, CMSG_DATA(c), sizeof(fd));
      return fd;
    }
  return -1;
}

int connect_zygiskd() { return yukizygisk::endpoint::connect(); }

using HyosClock = std::chrono::steady_clock;
using HyosDeadline = HyosClock::time_point;

HyosDeadline make_hyos_deadline(int timeout_ms = kHyosSessionTimeoutMs) {
  return HyosClock::now() + std::chrono::milliseconds(timeout_ms);
}

bool wait_socket_event_until(int fd, short event,
                             const HyosDeadline &deadline) {
  for (;;) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              HyosClock::now());
    if (remaining.count() <= 0)
      return false;
    pollfd descriptor{fd, event, 0};
    const int result =
        poll(&descriptor, 1,
             static_cast<int>(std::max<int64_t>(1, remaining.count())));
    if (result < 0 && errno == EINTR)
      continue;
    return result == 1 && (descriptor.revents & event) != 0;
  }
}

bool send_packet_until(int fd, const void *buffer, size_t size,
                       const HyosDeadline &deadline) {
  const auto *data = static_cast<const uint8_t *>(buffer);
  while (size > 0) {
    const ssize_t sent = send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
      data += sent;
      size -= static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_socket_event_until(fd, POLLOUT, deadline))
      continue;
    return false;
  }
  return true;
}

bool receive_byte_until(int fd, uint8_t *value, const HyosDeadline &deadline) {
  for (;;) {
    const ssize_t received = recv(fd, value, sizeof(*value), MSG_DONTWAIT);
    if (received == sizeof(*value))
      return true;
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_socket_event_until(fd, POLLIN, deadline))
      continue;
    return false;
  }
}

int recv_fd_until(int sock, const HyosDeadline &deadline,
                  bool *message_received = nullptr) {
  for (;;) {
    char data = 0;
    char control[CMSG_SPACE(sizeof(int))] = {};
    iovec io{&data, sizeof(data)};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t result =
        recvmsg(sock, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (result > 0) {
      if (message_received != nullptr)
        *message_received = true;
      for (cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
           header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(sizeof(int)))
          continue;
        int received_fd = -1;
        memcpy(&received_fd, CMSG_DATA(header), sizeof(received_fd));
        return received_fd;
      }
      return -1;
    }
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_socket_event_until(sock, POLLIN, deadline))
      continue;
    return -1;
  }
}

int connect_zygiskd_for_hyos(const HyosDeadline &deadline) {
  return yukizygisk::endpoint::connect(deadline, true);
}

int request_fd(ZdRequest req, uint32_t arg) {
  int sock = connect_zygiskd();
  if (sock < 0)
    return -1;
  uint8_t op = static_cast<uint8_t>(req);
  bool ok = write_all(sock, &op, 1) && write_all(sock, &arg, sizeof(arg));
  int fd = ok ? recv_fd(sock) : -1;
  close(sock);
  return fd;
}

int open_hyos_control_session_once(const HyosDeadline &outer_deadline) {
  const HyosDeadline deadline = std::min(outer_deadline, make_hyos_deadline());
  const int socket = connect_zygiskd_for_hyos(deadline);
  if (socket < 0)
    return -1;
  const uint8_t op = static_cast<uint8_t>(ZdRequest::OpenHyosControlSession);
  const int session = send_packet_until(socket, &op, sizeof(op), deadline)
                          ? recv_fd_until(socket, deadline)
                          : -1;
  close(socket);
  return session;
}

int open_hyos_control_session() {
  if (g_hyos_probe_complete)
    return open_hyos_control_session_once(make_hyos_deadline());

  using Clock = std::chrono::steady_clock;
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(kHyosProbeTimeoutMs);
  for (;;) {
    const int session = open_hyos_control_session_once(deadline);
    if (session >= 0) {
      g_hyos_probe_complete = true;
      return session;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    if (remaining.count() <= 0)
      return -1;
    const int delay = static_cast<int>(
        std::min<int64_t>(remaining.count(), kHyosProbeRetryMs));
    int result;
    do {
      result = poll(nullptr, 0, delay);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return -1;
  }
}

int request_fd_from_hyos_session(ZdRequest request, uint32_t argument) {
  if (!yukizygisk::hyos::in_specialized_child())
    return -1;
  uint8_t frame[sizeof(uint8_t) + sizeof(argument)];
  frame[0] = static_cast<uint8_t>(request);
  memcpy(frame + sizeof(uint8_t), &argument, sizeof(argument));
  yukizygisk::hyos::lock_child_control_session();
  const int session = yukizygisk::hyos::child_control_session();
  if (session < 0) {
    yukizygisk::hyos::unlock_child_control_session();
    return -1;
  }
  const HyosDeadline deadline = make_hyos_deadline();
  bool response_received = false;
  const bool sent = send_packet_until(session, frame, sizeof(frame), deadline);
  const int fd =
      sent ? recv_fd_until(session, deadline, &response_received) : -1;
  if (!sent || !response_received)
    yukizygisk::hyos::invalidate_child_control_session();
  yukizygisk::hyos::unlock_child_control_session();
  return fd;
}

bool report_hyos_callback(uint32_t module_index) {
  if (!yukizygisk::hyos::in_specialized_child())
    return false;
  uint8_t frame[sizeof(uint8_t) + sizeof(module_index)];
  frame[0] = static_cast<uint8_t>(ZdRequest::ReportHyosCallback);
  memcpy(frame + sizeof(uint8_t), &module_index, sizeof(module_index));
  yukizygisk::hyos::lock_child_control_session();
  const int session = yukizygisk::hyos::child_control_session();
  if (session < 0) {
    yukizygisk::hyos::unlock_child_control_session();
    return false;
  }
  const HyosDeadline deadline = make_hyos_deadline();
  uint8_t ack = 0;
  const bool sent = send_packet_until(session, frame, sizeof(frame), deadline);
  const bool received = sent && receive_byte_until(session, &ack, deadline);
  if (!sent || !received)
    yukizygisk::hyos::invalidate_child_control_session();
  yukizygisk::hyos::unlock_child_control_session();
  return received && ack != 0;
}

void load_config() {
  if (g_early_packet_fd >= 0)
    g_yz_config = yukizygisk::config::early_config(g_early_packet_fd);
  (void)yukizygisk::config::read_runtime(&g_yz_config);
}

void restore_native_load_policy() {
  int s = connect_zygiskd();
  if (s < 0) {
    LOGE("native load policy restore: zygiskd unavailable");
    return;
  }
  uint8_t op = static_cast<uint8_t>(ZdRequest::RestoreLoadPolicy);
  uint8_t ok = 0;
  bool sent = write_all(s, &op, 1) && read_all(s, &ok, sizeof(ok));
  close(s);
  if (!sent)
    LOGE("native load policy restore: request failed");
  else
    LOGI("native load policy restore: ok=%u", ok ? 1U : 0U);
}

std::string basename_of(std::string_view path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string_view::npos)
    return std::string(path);
  return std::string(path.substr(pos + 1));
}

std::string clean_map_path(const std::string &path) {
  constexpr char kDeleted[] = " (deleted)";
  if (path.size() > sizeof(kDeleted) - 1 &&
      path.compare(path.size() - (sizeof(kDeleted) - 1), sizeof(kDeleted) - 1,
                   kDeleted) == 0)
    return path.substr(0, path.size() - (sizeof(kDeleted) - 1));
  return path;
}

std::string self_exe_path() {
  char buf[512];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return {};
  buf[n] = '\0';
  return clean_map_path(buf);
}

bool target_matches(const zygiskd::NativeModuleInfo &info,
                    const std::string &exe_path, const std::string &exe_base) {
  if (info.target[0] == '\0')
    return false;
  if (info.target_type == YZ_NATIVE_TARGET_NAME)
    return exe_base == info.target;
  if (info.target_type == YZ_NATIVE_TARGET_PATH)
    return exe_path == info.target;
  return false;
}

bool request_native_info(uint32_t idx, zygiskd::NativeModuleInfo *info) {
  int sock = connect_zygiskd();
  if (sock < 0) {
    LOGE("native module info: zygiskd unavailable idx=%u", idx);
    return false;
  }
  uint8_t op = static_cast<uint8_t>(ZdRequest::GetNativeModuleInfo);
  bool ok = write_all(sock, &op, 1) && write_all(sock, &idx, sizeof(idx)) &&
            read_all(sock, info, sizeof(*info));
  close(sock);
  if (!ok)
    LOGE("native module info: request failed idx=%u", idx);
  return ok;
}

uint32_t request_runtime_generation() {
  int sock = connect_zygiskd();
  if (sock < 0)
    return 0;
  const uint8_t op = static_cast<uint8_t>(ZdRequest::GetRuntimeGeneration);
  const uint8_t kind = YZ_RUNTIME_KIND_NATIVE;
  uint32_t generation = 0;
  if (!write_all(sock, &op, sizeof(op)) ||
      !write_all(sock, &kind, sizeof(kind)) ||
      !read_all(sock, &generation, sizeof(generation)))
    generation = 0;
  close(sock);
  return generation;
}

bool report_native_injection(uint32_t idx) {
  if (g_runtime_generation == 0)
    g_runtime_generation = request_runtime_generation();
  if (g_runtime_generation == 0)
    return false;
  int sock = connect_zygiskd();
  if (sock < 0) {
    LOGE("native injection report: zygiskd unavailable idx=%u", idx);
    return false;
  }
  uint8_t op = static_cast<uint8_t>(ZdRequest::ReportNativeInjection);
  uint8_t ok = 0;
  bool sent =
      write_all(sock, &op, 1) && write_all(sock, &idx, sizeof(idx)) &&
      write_all(sock, &g_runtime_generation, sizeof(g_runtime_generation)) &&
      read_all(sock, &ok, sizeof(ok));
  close(sock);
  if (!sent)
    LOGE("native injection report: request failed idx=%u", idx);
  else
    LOGI("native injection report: idx=%u ok=%u", idx, ok ? 1U : 0U);
  return sent && ok != 0;
}

ModuleHandle *find_loaded_module(std::string_view module_id) {
  for (auto *h : g_loaded_modules) {
    if (h != nullptr && h->module_id == module_id)
      return h;
  }
  return nullptr;
}

bool has_pending_early_report() {
  for (auto *h : g_loaded_modules) {
    if (h != nullptr && h->early && !h->reported &&
        (!yukizygisk::hyos::available() || h->hyos_registered))
      return true;
  }
  return false;
}

bool map_from_base(void *base_addr, yukizygisk::MapInfo *out) {
  uintptr_t base = reinterpret_cast<uintptr_t>(base_addr);
  auto maps = yukizygisk::hyos::in_specialized_child()
                  ? yukizygisk::MapInfo::Scan()
                  : yukizygisk::MapInfo::Scan();
  if (base == 0) {
    std::string exe = self_exe_path();
    for (const auto &m : maps) {
      if (m.inode != 0 && m.offset == 0 && clean_map_path(m.path) == exe) {
        *out = m;
        return true;
      }
    }
    return false;
  }
  for (const auto &m : maps) {
    uintptr_t load_base = m.start - m.offset;
    if ((base >= m.start && base < m.end) || base == m.start ||
        base == load_base) {
      if (m.inode == 0)
        continue;
      for (const auto &h : maps) {
        if (h.inode == m.inode && h.dev == m.dev && h.offset == 0) {
          *out = h;
          return true;
        }
      }
      *out = m;
      return true;
    }
  }
  return false;
}

bool path_matches_map(const std::string &want, const std::string &map_path) {
  if (want.empty())
    return false;
  std::string clean = clean_map_path(map_path);
  if (clean == want)
    return true;
  if (basename_of(clean) == want)
    return true;
  if (want[0] == '/')
    return false;
  return clean.size() > want.size() &&
         clean.compare(clean.size() - want.size(), want.size(), want) == 0 &&
         clean[clean.size() - want.size() - 1] == '/';
}

bool find_library_map(const char *path, void *base_addr,
                      yukizygisk::MapInfo *out, uintptr_t *load_base) {
  if (base_addr != nullptr && map_from_base(base_addr, out)) {
    *load_base = out->start - out->offset;
    return true;
  }

  std::string want = path != nullptr ? path : "";
  if (want == "linker" || want == "linker64") {
    void *linker_base = reinterpret_cast<void *>(getauxval(AT_BASE));
    if (linker_base != nullptr && map_from_base(linker_base, out)) {
      *load_base = out->start - out->offset;
      return true;
    }
  }

  auto maps = yukizygisk::hyos::in_specialized_child()
                  ? yukizygisk::MapInfo::Scan()
                  : yukizygisk::MapInfo::Scan();
  for (const auto &m : maps) {
    if (m.inode == 0 || m.offset != 0)
      continue;
    if (!path_matches_map(want, m.path))
      continue;
    *out = m;
    *load_base = m.start - m.offset;
    return true;
  }
  return false;
}

bool elf_range_ok(size_t file_size, uint64_t off, uint64_t size) {
  return off <= file_size && size <= file_size - off;
}

bool elf_image_ok(const uint8_t *image, size_t file_size,
                  const ElfW(Ehdr) * *eh_out,
                  const ElfW(Shdr) * *sections_out) {
  if (image == nullptr || file_size < sizeof(ElfW(Ehdr)))
    return false;
  const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(image);
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] !=
          (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      eh->e_shoff == 0 || eh->e_shnum == 0 ||
      eh->e_shentsize != sizeof(ElfW(Shdr)))
    return false;
  uint64_t table_size = static_cast<uint64_t>(eh->e_shnum) *
                        static_cast<uint64_t>(eh->e_shentsize);
  if (!elf_range_ok(file_size, eh->e_shoff, table_size))
    return false;
  *eh_out = eh;
  *sections_out = reinterpret_cast<const ElfW(Shdr) *>(image + eh->e_shoff);
  return true;
}

bool section_name_equals(const uint8_t *image, size_t file_size,
                         const ElfW(Ehdr) * eh, const ElfW(Shdr) * sections,
                         const ElfW(Shdr) * sh, const char *want) {
  if (eh->e_shstrndx == SHN_UNDEF || eh->e_shstrndx >= eh->e_shnum)
    return false;
  const ElfW(Shdr) *names = &sections[eh->e_shstrndx];
  if (!elf_range_ok(file_size, names->sh_offset, names->sh_size) ||
      sh->sh_name >= names->sh_size)
    return false;
  const char *name =
      reinterpret_cast<const char *>(image + names->sh_offset + sh->sh_name);
  size_t limit = static_cast<size_t>(names->sh_size - sh->sh_name);
  size_t want_len = strlen(want);
  return limit > want_len && memcmp(name, want, want_len) == 0 &&
         name[want_len] == '\0';
}

void add_symbols_from_section(ZnSymbolResolver *r, const uint8_t *image,
                              size_t file_size, const ElfW(Ehdr) * eh,
                              const ElfW(Shdr) * sections,
                              const ElfW(Shdr) * sh) {
  if (sh->sh_entsize != sizeof(ElfW(Sym)) || sh->sh_link >= eh->e_shnum)
    return;
  const ElfW(Shdr) *str = &sections[sh->sh_link];
  if (!elf_range_ok(file_size, sh->sh_offset, sh->sh_size) ||
      !elf_range_ok(file_size, str->sh_offset, str->sh_size) ||
      str->sh_size == 0)
    return;
  const char *strtab = reinterpret_cast<const char *>(image + str->sh_offset);
  const auto *sym = reinterpret_cast<const ElfW(Sym) *>(image + sh->sh_offset);
  size_t count = sh->sh_size / sh->sh_entsize;
  for (size_t i = 0; i < count; ++i) {
    if (sym[i].st_name == 0 || sym[i].st_shndx == SHN_UNDEF ||
        sym[i].st_value == 0)
      continue;
    if (sym[i].st_name >= str->sh_size)
      continue;
    const char *name = strtab + sym[i].st_name;
    size_t name_limit = static_cast<size_t>(str->sh_size - sym[i].st_name);
    size_t name_len = strnlen(name, name_limit);
    if (name_len == 0 || name_len == name_limit)
      continue;
    uintptr_t addr =
        eh->e_type == ET_EXEC ? sym[i].st_value : r->base + sym[i].st_value;
    r->symbols.push_back(ResolvedSymbol{std::string(name),
                                        reinterpret_cast<void *>(addr),
                                        static_cast<size_t>(sym[i].st_size)});
  }
}

bool decompress_xz(const uint8_t *data, size_t size,
                   std::vector<uint8_t> *out) {
  static constexpr uint8_t kXzMagic[] = {0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00};
  if (data == nullptr || out == nullptr || size < sizeof(kXzMagic) ||
      memcmp(data, kXzMagic, sizeof(kXzMagic)) != 0)
    return false;

  xz_dec *dec = xz_dec_init(XZ_DYNALLOC, kMaxGnuDebugDataSize);
  if (dec == nullptr)
    return false;

  out->clear();
  out->resize(size_t{64} * 1024);
  xz_buf b{};
  b.in = data;
  b.in_size = size;
  b.out = out->data();
  b.out_size = out->size();

  bool ok = false;
  for (;;) {
    xz_ret ret = xz_dec_run(dec, &b);
    if (ret == XZ_STREAM_END) {
      out->resize(b.out_pos);
      ok = true;
      break;
    }
    if (ret == XZ_UNSUPPORTED_CHECK)
      continue;
    if (ret != XZ_OK)
      break;
    if (b.out_pos == out->size()) {
      if (out->size() >= kMaxGnuDebugDataSize)
        break;
      const size_t next = std::min(out->size() * 2, kMaxGnuDebugDataSize);
      out->resize(next);
      b.out = out->data();
      b.out_size = out->size();
      continue;
    }
    if (b.in_pos == b.in_size)
      break;
  }

  xz_dec_end(dec);
  if (!ok)
    out->clear();
  return ok;
}

bool load_symbols_from_image(ZnSymbolResolver *r, const uint8_t *image,
                             size_t size, bool parse_gnu_debugdata) {
  const ElfW(Ehdr) *eh = nullptr;
  const ElfW(Shdr) *sections = nullptr;
  if (!elf_image_ok(image, size, &eh, &sections))
    return false;

  size_t before = r->symbols.size();
  const ElfW(Shdr) *gnu_debugdata = nullptr;

  for (size_t i = 0; i < eh->e_shnum; ++i) {
    const ElfW(Shdr) *sh = &sections[i];
    if ((sh->sh_type == SHT_DYNSYM || sh->sh_type == SHT_SYMTAB)) {
      add_symbols_from_section(r, image, size, eh, sections, sh);
      continue;
    }
    if (!parse_gnu_debugdata || sh->sh_type != SHT_PROGBITS)
      continue;
    if (section_name_equals(image, size, eh, sections, sh, ".gnu_debugdata"))
      gnu_debugdata = sh;
  }

  if (gnu_debugdata != nullptr &&
      elf_range_ok(size, gnu_debugdata->sh_offset, gnu_debugdata->sh_size)) {
    std::vector<uint8_t> debug_elf;
    const auto *debug_xz = image + gnu_debugdata->sh_offset;
    if (decompress_xz(debug_xz, static_cast<size_t>(gnu_debugdata->sh_size),
                      &debug_elf)) {
      (void)load_symbols_from_image(r, debug_elf.data(), debug_elf.size(),
                                    false);
      LOGI("gnu_debugdata parsed: %s symbols=%zu", r->path.c_str(),
           r->symbols.size() - before);
    } else {
      LOGE("failed to initialize gnu_debugdata: %s", r->path.c_str());
    }
  }

  return r->symbols.size() != before;
}

bool load_symbols(ZnSymbolResolver *r, const std::string &file_path) {
  int fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return false;
  }
  size_t sz = static_cast<size_t>(st.st_size);
  void *map = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return false;

  const auto *image = static_cast<const uint8_t *>(map);
  bool ok = load_symbols_from_image(r, image, sz, true);
  munmap(map, sz);
  return ok;
}

struct NativePltBinding {
  yukiplt::NamedImage image;
  std::string symbol;
  void *replacement = nullptr;
  void *original = nullptr;
  std::vector<void **> outputs;
  yukiplt::HookHandle handle;
  yukiplt::SlotReport slot;
  yukiplt::OperationDiagnostic install;
  yukiplt::OperationDiagnostic restore;
  bool recovery = false;
};
std::vector<std::unique_ptr<NativePltBinding>> g_native_plt;

bool same_native_image(const yukiplt::NamedImage &left,
                       const yukiplt::NamedImage &right) {
  return left.image.load_bias == right.image.load_bias &&
         left.image.phdr_table == right.image.phdr_table &&
         left.image.phdr_count == right.image.phdr_count &&
         left.image.instance_token == right.image.instance_token &&
         left.path == right.path;
}

yukiplt::Result<yukiplt::NamedImage> native_plt_image(void *address) {
  if (address != nullptr) {
    auto containing = yukiplt::find_image_containing(address);
    if (containing ||
        containing.error().code != yukiplt::ErrorCode::kImageNotFound)
      return containing;
  }
  auto images = yukiplt::find_images(
      {address == nullptr ? yukiplt::ImageMatch::kMainExecutable
                          : yukiplt::ImageMatch::kLoadBias,
       nullptr, reinterpret_cast<uintptr_t>(address)});
  if (!images)
    return images.error();
  if (images.value().size() != 1)
    return yukiplt::Error::Make(yukiplt::ErrorCode::kAmbiguousImage);
  return std::move(images.value().front());
}

int api_plt_hook(void *base_addr, const char *symbol, void *hook_handler,
                 void **original) noexcept {
  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation || symbol == nullptr || *symbol == '\0' ||
      hook_handler == nullptr ||
      (original != nullptr &&
       reinterpret_cast<uintptr_t>(original) % alignof(void *) != 0))
    return kFailed;
  try {
    auto image = native_plt_image(base_addr);
    if (!image) {
      LOGE("PLT discovery: %s errno=%d",
           yukiplt::error_name(image.error().code), image.error().sys_errno);
      return kFailed;
    }
    NativePltBinding *existing = nullptr;
    for (const auto &binding : g_native_plt) {
      if (same_native_image(binding->image, image.value()) &&
          binding->symbol == symbol && binding->handle.valid()) {
        if (binding->replacement != hook_handler || binding->recovery)
          return kFailed;
        existing = binding.get();
      }
    }
    if (original != nullptr) {
      if (!yukizygisk::hyos::original_output_allowed(original))
        return kFailed;
      for (const auto &binding : g_native_plt) {
        if (original == &binding->original)
          return kFailed;
        if (binding.get() != existing &&
            std::find(binding->outputs.begin(), binding->outputs.end(),
                      original) != binding->outputs.end())
          return kFailed;
        if (binding->handle.valid()) {
          yukiplt::SlotReport slot;
          size_t count = 0;
          if (!yukiplt::query(binding->handle, &slot, 1, &count).ok() ||
              count != 1 ||
              slot.address == reinterpret_cast<uintptr_t>(original))
            return kFailed;
        }
      }
    }
    if (existing != nullptr) {
      yukiplt::SlotReport current;
      size_t count = 0;
      const auto error = yukiplt::query(existing->handle, &current, 1, &count);
      if (!error.ok() || count != 1 ||
          current.status != yukiplt::SlotStatus::kInstalled ||
          current.value_observed != reinterpret_cast<uintptr_t>(hook_handler))
        return kFailed;
      if (original != nullptr) {
        if (std::find(existing->outputs.begin(), existing->outputs.end(),
                      original) == existing->outputs.end())
          existing->outputs.push_back(original);
        __atomic_store_n(original, yukiplt::load_original(&existing->original),
                         __ATOMIC_RELEASE);
      }
      return kSuccess;
    }
    auto binding = std::make_unique<NativePltBinding>();
    binding->image = std::move(image.value());
    binding->symbol = symbol;
    binding->replacement = hook_handler;
    if (original != nullptr)
      binding->outputs.push_back(original);
    g_native_plt.reserve(g_native_plt.size() + 1);
    const yukiplt::HookRequest request{
        binding->symbol.c_str(), hook_handler,
        original != nullptr ? original : &binding->original};
    auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
        binding->image, &request, 1, &binding->slot, &binding->install);
    binding->handle = std::move(result.handle);
    const bool published =
        binding->install.stage >= yukiplt::OperationStage::kCommit;
    if (published && original != nullptr)
      __atomic_store_n(&binding->original, yukiplt::load_original(original),
                       __ATOMIC_RELEASE);
    yukizygisk::plt_lifecycle::set_recovery(
        binding->recovery, !result.ok() && binding->handle.valid());
    LOGI("PLT hook: path=%s symbol=%s result=%s cause=%s errno=%d recovery=%d",
         binding->image.path.c_str(), symbol,
         yukiplt::error_name(result.error.code),
         yukiplt::error_cause_name(binding->install.cause),
         result.error.sys_errno, binding->recovery);
    if (binding->handle.valid() || published)
      g_native_plt.push_back(std::move(binding));
    return result.ok() ? kSuccess : kFailed;
  } catch (const std::bad_alloc &) {
    LOGE("PLT preparation: OutOfMemory");
  } catch (...) {
    LOGE("PLT preparation failed");
  }
  return kFailed;
}

void recover_native_plt() {
  yukizygisk::plt_lifecycle::Operation operation(
      yukizygisk::plt_lifecycle::Access::Recover);
  if (!operation)
    return;
  for (auto &binding : g_native_plt) {
    if (!binding->recovery)
      continue;
    yukiplt::SlotReport slot;
    const auto error =
        yukiplt::unhook_diagnosed(binding->handle, &slot, 1, &binding->restore);
    yukizygisk::plt_lifecycle::set_recovery(binding->recovery,
                                            binding->handle.valid());
    LOGI("native PLT recovery: symbol=%s result=%s errno=%d consumed=%d",
         binding->symbol.c_str(), yukiplt::error_name(error.code),
         error.sys_errno, binding->restore.handle_consumed);
  }
}

int api_inline_hook(void *target, void *addr, void **original) {
  if (target == nullptr || addr == nullptr ||
      !yuki::ihook::single_threaded_process()) {
    LOGE("inline hook: invalid args target=%p handler=%p", target, addr);
    return kFailed;
  }
  pthread_mutex_lock(&g_inline_hooks_mutex);
  const uintptr_t requested =
      reinterpret_cast<uintptr_t>(target) & ~uintptr_t{1};
#if defined(__aarch64__)
  constexpr size_t requested_size = 8;
#else
  constexpr size_t requested_size = 10;
#endif
  for (const auto &rec : g_inline_hooks) {
    const uintptr_t existing =
        reinterpret_cast<uintptr_t>(rec.target) & ~uintptr_t{1};
#if defined(__aarch64__)
    constexpr size_t existing_size = 8;
#else
    const size_t existing_size = rec.hook.patched_size;
#endif
    if (yuki::ihook::hook_ranges_overlap(requested, requested_size, existing,
                                         existing_size)) {
      LOGE("inline hook: overlapping target=%p existing=%p", target,
           rec.target);
      pthread_mutex_unlock(&g_inline_hooks_mutex);
      return kFailed;
    }
  }

  g_inline_hooks.emplace_back();
  InlineHookRecord &rec = g_inline_hooks.back();
  rec.target = target;
#if defined(__aarch64__)
  yuki::ihook::a64::Error reason = yuki::ihook::a64::Error::None;
  void *orig =
      yuki::ihook::install(target, addr, &rec.hook, true, false, &reason);
#else
  void *orig = yuki::ihook::install(target, addr, &rec.hook, true);
#endif
  if (orig == nullptr) {
#if defined(__aarch64__)
    LOGE("inline hook: install failed target=%p handler=%p reason=%s", target,
         addr, yuki::ihook::a64::error_name(reason));
#else
    LOGE("inline hook: install failed target=%p handler=%p", target, addr);
#endif
    g_inline_hooks.pop_back();
    pthread_mutex_unlock(&g_inline_hooks_mutex);
    return kFailed;
  }
  if (original != nullptr)
    *original = orig;
  pthread_mutex_unlock(&g_inline_hooks_mutex);
  LOGI("inline hook: installed target=%p handler=%p original=%p", target, addr,
       orig);
  return kSuccess;
}

int api_inline_unhook(void *target) {
  if (target == nullptr || !yuki::ihook::single_threaded_process())
    return kFailed;
  pthread_mutex_lock(&g_inline_hooks_mutex);
  for (auto it = g_inline_hooks.begin(); it != g_inline_hooks.end(); ++it) {
    if (it->target != target)
      continue;
    if (!yuki::ihook::uninstall(&it->hook)) {
      pthread_mutex_unlock(&g_inline_hooks_mutex);
      return kFailed;
    }
    g_inline_hooks.erase(it);
    pthread_mutex_unlock(&g_inline_hooks_mutex);
    return kSuccess;
  }
  pthread_mutex_unlock(&g_inline_hooks_mutex);
  return kFailed;
}

ZnSymbolResolver *api_new_symbol_resolver(const char *path, void *base_addr) {
  yukizygisk::MapInfo info{};
  uintptr_t base = 0;
  if (!find_library_map(path, base_addr, &info, &base)) {
    LOGE("symbol resolver: map not found path=%s base=%p",
         path != nullptr ? path : "(self)", base_addr);
    return nullptr;
  }

  auto *r = new ZnSymbolResolver();
  r->path = clean_map_path(info.path);
  r->base = base;
  if (!load_symbols(r, r->path)) {
    LOGE("symbol resolver: no symbols path=%s base=0x%lx", r->path.c_str(),
         static_cast<unsigned long>(r->base));
    delete r;
    return nullptr;
  }
  LOGI("symbol resolver: path=%s base=0x%lx symbols=%zu", r->path.c_str(),
       static_cast<unsigned long>(r->base), r->symbols.size());
  return r;
}

void api_free_symbol_resolver(ZnSymbolResolver *resolver) { delete resolver; }

void *api_get_base_address(ZnSymbolResolver *resolver) {
  return resolver == nullptr ? nullptr
                             : reinterpret_cast<void *>(resolver->base);
}

void *api_symbol_lookup(ZnSymbolResolver *resolver, const char *name,
                        bool prefix, size_t *size) {
  if (resolver == nullptr || name == nullptr)
    return nullptr;
  size_t n = strlen(name);
  for (const auto &sym : resolver->symbols) {
    bool match = prefix ? sym.name.compare(0, n, name) == 0 : sym.name == name;
    if (!match)
      continue;
    if (size != nullptr)
      *size = sym.size;
    LOGI("symbol lookup: hit path=%s name=%s prefix=%u addr=%p size=%zu",
         resolver->path.c_str(), name, prefix ? 1U : 0U, sym.addr, sym.size);
    return sym.addr;
  }
  LOGE("symbol lookup: miss path=%s name=%s prefix=%u symbols=%zu",
       resolver->path.c_str(), name, prefix ? 1U : 0U,
       resolver->symbols.size());
  return nullptr;
}

void api_for_each_symbols(ZnSymbolResolver *resolver,
                          bool (*callback)(const char *name, void *addr,
                                           size_t size, void *data),
                          void *data) {
  if (resolver == nullptr || callback == nullptr)
    return;
  for (const auto &sym : resolver->symbols)
    if (!callback(sym.name.c_str(), sym.addr, sym.size, data))
      break;
}

int api_connect_companion(void *handle) {
  auto *h = static_cast<ModuleHandle *>(handle);
  if (h == nullptr || h->magic != kHandleMagic) {
    LOGE("connect companion: invalid handle=%p", handle);
    return -1;
  }
  if (h->early && !h->has_companion) {
    LOGE("connect companion: unavailable for early module id=%s",
         h->module_id.c_str());
    return -1;
  }
  const bool hyos_child = yukizygisk::hyos::in_specialized_child();
  int fd = hyos_child ? request_fd_from_hyos_session(
                            ZdRequest::ConnectNativeCompanion, h->index)
                      : request_fd(ZdRequest::ConnectNativeCompanion, h->index);
  if (!hyos_child)
    LOGI("connect companion: idx=%u fd=%d", h->index, fd);
  return fd;
}

const ZygiskNextRuntime *api_get_runtime() {
  return yukizygisk::hyos::runtime();
}

const ZygiskNextAPI g_api = {
    .pltHook = api_plt_hook,
    .inlineHook = api_inline_hook,
    .inlineUnhook = api_inline_unhook,
    .newSymbolResolver = api_new_symbol_resolver,
    .freeSymbolResolver = api_free_symbol_resolver,
    .getBaseAddress = api_get_base_address,
    .symbolLookup = api_symbol_lookup,
    .forEachSymbols = api_for_each_symbols,
    .connectCompanion = api_connect_companion,
    .getRuntime = api_get_runtime,
};

bool request_native_module_count(uint32_t *count, bool quiet) {
  int sock = connect_zygiskd();
  if (sock < 0) {
    if (!quiet)
      LOGE("native core: zygiskd unavailable");
    return false;
  }
  uint8_t op = static_cast<uint8_t>(ZdRequest::GetNativeModuleCount);
  uint32_t value = 0;
  if (!write_all(sock, &op, 1) || !read_all(sock, &value, sizeof(value))) {
    close(sock);
    if (!quiet)
      LOGE("native core: module count request failed");
    return false;
  }
  close(sock);
  *count = value;
  return true;
}

std::string bounded_cstr(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n] != '\0')
    n++;
  return {s, n};
}

std::string module_id_of(const zygiskd::NativeModuleInfo &info) {
  return bounded_cstr(info.module_id, sizeof(info.module_id));
}

bool load_native_module_from_fd(const zygiskd::NativeModuleInfo &info,
                                uint32_t idx, int lib_fd, bool early) {
  std::string module_id = module_id_of(info);
  std::string lib_path = bounded_cstr(info.lib_path, sizeof(info.lib_path));
  if (module_id.empty() || lib_path.empty()) {
    if (lib_fd >= 0)
      close(lib_fd);
    LOGE("native core: invalid module info idx=%u early=%u", idx,
         early ? 1U : 0U);
    return false;
  }

  if (find_loaded_module(module_id) != nullptr) {
    if (lib_fd >= 0)
      close(lib_fd);
    LOGI("native core: already initialized id=%s", module_id.c_str());
    return true;
  }
  if (lib_fd < 0) {
    LOGE("native core: invalid library fd id=%s idx=%u", module_id.c_str(),
         idx);
    return false;
  }

  struct stat identity{};
  if (fstat(lib_fd, &identity) != 0) {
    const int error = errno;
    LOGE("native core: library identity failed id=%s idx=%u: %s",
         module_id.c_str(), idx, strerror(error));
    close(lib_fd);
    return false;
  }
  for (const auto *loaded : g_loaded_modules) {
    if (loaded->device == identity.st_dev && loaded->inode == identity.st_ino) {
      LOGE("native core: library fd already initialized by id=%s, rejecting "
           "id=%s",
           loaded->module_id.c_str(), module_id.c_str());
      close(lib_fd);
      return false;
    }
  }

  // Use the full path and fd identity; FORCE_LOAD would bypass Bionic reuse.
  android_dlextinfo ext{};
  ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  ext.library_fd = lib_fd;
  const auto load = yukizygisk::native_bionic::load(lib_path.c_str(),
                                                    RTLD_NOW | RTLD_LOCAL, ext);
  const auto &context = load.context;
  LOGI("native caller: id=%s pid=%d early=%u stage=%s "
       "reason=%s errno=%d linker=%s bias=%p libc=%s bias=%p "
       "libc_dev=%llu libc_ino=%llu anchor=%p end=%p",
       module_id.c_str(), getpid(), early ? 1U : 0U,
       yukizygisk::native_bionic::stage_name(load.failure.stage),
       yukizygisk::native_bionic::error_name(load.failure.error),
       load.failure.system_error, context.api.linker.path.c_str(),
       reinterpret_cast<void *>(context.api.linker.bias),
       context.libc.path.c_str(), reinterpret_cast<void *>(context.libc.bias),
       static_cast<unsigned long long>(context.libc.identity.device),
       static_cast<unsigned long long>(context.libc.identity.inode),
       reinterpret_cast<void *>(context.anchor),
       reinterpret_cast<void *>(context.segment_end));
  void *so = load.handle;
  if (so == nullptr) {
    LOGE("native core: Bionic load failed id=%s idx=%u path=%s early=%u: %s",
         module_id.c_str(), idx, lib_path.c_str(), early ? 1U : 0U,
         load.loader_error.empty()
             ? yukizygisk::native_bionic::error_name(load.failure.error)
             : load.loader_error.c_str());
    close(lib_fd);
    return false;
  }
  close(lib_fd);
  for (const auto *loaded : g_loaded_modules) {
    if (loaded->so == so) {
      LOGE("native core: library already initialized by id=%s, rejecting id=%s",
           loaded->module_id.c_str(), module_id.c_str());
      dlclose(so);
      return false;
    }
  }
  LOGI("native core: Bionic loaded id=%s idx=%u path=%s early=%u "
       "caller=libc mappings=bionic",
       module_id.c_str(), idx, lib_path.c_str(), early ? 1U : 0U);
  // Native modules retain Bionic mappings and identity independently of Core
  // anonymity.

  dlerror();
  auto *mod = reinterpret_cast<ZygiskNextModule *>(dlsym(so, "zn_module"));
  if (mod == nullptr) {
    const char *error = dlerror();
    LOGE("native core: zn_module lookup failed id=%s idx=%u: %s",
         module_id.c_str(), idx, error != nullptr ? error : "null entry");
    dlclose(so);
    return false;
  }
  if ((mod->target_api_version != kApiVersion3 &&
       mod->target_api_version != kApiVersion4) ||
      mod->onModuleLoaded == nullptr) {
    LOGE("native core: unsupported module id=%s idx=%u api=%d entry=%p",
         module_id.c_str(), idx, mod->target_api_version, mod->onModuleLoaded);
    dlclose(so);
    return false;
  }

  auto *handle = new ModuleHandle();
  handle->index = idx;
  handle->so = so;
  handle->device = identity.st_dev;
  handle->inode = identity.st_ino;
  handle->module_id = module_id;
  handle->early = early;
  handle->has_companion = info.has_companion != 0;
  g_loaded_modules.push_back(handle);
  LOGI("native module onModuleLoaded: %s early=%u", module_id.c_str(),
       early ? 1U : 0U);
  const size_t registrations_before =
      yukizygisk::hyos::registered_module_count();
  yukizygisk::hyos::begin_module_registration(&handle->index);
  mod->onModuleLoaded(handle, &g_api);
  recover_native_plt();
  yukizygisk::hyos::end_module_registration();
  handle->hyos_registered =
      yukizygisk::hyos::registered_module_count() > registrations_before;
  // Keep the handle for process lifetime: callbacks can still use libdl.
  LOGI("native module Bionic handle retained: %s", module_id.c_str());
  LOGI("native module loaded: %s early=%u", module_id.c_str(), early ? 1U : 0U);
  if (!early && !yukizygisk::hyos::available())
    handle->reported = report_native_injection(idx);
  return true;
}

bool early_packet_entry_to_info(const yz_early_native_packet_entry &entry,
                                zygiskd::NativeModuleInfo *info) {
  yz_early_native_entry module = entry.module;
  module.module_id[YZ_NATIVE_MODULE_ID_MAX - 1] = '\0';
  module.target[YZ_NATIVE_TARGET_VALUE_MAX - 1] = '\0';
  module.lib_path[YZ_NATIVE_MODULE_PATH_MAX - 1] = '\0';
  if (entry.fd < 0)
    return false;
  if (module.target_type != YZ_NATIVE_TARGET_NAME &&
      module.target_type != YZ_NATIVE_TARGET_PATH)
    return false;
  if (module.module_id[0] == '\0' || module.target[0] == '\0' ||
      module.lib_path[0] == '\0')
    return false;

  *info = {};
  info->target_type = module.target_type;
  info->has_companion = 0;
  (void)snprintf(info->module_id, sizeof(info->module_id), "%s",
                 module.module_id);
  (void)snprintf(info->target, sizeof(info->target), "%s", module.target);
  (void)snprintf(info->lib_path, sizeof(info->lib_path), "%s", module.lib_path);
  return true;
}

void load_early_modules() {
  int packet_fd = g_early_packet_fd;
  if (packet_fd < 0)
    return;
  g_early_packet_fd = -1;

  yz_bootstrap_packet_header hdr{};
  if (lseek(packet_fd, 0, SEEK_SET) != 0 ||
      !read_all(packet_fd, &hdr, sizeof(hdr)) ||
      hdr.magic != YZ_BOOTSTRAP_PACKET_MAGIC ||
      hdr.version != YZ_BOOTSTRAP_PACKET_VERSION ||
      hdr.header_size != sizeof(hdr) ||
      hdr.entry_size != sizeof(yz_early_native_packet_entry) ||
      hdr.count > YZ_NATIVE_TARGET_MAX) {
    LOGE("native core: invalid early packet fd=%d", packet_fd);
    close(packet_fd);
    return;
  }

  LOGI("native core: early packet modules=%u", hdr.count);
  for (uint32_t i = 0; i < hdr.count; i++) {
    yz_early_native_packet_entry entry{};
    if (!read_all(packet_fd, &entry, sizeof(entry))) {
      LOGE("native core: early packet read failed idx=%u", i);
      break;
    }

    zygiskd::NativeModuleInfo info{};
    if (!early_packet_entry_to_info(entry, &info)) {
      if (entry.fd >= 0)
        close(entry.fd);
      LOGE("native core: early packet invalid module idx=%u", i);
      continue;
    }
    std::string module_id = module_id_of(info);
    if (find_loaded_module(module_id) != nullptr) {
      LOGI("native core: early module already loaded id=%s", module_id.c_str());
      close(entry.fd);
      continue;
    }
    (void)load_native_module_from_fd(info, 0xffffffffu, entry.fd,
                                     /*early=*/true);
  }
  close(packet_fd);
}

void load_matching_modules() {
  std::string exe = self_exe_path();
  std::string exe_base = basename_of(exe);
  LOGI("native core: exe=%s base=%s", exe.c_str(), exe_base.c_str());

  uint32_t count = 0;
  if (!request_native_module_count(&count, /*quiet=*/false))
    return;
  LOGI("native core: module count=%u", count);

  for (uint32_t i = 0; i < count; ++i) {
    zygiskd::NativeModuleInfo info{};
    if (!request_native_info(i, &info))
      continue;

    bool matched = target_matches(info, exe, exe_base);
    LOGI("native core: candidate idx=%u id=%s target_type=%u target=%s "
         "companion=%u match=%u",
         i, info.module_id, info.target_type, info.target,
         info.has_companion ? 1U : 0U, matched ? 1U : 0U);
    if (!matched)
      continue;

    std::string module_id = module_id_of(info);
    if (auto *loaded = find_loaded_module(module_id)) {
      loaded->index = i;
      loaded->has_companion = info.has_companion != 0;
      LOGI("native core: duplicate skipped id=%s idx=%u early=%u",
           module_id.c_str(), i, loaded->early ? 1U : 0U);
      if (loaded->early && !loaded->reported && !yukizygisk::hyos::available())
        loaded->reported = report_native_injection(i);
      continue;
    }

    int lib_fd = request_fd(ZdRequest::GetNativeModuleFd, i);
    if (lib_fd < 0) {
      LOGE("native core: module fd failed id=%s idx=%u", info.module_id, i);
      continue;
    }
    (void)load_native_module_from_fd(info, i, lib_fd, /*early=*/false);
  }
}

bool sync_early_native_reports_once() {
  if (!has_pending_early_report())
    return true;

  uint32_t count = 0;
  if (!request_native_module_count(&count, /*quiet=*/true))
    return false;

  bool all_reported = true;
  for (auto *h : g_loaded_modules) {
    if (h == nullptr || !h->early || h->reported)
      continue;
    if (yukizygisk::hyos::available() && !h->hyos_registered)
      continue;

    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
      zygiskd::NativeModuleInfo info{};
      if (!request_native_info(i, &info))
        continue;
      if (module_id_of(info) != h->module_id)
        continue;
      h->index = i;
      h->has_companion = info.has_companion != 0;
      h->reported = report_native_injection(i);
      found = true;
      break;
    }
    if (!found || !h->reported)
      all_reported = false;
  }
  return all_reported;
}

void *deferred_native_sync_main(void *) {
  constexpr int kAttempts = 80;
  constexpr useconds_t kSleepUs = 100000;

  for (int i = 0; i < kAttempts; i++) {
    if (sync_early_native_reports_once()) {
      restore_native_load_policy();
      LOGI("native core: deferred early sync complete");
      return nullptr;
    }
    usleep(kSleepUs);
  }
  LOGE("native core: deferred early sync timed out");
  return nullptr;
}

void maybe_start_deferred_native_sync() {
  if (!has_pending_early_report())
    return;
  if (sync_early_native_reports_once())
    return;

  pthread_t tid{};
  int rc = pthread_create(&tid, nullptr, deferred_native_sync_main, nullptr);
  if (rc != 0) {
    LOGE("native core: deferred early sync thread failed rc=%d", rc);
    return;
  }
  pthread_detach(tid);
}

extern "C" {
extern void (*__init_array_start[])(void) __attribute__((visibility("hidden")));
extern void (*__init_array_end[])(void) __attribute__((visibility("hidden")));
}

volatile int g_ctors_done = 0;
__attribute__((constructor)) void mark_ctors_done() { g_ctors_done = 1; }

void run_ctors_once() {
  if (g_ctors_done)
    return;
  for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
    if (*p != nullptr)
      (*p)();
}

void core_start() {
  run_ctors_once();
  g_inline_hooks.reserve(YZ_NATIVE_TARGET_MAX);
  g_runtime_generation = request_runtime_generation();
  load_config();
  (void)yukizygisk::hyos::initialize(open_hyos_control_session,
                                     report_hyos_callback, recover_native_plt);
  load_early_modules();
  load_matching_modules();
  if (yukizygisk::hyos::available()) {
    if (!yukizygisk::hyos::install_registered_hooks()) {
      LOGE("native core: HyperOS Rust Runtime bridge unavailable");
      restore_native_load_policy();
      return;
    }
    restore_native_load_policy();
    return;
  }
  maybe_start_deferred_native_sync();
  restore_native_load_policy();
}

} // namespace

extern "C" bool yz_patch_text(uintptr_t addr, const void *bytes,
                              unsigned int len) {
  if (bytes == nullptr || len == 0 || len > YZ_PATCH_TEXT_MAX)
    return false;
  if (yukizygisk::hyos::in_specialized_child()) {
    uint8_t frame[sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) +
                  YZ_PATCH_TEXT_MAX];
    size_t offset = 0;
    frame[offset++] = static_cast<uint8_t>(ZdRequest::PatchText);
    const uint64_t address = addr;
    const uint32_t length = len;
    memcpy(frame + offset, &address, sizeof(address));
    offset += sizeof(address);
    memcpy(frame + offset, &length, sizeof(length));
    offset += sizeof(length);
    memcpy(frame + offset, bytes, len);
    offset += len;
    uint8_t ack = 0;
    yukizygisk::hyos::lock_child_control_session();
    const int session = yukizygisk::hyos::child_control_session();
    if (session < 0) {
      yukizygisk::hyos::unlock_child_control_session();
      return false;
    }
    const HyosDeadline deadline = make_hyos_deadline();
    const bool sent = send_packet_until(session, frame, offset, deadline);
    const bool received = sent && receive_byte_until(session, &ack, deadline);
    if (!sent || !received)
      yukizygisk::hyos::invalidate_child_control_session();
    yukizygisk::hyos::unlock_child_control_session();
    return received && ack != 0;
  }
  int s = connect_zygiskd();
  if (s < 0)
    return false;
  uint8_t op = static_cast<uint8_t>(ZdRequest::PatchText);
  uint64_t a64 = addr;
  uint32_t l32 = len;
  uint8_t ack = 0;
  bool ok = write_all(s, &op, 1) && write_all(s, &a64, sizeof(a64)) &&
            write_all(s, &l32, sizeof(l32)) && write_all(s, bytes, len);
  if (ok)
    ok = read_all(s, &ack, 1) && ack != 0;
  close(s);
  return ok;
}

extern "C" uint8_t yz_patch_text_checked(uintptr_t addr, const void *expected,
                                         const void *replacement,
                                         unsigned int len) {
  if (expected == nullptr || replacement == nullptr || len == 0 ||
      len > YZ_PATCH_TEXT_MAX)
    return 0;
  if (yukizygisk::hyos::in_specialized_child()) {
    uint8_t frame[sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) +
                  (size_t{2} * YZ_PATCH_TEXT_MAX)];
    size_t offset = 0;
    frame[offset++] = static_cast<uint8_t>(ZdRequest::PatchTextV2);
    const uint64_t address = addr;
    const uint32_t length = len;
    memcpy(frame + offset, &address, sizeof(address));
    offset += sizeof(address);
    memcpy(frame + offset, &length, sizeof(length));
    offset += sizeof(length);
    memcpy(frame + offset, expected, len);
    offset += len;
    memcpy(frame + offset, replacement, len);
    offset += len;
    uint8_t result = 2;
    yukizygisk::hyos::lock_child_control_session();
    const int session = yukizygisk::hyos::child_control_session();
    if (session < 0) {
      yukizygisk::hyos::unlock_child_control_session();
      return 0;
    }
    const HyosDeadline deadline = make_hyos_deadline();
    const bool sent = send_packet_until(session, frame, offset, deadline);
    const bool received =
        sent && receive_byte_until(session, &result, deadline);
    if (!sent || !received)
      yukizygisk::hyos::invalidate_child_control_session();
    yukizygisk::hyos::unlock_child_control_session();
    return received && result <= 2 ? result : 2;
  }
  int s = connect_zygiskd();
  if (s < 0) {
    return 0;
  }
  const uint8_t op = static_cast<uint8_t>(ZdRequest::PatchTextV2);
  const uint64_t a64 = addr;
  const uint32_t l32 = len;
  uint8_t result = 2;
  const bool sent = write_all(s, &op, 1) && write_all(s, &a64, sizeof(a64)) &&
                    write_all(s, &l32, sizeof(l32)) &&
                    write_all(s, expected, len) &&
                    write_all(s, replacement, len);
  const bool received = sent && read_all(s, &result, 1);
  close(s);
  return received && result <= 2 ? result : 2;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_set_load_config(const yz_config *config) {
  if (config != nullptr)
    g_yz_config = *config;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char * /*self_path*/, void *loader_self,
                  void *core_base, void *core_size, int early_packet_fd_plus1) {
  g_loader_base = reinterpret_cast<uintptr_t>(loader_self);
  g_self_base = reinterpret_cast<uintptr_t>(core_base);
  g_self_size = reinterpret_cast<size_t>(core_size);
  g_early_packet_fd =
      early_packet_fd_plus1 > 0 ? early_packet_fd_plus1 - 1 : -1;
  if (!yukizygisk::endpoint::initialize(g_early_packet_fd, true)) {
    yukizygisk::endpoint::close_packet(g_early_packet_fd, false);
    g_early_packet_fd = -1;
    return;
  }
  core_start();
  const auto result =
      g_self_base == 0
          ? yukizygisk::handoff::verify_loaded(
                reinterpret_cast<uintptr_t>(core_start), g_loader_base)
          : yukizygisk::handoff::verify(g_self_base, g_self_size,
                                        g_loader_base);
  if (!result.complete && !result.tls_retained)
    LOGE("native loader verification failed: %s", result.reason);
  g_loader_unmap_safe =
      result.complete &&
      yukizygisk::handoff::anonymous_image_matches(g_self_base, g_self_size) &&
      core_runtime_profile_ready(g_self_base, g_self_size);
  LOGI("native loader handoff: %s; slot=%p target=%p; retain=%d", result.reason,
       reinterpret_cast<void *>(result.slot),
       reinterpret_cast<void *>(result.target), !g_loader_unmap_safe);
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_handoff_ready() {
  return g_loader_unmap_safe;
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_handoff_complete() {
  return g_loader_unmap_safe && yukizygisk::handoff::retirement_ready();
}

extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int,
                                                                      int) {
  if (!zygisk_bootstrap_handoff_complete()) {
    LOGI("loader retirement not authorized; retaining loader and dependencies");
    return;
  }

  const bool released = yukizygisk::handoff::close_loader_reference();
  LOGI("native finalize_loader: bootstrap reference released=%d", released);
  if (released)
    g_loader_base = 0;
}
