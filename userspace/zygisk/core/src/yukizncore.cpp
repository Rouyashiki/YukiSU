#include "hyos_runtime.hpp"
#include "inline_hook.hpp"
#include "load_config.hpp"
#include "log.hpp"
#include "solist.hpp"
#include "yukilinker.hpp"
#include "zygiskd.hpp"

#include "uapi/yukizygisk.h"
#include "zygisk_next_api.h"

#include "lsplt.hpp"

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
  std::string module_id;
  bool yuki_loaded = false;
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

int connect_zygiskd() {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t len = std::strlen(zygiskd::kSocketName);
  memcpy(addr.sun_path + 1, zygiskd::kSocketName, len);
  auto alen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + len);
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), alen) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

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
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const size_t length = std::strlen(zygiskd::kSocketName);
  memcpy(address.sun_path + 1, zygiskd::kSocketName, length);
  const auto address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), address_length) !=
      0) {
    if ((errno != EINPROGRESS && errno != EAGAIN) ||
        !wait_socket_event_until(fd, POLLOUT, deadline)) {
      close(fd);
      return -1;
    }
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 ||
        error != 0) {
      close(fd);
      return -1;
    }
  }
  return fd;
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

bool map_from_base(void *base_addr, lsplt::MapInfo *out) {
  uintptr_t base = reinterpret_cast<uintptr_t>(base_addr);
  auto maps = yukizygisk::hyos::in_specialized_child()
                  ? lsplt::MapInfo::ScanCached()
                  : lsplt::MapInfo::Scan();
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

bool find_library_map(const char *path, void *base_addr, lsplt::MapInfo *out,
                      uintptr_t *load_base) {
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
                  ? lsplt::MapInfo::ScanCached()
                  : lsplt::MapInfo::Scan();
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

int api_plt_hook(void *base_addr, const char *symbol, void *hook_handler,
                 void **original) {
  if (symbol == nullptr || hook_handler == nullptr) {
    LOGE("plt hook: invalid args base=%p symbol=%p handler=%p", base_addr,
         static_cast<const void *>(symbol), hook_handler);
    return kFailed;
  }
  lsplt::MapInfo info{};
  if (!map_from_base(base_addr, &info)) {
    LOGE("plt hook: map not found base=%p symbol=%s", base_addr, symbol);
    return kFailed;
  }
  if (!lsplt::RegisterHook(info.dev, info.inode, symbol, hook_handler,
                           original)) {
    LOGE("plt hook: register failed path=%s symbol=%s", info.path.c_str(),
         symbol);
    return kFailed;
  }
  const bool ok = yukizygisk::hyos::in_specialized_child()
                      ? lsplt::CommitHookCached()
                      : lsplt::CommitHook();
  LOGI("plt hook: path=%s symbol=%s result=%u", info.path.c_str(), symbol,
       ok ? 0U : 1U);
  return ok ? kSuccess : kFailed;
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
  lsplt::MapInfo info{};
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

  std::string lib_name = basename_of(lib_path);
  bool yuki_loaded = false;
  int fallback_anonymized = 0;
  void *so = g_yz_config.yukilinker
                 ? yukilinker::dlopen_memfd(
                       lib_fd, lib_path.c_str(),
                       !yukizygisk::config::anonymous(g_yz_config))
                 : nullptr;
  if (so != nullptr) {
    yuki_loaded = true;
    LOGI("native core: yukilinker loaded id=%s idx=%u path=%s early=%u",
         module_id.c_str(), idx, lib_path.c_str(), early ? 1U : 0U);
  } else {
    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_FORCE_LOAD;
    ext.library_fd = lib_fd;
    dlerror();
    so = android_dlopen_ext(lib_name.c_str(), RTLD_NOW, &ext);
    const char *name_err = dlerror();
    std::string name_error = name_err != nullptr ? name_err : "";
    if (so == nullptr) {
      dlerror();
      so = android_dlopen_ext(lib_path.c_str(), RTLD_NOW, &ext);
    }
    const char *path_err = dlerror();
    std::string path_error = path_err != nullptr ? path_err : "";
    if (so == nullptr) {
      LOGE("native core: dlopen failed id=%s idx=%u name=%s path=%s early=%u",
           module_id.c_str(), idx, lib_name.c_str(), lib_path.c_str(),
           early ? 1U : 0U);
      LOGE("native core: dlopen name err=%s",
           name_error.empty() ? "(null)" : name_error.c_str());
      LOGE("native core: dlopen path err=%s",
           path_error.empty() ? "(null)" : path_error.c_str());
    }
    if (so != nullptr && yukizygisk::config::anonymous(g_yz_config))
      fallback_anonymized =
          yuki::solist::spoof_fd_maps(lib_fd, /*private_only=*/true);
  }
  close(lib_fd);
  if (so == nullptr)
    return false;

  auto *mod = reinterpret_cast<ZygiskNextModule *>(
      yuki_loaded ? yukilinker::dlsym(static_cast<yukilinker::SoHandle *>(so),
                                      "zn_module")
                  : dlsym(so, "zn_module"));
  if (mod == nullptr) {
    LOGE("native core: zn_module missing id=%s idx=%u", module_id.c_str(), idx);
    if (yuki_loaded)
      yukilinker::dlclose(static_cast<yukilinker::SoHandle *>(so));
    else
      dlclose(so);
    return false;
  }
  if ((mod->target_api_version != kApiVersion3 &&
       mod->target_api_version != kApiVersion4) ||
      mod->onModuleLoaded == nullptr) {
    LOGE("native core: unsupported module id=%s idx=%u api=%d entry=%p",
         module_id.c_str(), idx, mod->target_api_version, mod->onModuleLoaded);
    if (yuki_loaded)
      yukilinker::dlclose(static_cast<yukilinker::SoHandle *>(so));
    else
      dlclose(so);
    return false;
  }

  auto *handle = new ModuleHandle();
  handle->index = idx;
  handle->so = so;
  handle->module_id = module_id;
  handle->yuki_loaded = yuki_loaded;
  handle->early = early;
  handle->has_companion = info.has_companion != 0;
  g_loaded_modules.push_back(handle);
  LOGI("native module onModuleLoaded: %s early=%u", module_id.c_str(),
       early ? 1U : 0U);
  const size_t registrations_before =
      yukizygisk::hyos::registered_module_count();
  yukizygisk::hyos::begin_module_registration(&handle->index);
  mod->onModuleLoaded(handle, &g_api);
  yukizygisk::hyos::end_module_registration();
  handle->hyos_registered =
      yukizygisk::hyos::registered_module_count() > registrations_before;
  if (!yuki_loaded) {
    int hidden = yuki::solist::drop_lib_containing(
        reinterpret_cast<uintptr_t>(mod->onModuleLoaded),
        /*keep_mapped=*/true);
    LOGI("native module fallback sanitized: %s maps=%d soinfo=%d",
         module_id.c_str(), fallback_anonymized, hidden);
  }
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

  yz_early_native_packet_header hdr{};
  if (lseek(packet_fd, 0, SEEK_SET) != 0 ||
      !read_all(packet_fd, &hdr, sizeof(hdr)) ||
      hdr.magic != YZ_EARLY_NATIVE_PACKET_MAGIC ||
      hdr.version != YZ_EARLY_NATIVE_VERSION ||
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

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif // #ifndef R_AARCH64_GLOB_DAT
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif // #ifndef R_AARCH64_JUMP_SLOT
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif // #ifndef R_ARM_GLOB_DAT
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif // #ifndef R_ARM_JUMP_SLOT

#if defined(__LP64__)
using SelfRelocation = ElfW(Rela);
#else
using SelfRelocation = ElfW(Rel);
#endif // #if defined(__LP64__)

extern "C" const ElfW(Dyn) _DYNAMIC[];

const ElfW(Dyn) * self_dynamic_table(uintptr_t load_bias) {
  uintptr_t dyn = reinterpret_cast<uintptr_t>(_DYNAMIC);
  if (load_bias != 0 && g_self_size != 0 && dyn < g_self_size)
    dyn += load_bias;
  return reinterpret_cast<const ElfW(Dyn) *>(dyn);
}

void *resolve_system_dl_iterate_phdr() {
  volatile char vn[] = "dl_iterate_phdr";
  char nm[sizeof(vn)];
  for (size_t i = 0; i < sizeof(vn); ++i)
    nm[i] = vn[i];
  return dlsym(RTLD_DEFAULT, nm);
}

bool rebind_self_dl_iterate_slot(uintptr_t load_bias) {
  if (load_bias == 0)
    return true;
  void *sysfn = resolve_system_dl_iterate_phdr();

  const ElfW(Sym) *symtab = nullptr;
  const char *strtab = nullptr;
  const SelfRelocation *jmprel = nullptr;
  const SelfRelocation *rel = nullptr;
  size_t pltrelsz = 0;
  size_t relasz = 0;
  for (const ElfW(Dyn) *d = self_dynamic_table(load_bias); d->d_tag != DT_NULL;
       ++d) {
    switch (d->d_tag) {
    case DT_SYMTAB:
      symtab = reinterpret_cast<const ElfW(Sym) *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_STRTAB:
      strtab = reinterpret_cast<const char *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_JMPREL:
      jmprel =
          reinterpret_cast<const SelfRelocation *>(load_bias + d->d_un.d_ptr);
      break;
    case DT_PLTRELSZ:
      pltrelsz = d->d_un.d_val;
      break;
#if defined(__LP64__)
    case DT_RELA:
#else
    case DT_REL:
#endif // #if defined(__LP64__)
      rel = reinterpret_cast<const SelfRelocation *>(load_bias + d->d_un.d_ptr);
      break;
#if defined(__LP64__)
    case DT_RELASZ:
#else
    case DT_RELSZ:
#endif // #if defined(__LP64__)
      relasz = d->d_un.d_val;
      break;
    default:
      break;
    }
  }
  if (symtab == nullptr || strtab == nullptr)
    return false;

  const long pg = getpagesize();
  bool safe = true;
  auto patch = [&](const SelfRelocation *r, size_t count) {
    for (size_t i = 0; i < count; ++i) {
#if defined(__LP64__)
      uint32_t type = ELF64_R_TYPE(r[i].r_info);
      if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT)
        continue;
      uint32_t si = ELF64_R_SYM(r[i].r_info);
#else
      uint32_t type = ELF32_R_TYPE(r[i].r_info);
      if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT)
        continue;
      uint32_t si = ELF32_R_SYM(r[i].r_info);
#endif // #if defined(__LP64__)
      if (strcmp(strtab + symtab[si].st_name, "dl_iterate_phdr") != 0)
        continue;
      if (sysfn == nullptr) {
        safe = false;
        continue;
      }
      auto **slot = reinterpret_cast<void **>(load_bias + r[i].r_offset);
      uintptr_t pbase =
          reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(pg - 1);
      mprotect(reinterpret_cast<void *>(pbase), static_cast<size_t>(pg),
               PROT_READ | PROT_WRITE);
      *slot = sysfn;
    }
  };
  if (jmprel != nullptr)
    patch(jmprel, pltrelsz / sizeof(SelfRelocation));
  if (rel != nullptr)
    patch(rel, relasz / sizeof(SelfRelocation));
  return safe;
}

void core_start() {
  run_ctors_once();
  g_inline_hooks.reserve(YZ_NATIVE_TARGET_MAX);
  g_runtime_generation = request_runtime_generation();
  load_config();
  if (g_self_base == 0 && yukizygisk::config::anonymous(g_yz_config))
    (void)yuki::solist::spoof_loaded_object_maps(
        reinterpret_cast<uintptr_t>(core_start), true);
  (void)yukizygisk::hyos::initialize(open_hyos_control_session,
                                     report_hyos_callback);
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
  g_loader_unmap_safe = rebind_self_dl_iterate_slot(g_self_base);
  core_start();
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry_direct(int /*core_fd*/) {
  g_loader_base = 0;
  g_self_base = 0;
  g_self_size = 0;
  g_early_packet_fd = -1;
  g_loader_unmap_safe = true;
  core_start();
}

extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int,
                                                                      int) {
  int n =
      yuki::solist::drop_lib_containing(g_loader_base, !g_loader_unmap_safe);
  LOGI("native finalize_loader: unloaded %d soinfo(s)", n);
}
