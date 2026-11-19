#include "userspace/ksud/src/yzctl.cpp"

#include <cassert>

namespace ksud {

static bool legacy;
static bool runtime_denial;
static unsigned policy_reads;

int ksuctl(int request, void *argument) {
  if (request == KSU_IOCTL_YZ_GET_RUNTIME)
    return 0;
  assert(request == static_cast<int>(KSU_IOCTL_YZ_GET_FIXED_POLICY));
  ++policy_reads;
  if (legacy) {
    errno = ENOTTY;
    return -1;
  }
  auto *status = static_cast<yz_fixed_policy_status *>(argument);
  status->version = YZ_FIXED_POLICY_VERSION;
  status->error = runtime_denial ? 0 : -EACCES;
  strcpy(status->source, runtime_denial ? "netd" : "domain");
  strcpy(status->target, runtime_denial ? "yukizygisk_file" : "test_daemon");
  strcpy(status->class_name, runtime_denial ? "file" : "fd");
  status->missing_av = runtime_denial ? 0x44412U : 4U;
  return 0;
}

std::pair<uint64_t, bool> get_feature(uint32_t) { return {1, true}; }

} // namespace ksud

int main(int argc, char **argv) {
  ksud::legacy = argc == 2 && strcmp(argv[1], "legacy") == 0;
  ksud::runtime_denial = argc == 2 && strcmp(argv[1], "denied") == 0;
  ksud::RuntimeSnapshot status;
  assert(ksud::query_runtime(&status));
  ksud::print_human_status(status);
  assert(ksud::policy_reads == 1 && status.policy_supported != ksud::legacy);
}
