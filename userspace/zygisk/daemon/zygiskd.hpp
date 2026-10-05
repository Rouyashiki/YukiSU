#pragma once

#include <cstdint>

namespace zygiskd {

/* One-byte request protocol. */
enum class Request : uint8_t {
  GetProcessFlags = 1,
  GetModuleCount = 2,
  GetModuleFd = 3,
  ConnectCompanion = 4,
  GetModuleDir = 5,
  GetConfig = 6, // -> struct yz_config (runtime config from yzconfig.json)
  // 8-9 reserved
  Log = 10,
  PatchText = 11,
  PatchTextV2 = 29,
  ReportZygote = 12,
  GetNativeModuleCount = 13,
  GetNativeModuleInfo = 14,
  GetNativeModuleFd = 15,
  ConnectNativeCompanion = 16,
  RestoreLoadPolicy = 17,
  ReportNativeInjection = 18,
  GetRuntimeGeneration = 21,
  WriteLog = 22,
  OpenHyosControlSession = 23,
  ReportHyosCallback = 24,
  GetModuleInfo = 25,
  ReportZygoteModule = 26,
  GetModuleLoadState = 27, // -> uint8_t: 0 available, 1 suspended, 2 invalid.
  GetHealth = 28,
};

enum class LogLevel : uint8_t {
  Debug = 0,
  Info = 1,
  Warning = 2,
  Error = 3,
};

enum class LogSource : uint8_t {
  Daemon = 0,
  Zygisk = 1,
  Native = 2,
  Linker = 3,
};

inline constexpr uint16_t kLogMessageMax = 1024;

struct LogHeader {
  LogLevel level;
  LogSource source;
  uint16_t length;
};

static_assert(sizeof(LogHeader) == 4);

inline constexpr uint32_t kNativeModuleNameMax = 64;
inline constexpr uint32_t kNativeModuleTargetMax = 256;
inline constexpr uint32_t kNativeModulePathMax = 512;

struct NativeModuleInfo {
  uint8_t target_type;
  uint8_t has_companion;
  uint16_t reserved;
  char module_id[kNativeModuleNameMax];
  char target[kNativeModuleTargetMax];
  char lib_path[kNativeModulePathMax];
};

struct ModuleInfo {
  char module_id[kNativeModuleNameMax];
};

struct ZygoteModuleReport {
  uint32_t zygote_pid;
  uint32_t generation;
  uint8_t state;
  uint8_t reserved[3];
  char module_id[kNativeModuleNameMax];
};

static_assert(sizeof(ModuleInfo) == 64);
static_assert(sizeof(ZygoteModuleReport) == 76);

#if defined(__LP64__)
inline constexpr char kSocketName[] = "zygiskd64";
#else
inline constexpr char kSocketName[] = "zygiskd32";
#endif // #if defined(__LP64__)

} // namespace zygiskd

extern "C" int zygiskd_main();
