#ifndef YZ_TANGO_H
#define YZ_TANGO_H

#include <linux/types.h>

int yz_tango_enable(void);
void yz_tango_disable(void);
bool yz_tango_active(void);
bool yz_tango_is_process(void);
void yz_tango_linker_offsets(u64 *dlopen, u64 *dlsym);
int yz_tango_prepare(u32 *generation);
void yz_tango_finish(u32 generation, int fd, bool redirected);

#endif
