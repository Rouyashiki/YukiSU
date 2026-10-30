#pragma once

#include <cstdint>
#include <link.h>

bool core_private_tsd_used() noexcept;
// Requires other participants to have exited and no active exception.
// Drains current-thread values, without deleting keys or proving quiescence.
int core_drain_current_thread_tsd() noexcept;
extern "C" int core_system_iterate_phdr(int (*)(dl_phdr_info *, size_t, void *),
                                        void *);

using CoreUnmapFunction = int (*)(void *, size_t);
// Fixed NDK CRT profile; callers must retain this image through termination.
bool core_prepare_runtime_retirement(uintptr_t base, size_t span) noexcept;
bool core_runtime_profile_ready(uintptr_t base, size_t span) noexcept;
CoreUnmapFunction core_retirement_unmap() noexcept;
int core_finish_runtime_retirement() noexcept;
bool core_runtime_closed() noexcept;
extern "C" bool zygisk_adopt_anonymous_image(uintptr_t base, size_t span,
                                             void *loader);
