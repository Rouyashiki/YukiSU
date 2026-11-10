#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int yukisu_unpack_xz_range_to_fd(const uint8_t* packed, size_t packed_size, uint64_t range_offset,
                                 size_t range_size, uint64_t unpacked_size, int dst_fd);
int yukisu_unpack_xz_range_to_buffer(const uint8_t* packed, size_t packed_size,
                                     uint64_t range_offset, size_t range_size,
                                     uint64_t unpacked_size, uint8_t* dst);

#ifdef __cplusplus
}
#endif
