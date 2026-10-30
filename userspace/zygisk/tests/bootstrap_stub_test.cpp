#include "userspace/zygisk/endpoint.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bootstrap_fixture.hpp"

namespace ep = yukizygisk::endpoint;
static int failure;
static int loader_fd, core_fd;
static bool use_loader, use_tango, called;
static yz_early_packet_state packet;
static void *stub;

static void closed(int fd) {
  assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

[[noreturn]] static void resume(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2,
                                uintptr_t arg3) {
  assert(arg0 == 0x123456);
  if (use_tango) {
    assert(arg1 == 0x234567 && arg2 == 0x345678 && arg3 == 0x456789);
  }
  assert(called == (failure == 0));
  closed(loader_fd);
  if (use_loader)
    closed(core_fd);
  closed(packet.packet_fd);
  closed(packet.endpoint_fd);
  for (unsigned i = 0; i < packet.module_fd_count; ++i)
    closed(packet.module_fds[i]);
  munmap(stub, getpagesize());
  _exit(0);
}

static void consume(int argument) {
  assert(argument == packet.packet_fd + 1);
  assert(ep::initialize(argument - 1, true));
  ep::close_packet(argument - 1, false);
  ep::release();
  called = true;
}

static void entry(int core, int argument) {
  if (use_loader) {
    assert(core == core_fd && fcntl(core, F_GETFD) >= 0);
    close(core);
  }
  consume(argument);
}

static void tango_entry(uint32_t got, uint32_t original, uint32_t address,
                        int core, int argument) {
  assert(got == 0x987654 &&
         original ==
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resume)));
  assert(address == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stub)));
  assert(core == core_fd && fcntl(core, F_GETFD) >= 0);
  close(core);
  consume(argument);
}

static void *open_library(const char *name, int flags, const void *extinfo,
                          uintptr_t caller) {
  assert(!strcmp(name, use_tango ? "libyukilinker32.so" : "test-loader.so"));
  assert(flags == (sizeof(void *) == 8 ? 2 : 0));
  assert(caller == reinterpret_cast<uintptr_t>(resume));
  assert(reinterpret_cast<uintptr_t>(extinfo) % 8 == 0);
  int fd;
  memcpy(&fd,
         static_cast<const char *>(extinfo) + (sizeof(void *) == 8 ? 28 : 20),
         sizeof(fd));
  assert(fd == loader_fd && fcntl(fd, F_GETFD) >= 0);
  return failure == 1 ? nullptr : reinterpret_cast<void *>(0x1234);
}

static void *find_entry(void *handle, const char *name, uintptr_t caller) {
  assert(handle == reinterpret_cast<void *>(0x1234));
  assert(!strcmp(name, use_tango ? "yuki_bootstrap_tango" : "test-entry"));
  assert(caller == reinterpret_cast<uintptr_t>(resume));
  closed(loader_fd);
  if (failure == 2)
    return nullptr;
  return use_tango ? reinterpret_cast<void *>(tango_entry)
                   : reinterpret_cast<void *>(entry);
}

static int make_file() {
  int fd = memfd_create("fixture", MFD_CLOEXEC);
  assert(fd >= 0);
  return fd;
}

static void run_case(unsigned count) {
  loader_fd = make_file();
  core_fd = use_loader ? make_file() : -1;
  packet.packet_fd = make_file();
  int writable = make_file();
  assert(ftruncate(writable, getpagesize()) == 0);
  yz_endpoint_state state{};
  state.version = YZ_ENDPOINT_VERSION;
  state.size = sizeof(state);
  state.abi = ep::kAbi;
  assert(pwrite(writable, &state, sizeof(state), 0) == sizeof(state));
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", writable);
  packet.endpoint_fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(packet.endpoint_fd >= 0);
  close(writable);
  yz_bootstrap_packet_header header{};
  header.magic = YZ_BOOTSTRAP_PACKET_MAGIC;
  header.version = YZ_BOOTSTRAP_PACKET_VERSION;
  header.header_size = sizeof(header);
  header.entry_size = sizeof(yz_early_native_packet_entry);
  header.count = packet.module_fd_count = count;
  header.endpoint_fd = packet.endpoint_fd;
  assert(write(packet.packet_fd, &header, sizeof(header)) == sizeof(header));
  for (unsigned i = 0; i < count; ++i) {
    yz_early_native_packet_entry module{};
    module.fd = packet.module_fds[i] = make_file();
    assert(write(packet.packet_fd, &module, sizeof(module)) == sizeof(module));
  }
  stub = mmap(nullptr, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(stub != MAP_FAILED);
  if (use_tango) {
    yz_tango_bootstrap(
        static_cast<u8 *>(stub), 0x987654, reinterpret_cast<uintptr_t>(resume),
        reinterpret_cast<uintptr_t>(stub),
        reinterpret_cast<uintptr_t>(open_library),
        reinterpret_cast<uintptr_t>(find_entry), loader_fd, core_fd, &packet);
  } else {
    prepare(stub, reinterpret_cast<uintptr_t>(resume),
            reinterpret_cast<uintptr_t>(open_library),
            reinterpret_cast<uintptr_t>(find_entry), loader_fd, core_fd,
            use_loader, packet);
  }
  __builtin___clear_cache(static_cast<char *>(stub),
                          static_cast<char *>(stub) + getpagesize());
  uintptr_t address = reinterpret_cast<uintptr_t>(stub);
  if (sizeof(void *) == 4)
    address |= 1;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(
      address)(0x123456, 0x234567, 0x345678, 0x456789);
  _exit(3);
}

static bool fail_restore;
extern "C" int __real_mprotect(void *, size_t, int);
extern "C" int __wrap_mprotect(void *page, size_t size, int protection) {
  if (fail_restore && protection == PROT_READ) {
    errno = EACCES;
    return -1;
  }
  return __real_mprotect(page, size, protection);
}

static void tango_slot_tests() {
  if (sizeof(void *) != 4)
    return;
  const size_t page_size = static_cast<size_t>(getpagesize());
  auto *slot =
      static_cast<uint32_t *>(mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(slot != MAP_FAILED);
  TangoBootstrap request{
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot)), 0x1111, 0x2220};
  *slot = request.stub | 1U;
  assert(mprotect(slot, page_size, PROT_READ) == 0);
  assert(restore_tango_slot(request) && *slot == request.original);
  int protection = 0;
  assert(tango_mapping_protection(reinterpret_cast<uintptr_t>(slot), page_size,
                                  protection));
  assert(protection == PROT_READ);
  assert(mprotect(slot, page_size, PROT_READ | PROT_WRITE) == 0);
  *slot = 0x3333;
  assert(!restore_tango_slot(request) && *slot == 0x3333);
  *slot = request.stub | 1U;
  assert(mprotect(slot, page_size, PROT_READ) == 0);
  fail_restore = true;
  assert(!restore_tango_slot(request) && *slot == request.original);
  assert(g_tango_protection.page == reinterpret_cast<uintptr_t>(slot));
  fail_restore = false;
  assert(restore_tango_slot(request) && g_tango_protection.page == 0);
  assert(tango_mapping_protection(reinterpret_cast<uintptr_t>(slot), page_size,
                                  protection));
  assert(protection == PROT_READ);
  assert(munmap(slot, page_size) == 0);
}

int main() {
  tango_slot_tests();
  for (unsigned mode = 0; mode < (sizeof(void *) == 4 ? 3U : 2U); ++mode) {
    use_tango = mode == 2;
    use_loader = mode == 0 || use_tango;
    for (failure = 0; failure < 3; ++failure) {
      for (unsigned count : {0U, 3U}) {
        if (use_tango && count)
          continue;
        pid_t child = fork();
        assert(child >= 0);
        if (!child)
          run_case(count);
        int status;
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
          fprintf(stderr,
                  "bootstrap failed: mode=%u failure=%d modules=%u status=%x\n",
                  mode, failure, count, status);
          return 1;
        }
      }
    }
  }
  printf("PASS: ARM%zu production stubs, handoff, continuation and failed-load "
         "fd cleanup\n",
         sizeof(void *) * 8);
}
