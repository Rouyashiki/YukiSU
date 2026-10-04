#pragma once

#include "daemon_health.hpp"
#include "daemon_state.hpp"

#include <elf.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace yukizygisk::health {

enum class Availability {
  Available,
  Missing,
  Unresponsive,
  Unsupported,
  IdentityError,
  Error,
};

enum class ExecutionBackend { Unknown, Native, Tango };

struct QueryResult {
  Availability availability = Availability::Error;
  int error = 0;
  Snapshot snapshot;
  ExecutionBackend execution_backend = ExecutionBackend::Unknown;
};

namespace client_detail {

using Deadline = std::chrono::steady_clock::time_point;

inline bool read_at(int fd, void *buffer, size_t size, off_t offset,
                    Deadline deadline) {
  auto *bytes = static_cast<unsigned char *>(buffer);
  size_t done = 0;
  while (done < size) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    const ssize_t count =
        pread(fd, bytes + done, size - done, offset + static_cast<off_t>(done));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    done += static_cast<size_t>(count);
  }
  return true;
}

inline bool read_process_file(pid_t pid, const char *name, size_t limit,
                              std::string *result, Deadline deadline) {
  char path[64];
  (void)snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  bool ok = true;
  result->clear();
  char buffer[4096];
  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ok = false;
      break;
    }
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count == 0)
      break;
    if (count < 0 || result->size() + static_cast<size_t>(count) > limit) {
      ok = false;
      break;
    }
    result->append(buffer, static_cast<size_t>(count));
  }
  close(fd);
  return ok;
}

inline bool elf_ident(const unsigned char *ident, unsigned elf_class) {
  return memcmp(ident, ELFMAG, SELFMAG) == 0 && ident[EI_CLASS] == elf_class &&
         ident[EI_DATA] == ELFDATA2LSB && ident[EI_VERSION] == EV_CURRENT;
}

inline bool same_inode(const struct stat &left, const struct stat &right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

inline bool same_image(const struct stat &left, const struct stat &right) {
  return same_inode(left, right) && left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

struct GuestImage {
  Elf32_Ehdr header{};
  std::vector<Elf32_Phdr> segments;
};

inline bool read_guest_image(int fd, const struct stat &status,
                             GuestImage *image, Deadline deadline) {
  auto &header = image->header;
  if (!read_at(fd, &header, sizeof(header), 0, deadline) ||
      !elf_ident(header.e_ident, ELFCLASS32) || header.e_machine != EM_ARM ||
      header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
      (header.e_type != ET_DYN && header.e_type != ET_EXEC) ||
      header.e_phentsize != sizeof(Elf32_Phdr) || header.e_phnum == 0 ||
      header.e_phnum > 128 || header.e_phoff < sizeof(header) ||
      header.e_phoff > INT32_MAX || status.st_size < 0)
    return false;
  const uint64_t table_end =
      static_cast<uint64_t>(header.e_phoff) +
      (static_cast<uint64_t>(header.e_phnum) * sizeof(Elf32_Phdr));
  if (table_end > static_cast<uint64_t>(status.st_size) ||
      table_end > INT32_MAX)
    return false;
  image->segments.resize(header.e_phnum);
  if (!read_at(fd, image->segments.data(),
               image->segments.size() * sizeof(Elf32_Phdr),
               static_cast<off_t>(header.e_phoff), deadline))
    return false;
  for (const auto &segment : image->segments)
    if (segment.p_type == PT_LOAD &&
        (segment.p_filesz > segment.p_memsz ||
         static_cast<uint64_t>(segment.p_offset) + segment.p_filesz >
             static_cast<uint64_t>(status.st_size) ||
         static_cast<uint64_t>(segment.p_vaddr) + segment.p_memsz >
             (uint64_t{1} << 32)))
      return false;
  return true;
}

struct Mapping {
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t offset = 0;
  uint64_t inode = 0;
  uint64_t device_major = 0;
  uint64_t device_minor = 0;
  bool readable = false;
  bool writable = false;
  bool executable = false;
  bool private_mapping = false;
};

inline std::string_view map_token(std::string_view *rest) {
  const auto first = rest->find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    *rest = {};
    return {};
  }
  rest->remove_prefix(first);
  const auto end = rest->find_first_of(" \t\r");
  const auto token = rest->substr(0, end);
  *rest =
      end == std::string_view::npos ? std::string_view{} : rest->substr(end);
  return token;
}

inline bool map_number(std::string_view text, int base, uint64_t *value) {
  if (text.empty())
    return false;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *value, base);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

inline bool parse_mapping(std::string_view line, Mapping *mapping) {
  const auto range = map_token(&line);
  const auto permissions = map_token(&line);
  const auto offset = map_token(&line);
  const auto device = map_token(&line);
  const auto inode = map_token(&line);
  const auto dash = range.find('-'), colon = device.find(':');
  if (dash == std::string_view::npos || colon == std::string_view::npos ||
      permissions.size() != 4 ||
      !map_number(range.substr(0, dash), 16, &mapping->start) ||
      !map_number(range.substr(dash + 1), 16, &mapping->end) ||
      !map_number(offset, 16, &mapping->offset) ||
      !map_number(device.substr(0, colon), 16, &mapping->device_major) ||
      !map_number(device.substr(colon + 1), 16, &mapping->device_minor) ||
      !map_number(inode, 10, &mapping->inode) ||
      mapping->end <= mapping->start ||
      (permissions[0] != 'r' && permissions[0] != '-') ||
      (permissions[1] != 'w' && permissions[1] != '-') ||
      (permissions[2] != 'x' && permissions[2] != '-') ||
      (permissions[3] != 'p' && permissions[3] != 's'))
    return false;
  mapping->readable = permissions[0] == 'r';
  mapping->writable = permissions[1] == 'w';
  mapping->executable = permissions[2] == 'x';
  mapping->private_mapping = permissions[3] == 'p';
  return true;
}

inline bool tango_maps_match(const GuestImage &image, const struct stat &guest,
                             std::string_view text) {
  std::vector<Mapping> mappings;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{}
                                         : text.substr(end + 1);
    if (line.empty())
      continue;
    Mapping mapping;
    if (!parse_mapping(line, &mapping))
      return false;
    if (mapping.inode == static_cast<uint64_t>(guest.st_ino) &&
        mapping.device_major == major(guest.st_dev) &&
        mapping.device_minor == minor(guest.st_dev)) {
      if (mappings.size() == 256)
        return false;
      mappings.push_back(mapping);
    }
  }
  const uint64_t table_end =
      static_cast<uint64_t>(image.header.e_phoff) +
      (static_cast<uint64_t>(image.segments.size()) * sizeof(Elf32_Phdr));
  const uint64_t entry = image.header.e_entry & ~uint32_t{1};
  for (const auto &segment : image.segments) {
    if (segment.p_type != PT_LOAD || segment.p_offset != 0 ||
        segment.p_filesz < table_end || (segment.p_flags & PF_R) == 0)
      continue;
    for (const auto &header_map : mappings) {
      if (header_map.offset != 0 || !header_map.readable ||
          header_map.writable || !header_map.private_mapping ||
          header_map.start < segment.p_vaddr ||
          header_map.end > (uint64_t{1} << 32) ||
          header_map.end - header_map.start < segment.p_filesz)
        continue;
      const uint64_t bias = header_map.start - segment.p_vaddr;
      if (image.header.e_type == ET_EXEC && bias != 0)
        continue;
      bool have_executable = false, have_entry = false, valid = true;
      for (const auto &load : image.segments) {
        if (load.p_type != PT_LOAD || (load.p_flags & PF_X) == 0)
          continue;
        have_executable = true;
        const uint64_t begin = bias + load.p_vaddr;
        const uint64_t finish = begin + load.p_filesz;
        const bool matched =
            load.p_filesz != 0 && finish <= (uint64_t{1} << 32) &&
            std::any_of(mappings.begin(), mappings.end(), [&](const auto &map) {
              return map.readable && map.executable && !map.writable &&
                     map.private_mapping && map.start <= begin &&
                     finish <= map.end &&
                     begin - map.start <= UINT64_MAX - map.offset &&
                     map.offset + (begin - map.start) == load.p_offset;
            });
        valid &= matched;
        have_entry |= matched && entry >= load.p_vaddr &&
                      entry - load.p_vaddr < load.p_filesz;
      }
      if (valid && have_executable && have_entry)
        return true;
    }
  }
  return false;
}

inline bool tango_identity(uint32_t abi, pid_t pid, const char *executable,
                           const struct stat &expected,
                           const struct stat &actual, Deadline deadline) {
  if (abi != 1)
    return false;
  constexpr char translator_path[] = "/system_ext/bin/tango_translator";
  const int translator =
      open(translator_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (translator < 0)
    return false;
  struct stat translator_status{};
  struct statvfs translator_filesystem{};
  Elf64_Ehdr translator_header{};
  const bool translator_ok =
      fstat(translator, &translator_status) == 0 &&
      S_ISREG(translator_status.st_mode) && translator_status.st_uid == 0 &&
      fstatvfs(translator, &translator_filesystem) == 0 &&
      (translator_filesystem.f_flag & ST_RDONLY) != 0 &&
      same_inode(translator_status, actual) &&
      read_at(translator, &translator_header, sizeof(translator_header), 0,
              deadline) &&
      elf_ident(translator_header.e_ident, ELFCLASS64) &&
      translator_header.e_machine == EM_AARCH64 &&
      translator_header.e_version == EV_CURRENT &&
      translator_header.e_ehsize == sizeof(translator_header);
  close(translator);
  if (!translator_ok)
    return false;
  const int guest =
      open(executable, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (guest < 0)
    return false;
  struct stat guest_status{}, after{}, named{};
  GuestImage image;
  std::string command, maps;
  bool ok = fstat(guest, &guest_status) == 0 && S_ISREG(guest_status.st_mode) &&
            guest_status.st_uid == 0 && same_image(expected, guest_status) &&
            read_guest_image(guest, guest_status, &image, deadline) &&
            read_process_file(pid, "cmdline", 4096, &command, deadline);
  const size_t name_size = strlen(executable);
  ok = ok && command.size() >= name_size + 1 &&
       command.compare(0, name_size, executable) == 0 &&
       command[name_size] == '\0' &&
       std::all_of(command.begin() + static_cast<ptrdiff_t>(name_size),
                   command.end(), [](char byte) { return byte == '\0'; });
  ok = ok &&
       read_process_file(pid, "maps", size_t{1024} * 1024, &maps, deadline) &&
       tango_maps_match(image, guest_status, maps) &&
       fstat(guest, &after) == 0 && same_image(guest_status, after) &&
       stat(executable, &named) == 0 && same_image(guest_status, named);
  close(guest);
  char exe_path[64];
  (void)snprintf(exe_path, sizeof(exe_path), "/proc/%d/exe", pid);
  return ok && stat(exe_path, &named) == 0 && same_inode(actual, named) &&
         stat(translator_path, &named) == 0 &&
         same_image(translator_status, named) &&
         std::chrono::steady_clock::now() < deadline;
}

struct Connection {
  Connection() = default;
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(Connection &&) = delete;
  int fd = -1;
  int pidfd = -1;
  pid_t pid = 0;
  uint64_t start_ticks = 0;
  dev_t executable_device = 0;
  ino_t executable_inode = 0;
  size_t received = 0;
  unsigned stage = 0;
  ExecutionBackend execution_backend = ExecutionBackend::Unknown;
  QueryResult result;
  ~Connection() {
    if (fd >= 0)
      close(fd);
    if (pidfd >= 0)
      close(pidfd);
  }
  void finish(Availability availability, int error) {
    result.availability = availability;
    result.error = error;
    result.execution_backend = availability == Availability::Available
                                   ? execution_backend
                                   : ExecutionBackend::Unknown;
    stage = 0;
  }
};

inline bool identity(Connection &connection, uint32_t abi, Deadline deadline) {
  struct ucred peer{};
  socklen_t length = sizeof(peer);
  if (getsockopt(connection.fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
      length != sizeof(peer) || peer.uid != 0 || peer.pid <= 0)
    return false;
  connection.pid = peer.pid;
  connection.start_ticks = process_start_ticks(peer.pid);
  if (connection.start_ticks == 0)
    return false;
#ifdef SYS_pidfd_open
  connection.pidfd = static_cast<int>(syscall(SYS_pidfd_open, peer.pid, 0));
  if (connection.pidfd < 0)
    return false;
#endif
  char path[64];
  (void)snprintf(path, sizeof(path), "/proc/%d/exe", peer.pid);
  struct stat expected{}, actual{};
  const char *executable =
      abi == 1 ? "/data/adb/ksu/bin/zygiskd32" : "/data/adb/ksu/bin/zygiskd64";
  if (stat(executable, &expected) != 0 || stat(path, &actual) != 0 ||
      !S_ISREG(expected.st_mode) || expected.st_uid != 0)
    return false;
  if (same_inode(expected, actual))
    connection.execution_backend = ExecutionBackend::Native;
  else if (tango_identity(abi, peer.pid, executable, expected, actual,
                          deadline))
    connection.execution_backend = ExecutionBackend::Tango;
  else
    return false;
  connection.executable_device = actual.st_dev;
  connection.executable_inode = actual.st_ino;
  return connection.start_ticks == process_start_ticks(peer.pid);
}

inline std::array<QueryResult, 2> query_mask(unsigned mask, int timeout_ms) {
  using Clock = std::chrono::steady_clock;
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
  std::array<Connection, 2> connections;
  for (size_t index = 0; index < connections.size(); ++index) {
    auto &connection = connections[index];
    if ((mask & (1U << index)) == 0)
      continue;
    connection.fd =
        socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (connection.fd < 0) {
      connection.finish(Availability::Error, errno);
      continue;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const char *name = index == 0 ? "zygiskd32" : "zygiskd64";
    const size_t name_length = strlen(name);
    memcpy(address.sun_path + 1, name, name_length);
    const auto address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name_length);
    if (connect(connection.fd, reinterpret_cast<sockaddr *>(&address),
                address_length) != 0 &&
        errno != EINPROGRESS) {
      const int error = errno;
      connection.finish(error == ENOENT || error == ECONNREFUSED
                            ? Availability::Missing
                        : error == EAGAIN ? Availability::Unresponsive
                                          : Availability::Error,
                        error);
      continue;
    }
    connection.stage = 1;
  }
  for (;;) {
    std::array<pollfd, 2> descriptors{};
    bool pending = false;
    for (size_t index = 0; index < connections.size(); ++index) {
      const auto &connection = connections[index];
      descriptors[index] = {
          connection.stage == 0 ? -1 : connection.fd,
          static_cast<short>(connection.stage == 3 ? POLLIN : POLLOUT), 0};
      pending |= connection.stage != 0;
    }
    if (!pending)
      break;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    if (remaining.count() <= 0)
      break;
    const int ready = poll(descriptors.data(), descriptors.size(),
                           static_cast<int>(remaining.count()));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0)
      break;
    for (size_t index = 0; index < connections.size(); ++index) {
      auto &connection = connections[index];
      if (connection.stage == 0 || descriptors[index].revents == 0)
        continue;
      if (connection.stage == 1) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &error, &length) !=
                0 ||
            error != 0) {
          connection.finish(Availability::Error, error != 0 ? error : errno);
          continue;
        }
        if (!identity(connection, static_cast<uint32_t>(index + 1), deadline)) {
          const bool expired = Clock::now() >= deadline;
          connection.finish(expired ? Availability::Unresponsive
                                    : Availability::IdentityError,
                            expired ? ETIMEDOUT : EACCES);
          continue;
        }
        connection.stage = 2;
      }
      if (connection.stage == 2) {
        const ssize_t sent = send(connection.fd, &kRequest, sizeof(kRequest),
                                  MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR))
          continue;
        if (sent != 1) {
          connection.finish(Availability::Error, sent < 0 ? errno : EIO);
          continue;
        }
        connection.stage = 3;
      }
      auto *bytes =
          reinterpret_cast<unsigned char *>(&connection.result.snapshot);
      const ssize_t received =
          recv(connection.fd, bytes + connection.received,
               sizeof(Snapshot) - connection.received, MSG_DONTWAIT);
      if (received < 0 && (errno == EAGAIN || errno == EINTR))
        continue;
      if (received <= 0) {
        const bool unsupported = received == 0 && connection.received == 0;
        connection.finish(unsupported ? Availability::Unsupported
                                      : Availability::Error,
                          unsupported     ? EOPNOTSUPP
                          : received == 0 ? EPROTO
                                          : errno);
        continue;
      }
      connection.received += static_cast<size_t>(received);
      const auto &snapshot = connection.result.snapshot;
      if (connection.received >= 8 &&
          (snapshot.version != kVersion || snapshot.size != sizeof(Snapshot))) {
        connection.finish(Availability::Unsupported, EPROTONOSUPPORT);
        continue;
      }
      if (connection.received != sizeof(Snapshot))
        continue;
      if (snapshot.pid != static_cast<uint32_t>(connection.pid) ||
          snapshot.abi != index + 1 ||
          snapshot.start_ticks != connection.start_ticks ||
          process_start_ticks(connection.pid) != connection.start_ticks) {
        connection.finish(Availability::IdentityError, ESTALE);
        continue;
      }
      if (connection.pidfd >= 0) {
        pollfd process{connection.pidfd, POLLIN, 0};
        if (poll(&process, 1, 0) != 0) {
          connection.finish(Availability::IdentityError, ESTALE);
          continue;
        }
      }
      char executable_path[64];
      (void)snprintf(executable_path, sizeof(executable_path), "/proc/%d/exe",
                     connection.pid);
      struct stat executable{};
      if (stat(executable_path, &executable) != 0 ||
          executable.st_dev != connection.executable_device ||
          executable.st_ino != connection.executable_inode) {
        connection.finish(Availability::IdentityError, ESTALE);
        continue;
      }
      if (snapshot.catalog_error < 0 || snapshot.read_error < 0 ||
          snapshot.save_error < 0 || snapshot.reserved64 != 0 ||
          (snapshot.flags & ~uint32_t{63}) != 0 ||
          static_cast<uint32_t>(snapshot.reader_state) >
              static_cast<uint32_t>(ReaderState::Retrying)) {
        connection.finish(Availability::Error, EPROTO);
        continue;
      }
      connection.finish(Availability::Available, 0);
    }
  }
  std::array<QueryResult, 2> result;
  for (size_t index = 0; index < connections.size(); ++index) {
    auto &connection = connections[index];
    if (connection.stage != 0)
      connection.finish(Availability::Unresponsive, ETIMEDOUT);
    result[index] = connection.result;
  }
  return result;
}

} // namespace client_detail

inline std::array<QueryResult, 2> query_all(int timeout_ms = kQueryTimeoutMs) {
  return client_detail::query_mask(3, timeout_ms);
}

inline QueryResult query(uint32_t abi, int timeout_ms = kQueryTimeoutMs) {
  if (abi != 1 && abi != 2) {
    QueryResult result;
    result.error = EINVAL;
    return result;
  }
  return client_detail::query_mask(1U << (abi - 1), timeout_ms)[abi - 1];
}

} // namespace yukizygisk::health
