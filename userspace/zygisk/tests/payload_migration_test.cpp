#include "userspace/ksud/src/assets.hpp"
#include "userspace/ksud/src/core/ksucalls.hpp"
#include "userspace/ksud/src/core/restorecon.hpp"
#include "userspace/ksud/src/defs.hpp"
#include "userspace/ksud/src/utils.hpp"
#include "userspace/ksud/src/yukizygisk_diagnostics.hpp"
#include "userspace/ksud/src/yukizygisk_snapshot.hpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
namespace {
fs::path assets;
std::string missing_asset;
bool enabled = true;
bool native_capability = true;
} // namespace
namespace ksud {
int ksuctl(int request, void *arg) {
  assert(request == KSU_IOCTL_YZ_GET_RUNTIME);
  static_cast<yz_runtime_query_cmd *>(arg)->capabilities =
      native_capability ? YZ_RUNTIME_CAP_NATIVE_LIBC_BOOTSTRAP_V1 : 0;
  return 0;
}
bool get_asset(const std::string &name, const uint8_t *&data, size_t &size) {
  static const uint8_t present = 1;
  data = &present;
  size = 1;
  return name != missing_asset && fs::is_regular_file(assets / name);
}
bool copy_file_data(const fs::path &source, const fs::path &target, mode_t,
                    bool overwrite) {
  std::error_code error;
  fs::copy_file(source, target,
                overwrite ? fs::copy_options::overwrite_existing
                          : fs::copy_options::none,
                error);
  return !error;
}
bool copy_asset_to_file(const std::string &name, const std::string &target) {
  return name != missing_asset && copy_file_data(assets / name, target);
}
bool ensure_dir_exists(const std::string &path) {
  return fs::is_directory(path) || fs::create_directories(path);
}
bool lsetfilecon(const fs::path &, const std::string &) { return true; }
bool is_safe_mode() { return false; }
std::pair<uint64_t, bool> get_feature(uint32_t feature) {
  assert(feature == KSU_FEATURE_YUKIZYGISK);
  return {enabled ? 1U : 0U, true};
}
std::optional<std::string> read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input), {});
}
bool write_file(const fs::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  return output.good();
}
void record_yukizygisk_early_linker(
    const yz_early_native_snapshot_header &header) {
  assert(header.magic == YZ_EARLY_NATIVE_MAGIC && header.count == 0);
}
void log_i(const char *, ...) {}
void log_w(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}
void log_e(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}
} // namespace ksud

int main(int argc, char **argv) {
  assert(argc == 2 && geteuid() == 0);
  assets = argv[1];
  using namespace ksud;
  const fs::path snapshot = fs::path(PREINIT_DIR_DEFAULT) / "yukizygisk";
  assert(ensure_dir_exists(YUKIZYGISK_DIR));
  fs::create_directories(snapshot);
  fs::create_directories(YUKIZYGISK_STATE_DIR);
  assert(write_file(YUKIZYGISK_CONFIG_PATH,
                    R"({"early_load":true,"yukilinker":true})"));
  const char *legacy[] = {"libyukilinker.so", "libyukilinker32.so",
                          "libyukilinker64.so"};
  for (const char *name : legacy) {
    assert(write_file(fs::path(YUKIZYGISK_DIR) / name, "old"));
    assert(write_file(snapshot / name, "old"));
  }
  assert(ensure_yukizygisk(false) == 0);
  assert(refresh_yukizygisk_early_snapshot() == 0);
  for (const char *name : {"libyloader32.so", "libyloader64.so"}) {
    const auto expected = read_file((assets / name).string());
    assert(expected &&
           expected == read_file((fs::path(YUKIZYGISK_DIR) / name).string()));
    assert(expected == read_file((snapshot / name).string()));
  }
  for (const char *name : legacy) {
    assert(!fs::exists(fs::path(YUKIZYGISK_DIR) / name));
    assert(!fs::exists(snapshot / name));
  }
  assert(fs::exists(snapshot / "native_snapshot.bin"));
  native_capability = false;
  assert(refresh_yukizygisk_early_snapshot() == 0);
  const auto next_boot = read_file((snapshot / "native_snapshot.bin").string());
  yz_early_native_snapshot_header header{};
  assert(next_boot && next_boot->size() >= sizeof(header));
  memcpy(&header, next_boot->data(), sizeof(header));
  assert(header.version == 3);
  native_capability = true;
  missing_asset = "libyloader64.so";
  assert(fs::remove(ZYLOADER64_PATH));
  assert(refresh_yukizygisk_early_snapshot() == 1 && !fs::exists(snapshot));
  missing_asset.clear();
  assert(ensure_yukizygisk(false) == 0 &&
         refresh_yukizygisk_early_snapshot() == 0);
  enabled = false;
  assert(refresh_yukizygisk_early_snapshot() == 0 && !fs::exists(snapshot));
  puts("PASS payload and snapshot migration, byte identity, legacy cleanup and "
       "staging failure");
}
