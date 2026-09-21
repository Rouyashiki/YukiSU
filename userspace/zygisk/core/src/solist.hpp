#pragma once

#include <cstdint>

namespace yuki::solist {

/* Resolve linker internals before the zygote forks app namespaces. */
bool prepare_linker();

/* Unlink matching soinfo entries. */
int hide_from_solist(const char *path_substr);

/* Drop matching libraries through linker unload. */
int drop_module_from_solist(const char *path_substr, bool dry_run,
                            bool keep_mapped = true);

/* Drop the library containing an address. */
int drop_lib_containing(uintptr_t addr, bool keep_mapped = false);

/* Release a loaded library while preserving its mappings for deferred unmap. */
int release_lib_containing(uintptr_t addr);

/* Anonymize matching VMAs. */
int spoof_virtual_maps(const char *path_substr, bool private_only);

/* Anonymize VMAs backed by one exact open fd. */
int spoof_fd_maps(int fd, bool private_only);

/* Anonymize file-backed segments of the loaded object containing an address. */
int spoof_loaded_object_maps(uintptr_t address, bool private_only);

/* Name bare executable anonymous VMAs. */
int name_anonymous_exec();

} // namespace yuki::solist
