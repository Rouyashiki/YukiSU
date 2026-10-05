#pragma once

#include "../../../uapi/yukizygisk.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

struct yz_patch_io {
	void *context;
	int (*read)(void *context, unsigned long address, void *buffer,
		    unsigned int length);
	int (*write)(void *context, unsigned long address, void *buffer,
		     unsigned int length);
};

static inline unsigned int
yz_patch_transaction(const struct yz_patch_io *io, unsigned long address,
		     unsigned int length, const unsigned char *expected,
		     unsigned char *replacement, unsigned char *before,
		     unsigned char *after)
{
	int written;

	if (io->read(io->context, address, before, length) != (int)length ||
	    memcmp(before, expected, length))
		return YZ_PATCH_V2_REJECTED;

	written = io->write(io->context, address, replacement, length);
	if (written == (int)length &&
	    io->read(io->context, address, after, length) == (int)length &&
	    !memcmp(after, replacement, length))
		return YZ_PATCH_V2_APPLIED;

	io->write(io->context, address, before, length);
	if (io->read(io->context, address, after, length) != (int)length ||
	    memcmp(after, before, length))
		return YZ_PATCH_V2_INDETERMINATE;
	return YZ_PATCH_V2_REJECTED;
}
