#include "kernel/feature/yukizygisk/native_stub.h"
#include <asm/unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#if defined(__aarch64__)
#include "kernel/feature/yukizygisk/native_blob64.h"
#define RESOLVER yz_native_resolver64
#else
#include "kernel/feature/yukizygisk/native_blob32.h"
#define RESOLVER yz_native_resolver32
#endif

/* An actual Bionic-started executable without any DT_NEEDED libraries. */
static long call(long number, long a, long b, long c, long d, long e, long f) {
#if defined(__aarch64__)
  register long r0 __asm__("x0") = a, r1 __asm__("x1") = b;
  register long r2 __asm__("x2") = c, r3 __asm__("x3") = d;
  register long r4 __asm__("x4") = e, r5 __asm__("x5") = f;
  register long nr __asm__("x8") = number;
#else
  register long r0 __asm__("r0") = a, r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c, r3 __asm__("r3") = d;
  register long r4 __asm__("r4") = e, r5 __asm__("r5") = f;
  register long nr __asm__("r7") = number;
#endif
  __asm__ volatile("svc #0"
                   : "+r"(r0)
                   : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(nr)
                   : "memory", "cc");
  return r0;
}

__attribute__((noreturn)) void yz_empty_main(uintptr_t *stack) {
  uintptr_t *aux = stack + *stack + 2;
  while (*aux++) {
  }
  uintptr_t linker = 0;
  for (; aux[0]; aux += 2)
    if (aux[0] == 7)
      linker = aux[1];
#if defined(__aarch64__)
  const long map = __NR_mmap;
#else
  const long map = __NR_mmap2;
#endif
  const size_t length = 16384;
  void *code = (void *)call(map, 0, length, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  for (size_t i = 0; i < sizeof(RESOLVER); ++i)
    ((volatile unsigned char *)code)[i] = RESOLVER[i];
#if defined(__arm__)
  call(__ARM_NR_cacheflush, (long)code, (long)code + length, 0, 0, 0, 0);
#else
  // QEMU invalidates translated code on writes. The actual launcher's kernel
  // staging path calls flush_icache_range before exposing the generated image.
  __asm__ volatile("dsb ish; isb" ::: "memory");
#endif
  struct yz_native_stub data;
  for (size_t i = 0; i < sizeof(data); ++i)
    ((volatile unsigned char *)&data)[i] = 0;
  data.magic = YZ_NATIVE_STUB_MAGIC;
  data.version = YZ_NATIVE_STUB_VERSION;
  data.linker = linker;
  data.process = call(__NR_getpid, 0, 0, 0, 0, 0, 0);
  data.generation = 1;
  data.loader_fd = 0;
  ((void (*)(struct yz_native_stub *))code)(&data);
  const int success =
      data.status == 4 && !data.stage && !data.loader && !data.bootstrap;
  const char message[] =
      "PASS no-libc generated resolver refuses before loader/Core\n";
  if (success)
    call(__NR_write, 1, (long)message, sizeof(message) - 1, 0, 0, 0);
  call(__NR_exit_group, success ? 0 : 1, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}

__attribute__((naked, noreturn)) void _start(void) {
#if defined(__aarch64__)
  __asm__ volatile("mov x0, sp\n b yz_empty_main");
#else
  __asm__ volatile("mov r0, sp\n b yz_empty_main");
#endif
}
