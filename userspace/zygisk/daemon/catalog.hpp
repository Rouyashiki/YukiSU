#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yukizygisk::catalog {

struct Entry {
  uint32_t kind = 0;
  uint32_t target_type = 0;
  bool has_companion = false;
  std::string id;
  std::string target;
  std::string library;
  std::string manifest;
  std::string library_sha256;
  std::string manifest_sha256;
  std::string library_identity;
};

std::string sha256(const std::string &text);
bool fingerprint_file(const std::string &path, std::string *digest,
                      std::string *identity = nullptr);

class Catalog {
public:
  bool initialize(const std::string &directory, uint32_t abi,
                  const std::string &boot, int lifetime_lock,
                  bool live_consumers, std::vector<Entry> *entries);
  bool publish();
  bool replace(std::vector<Entry> *entries);
  bool validate_library_fd(const std::string &path, int fd);
  [[nodiscard]] bool frozen() const { return published_; }
  [[nodiscard]] bool reboot_required() const { return reboot_required_; }
  [[nodiscard]] const std::string &digest() const { return digest_; }
  [[nodiscard]] int error() const { return last_error_; }

private:
  bool save(const std::vector<Entry> &entries, bool published);
  bool failure(int error, bool reboot = false);
  static bool fingerprints(std::vector<Entry> *entries);
  std::string directory_;
  std::string boot_;
  uint32_t abi_ = 0;
  int lifetime_lock_ = -1;
  std::vector<Entry> entries_;
  std::string digest_;
  bool published_ = false;
  bool marker_confirmed_ = false;
  bool reboot_required_ = false;
  bool verified_ = false;
  int last_error_ = 0;
};

} // namespace yukizygisk::catalog
