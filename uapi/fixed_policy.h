#ifndef YUKIZYGISK_FIXED_POLICY_UAPI_H
#define YUKIZYGISK_FIXED_POLICY_UAPI_H

#include <linux/types.h>

#define YZ_FIXED_POLICY_VERSION 1
#define YZ_FIXED_DOMAIN_MAX 32
#define YZ_FIXED_NAME_MAX 64
#define YZ_FIXED_FILE_TYPE "yukizygisk_file"
#define YZ_FIXED_FILE_CONTEXT "u:object_r:" YZ_FIXED_FILE_TYPE ":s0"

struct yz_fixed_policy_config {
  __u32 version;
  __u32 enabled;
  __u32 count;
  __u32 reserved;
  char domains[YZ_FIXED_DOMAIN_MAX][YZ_FIXED_NAME_MAX];
};

struct yz_fixed_policy_status {
  __u32 version;
  __u32 initialized;
  __u32 requested;
  __u32 active;
  __s32 error;
  __u32 missing_av;
  char source[YZ_FIXED_NAME_MAX];
  char target[YZ_FIXED_NAME_MAX];
  char class_name[YZ_FIXED_NAME_MAX];
};

#endif
