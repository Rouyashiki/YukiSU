#include "catalog.hpp"

#include "../crash_evidence.hpp"
#include "../daemon_health.hpp"

#include <array>
#include <cstring>
#include <map>

namespace yukizygisk::catalog {
namespace {

bool write_all(int fd, const std::string &text) {
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (count == 0)
        errno = EIO;
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

class Sha256 {
public:
  void update(const unsigned char *data, size_t size) {
    bytes_ += size;
    while (size != 0) {
      const size_t count = std::min(size, block_.size() - used_);
      memcpy(block_.data() + used_, data, count);
      used_ += count;
      data += count;
      size -= count;
      if (used_ == block_.size()) {
        transform();
        used_ = 0;
      }
    }
  }
  std::string finish() {
    const uint64_t bits = bytes_ * 8;
    const unsigned char marker = 0x80;
    update(&marker, 1);
    const unsigned char zero = 0;
    while (used_ != 56)
      update(&zero, 1);
    unsigned char length[8];
    for (unsigned index = 0; index < 8; ++index)
      length[index] = static_cast<unsigned char>(bits >> (56 - index * 8));
    update(length, sizeof(length));
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto word : state_)
      for (unsigned shift = 32; shift != 0; shift -= 4)
        result.push_back(hex[(word >> (shift - 4)) & 15]);
    return result;
  }

private:
  static uint32_t rotate(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
  }
  void transform() {
    static constexpr std::array<uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<uint32_t, 64> words{};
    for (size_t index = 0; index < 16; ++index)
      for (size_t byte = 0; byte < 4; ++byte)
        words[index] = (words[index] << 8) | block_[(index * 4) + byte];
    for (size_t index = 16; index < words.size(); ++index) {
      const auto x = words[index - 15], y = words[index - 2];
      words[index] =
          words[index - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) +
          words[index - 7] + (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    auto work = state_;
    for (size_t index = 0; index < words.size(); ++index) {
      const auto a = work[0], b = work[1], c = work[2];
      const auto e = work[4], f = work[5], g = work[6];
      const uint32_t first =
          work[7] + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
          ((e & f) ^ (~e & g)) + constants[index] + words[index];
      const uint32_t second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                              ((a & b) ^ (a & c) ^ (b & c));
      for (size_t slot = 7; slot != 0; --slot)
        work[slot] = work[slot - 1];
      work[4] += first;
      work[0] = first + second;
    }
    for (size_t index = 0; index < state_.size(); ++index)
      state_[index] += work[index];
  }
  std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  size_t used_ = 0;
  uint64_t bytes_ = 0;
};

json::Value entry_json(const Entry &entry) {
  auto value = json::Value::object();
  value["kind"] = static_cast<int>(entry.kind);
  value["target_type"] = static_cast<int>(entry.target_type);
  value["has_companion"] = entry.has_companion;
  value["id"] = entry.id;
  value["target"] = entry.target;
  value["library"] = entry.library;
  value["manifest"] = entry.manifest;
  value["library_sha256"] = entry.library_sha256;
  value["manifest_sha256"] = entry.manifest_sha256;
  return value;
}

json::Value entries_json(const std::vector<Entry> &entries) {
  auto value = json::Value::array();
  for (const auto &entry : entries)
    value.push_back(entry_json(entry));
  return value;
}

bool same_file(const struct stat &left, const struct stat &right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size && left.st_uid == right.st_uid &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

std::string file_identity(const struct stat &status) {
  return std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) +
         ":" + std::to_string(status.st_size) + ":" +
         std::to_string(status.st_mtim.tv_sec) + ":" +
         std::to_string(status.st_mtim.tv_nsec) + ":" +
         std::to_string(status.st_ctim.tv_sec) + ":" +
         std::to_string(status.st_ctim.tv_nsec);
}

std::string publication_marker(int fd) {
  char value[128];
  const ssize_t count = pread(fd, value, sizeof(value), 0);
  return count > 0 ? std::string(value, static_cast<size_t>(count))
                   : std::string{};
}

bool identity_error(int error) {
  return error == ENOENT || error == ENOTDIR || error == ESTALE ||
         error == ELOOP || error == EACCES;
}

} // namespace

std::string sha256(const std::string &text) {
  Sha256 digest;
  digest.update(reinterpret_cast<const unsigned char *>(text.data()),
                text.size());
  return digest.finish();
}

bool fingerprint_file(const std::string &path, std::string *digest,
                      std::string *identity) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat before{}, after{}, named{};
  bool ok = fstat(fd, &before) == 0;
  if (ok && (!S_ISREG(before.st_mode) || before.st_uid != 0)) {
    ok = false;
    errno = EACCES;
  }
  Sha256 hash;
  std::array<unsigned char, 32768> bytes{};
  while (ok) {
    const ssize_t count = read(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0)
      break;
    hash.update(bytes.data(), static_cast<size_t>(count));
  }
  if (ok) {
    ok = fstat(fd, &after) == 0 && stat(path.c_str(), &named) == 0 &&
         same_file(before, after) && same_file(after, named);
    if (!ok)
      errno = ESTALE;
  }
  const int error = errno;
  close(fd);
  if (ok) {
    *digest = hash.finish();
    if (identity != nullptr)
      *identity = file_identity(after);
  } else
    errno = error;
  return ok;
}

bool Catalog::fingerprints(std::vector<Entry> *entries) {
  if (entries->size() > 4096) {
    errno = E2BIG;
    return false;
  }
  std::map<std::string, std::pair<std::string, std::string>> cache;
  for (auto &entry : *entries) {
    for (const auto pair :
         {std::make_pair(&entry.library, &entry.library_sha256),
          std::make_pair(&entry.manifest, &entry.manifest_sha256)}) {
      if (pair.first->empty()) {
        pair.second->clear();
        continue;
      }
      auto found = cache.find(*pair.first);
      if (found == cache.end()) {
        std::string digest, identity;
        if (!fingerprint_file(*pair.first, &digest, &identity))
          return false;
        found = cache
                    .emplace(*pair.first, std::make_pair(std::move(digest),
                                                         std::move(identity)))
                    .first;
      }
      *pair.second = found->second.first;
      if (pair.first == &entry.library)
        entry.library_identity = found->second.second;
    }
  }
  return true;
}

bool Catalog::initialize(const std::string &directory, uint32_t abi,
                         const std::string &boot, int lifetime_lock,
                         bool live_consumers, std::vector<Entry> *entries) {
  directory_ = directory;
  boot_ = boot;
  abi_ = abi;
  lifetime_lock_ = lifetime_lock;
  if (boot.empty() || (abi != 1 && abi != 2) || lifetime_lock < 0) {
    return failure(EINVAL);
  }
  std::string text;
  errno = 0;
  const std::string catalog_path = directory + "/" + health::catalog_name(abi);
  struct stat saved_status{};
  if (lstat(catalog_path.c_str(), &saved_status) == 0 &&
      (!S_ISREG(saved_status.st_mode) || saved_status.st_uid != 0 ||
       saved_status.st_nlink != 1 || (saved_status.st_mode & 0777) != 0600))
    return failure(EACCES, true);
  errno = 0;
  const bool exists =
      crash::read_bounded(catalog_path, crash::kMaxDocument, &text);
  const int read_error = errno;
  errno = 0;
  const std::string marker = publication_marker(lifetime_lock);
  if (errno != 0)
    return failure(errno);
  const bool marked = marker == boot;
  marker_confirmed_ = false;
  const bool prior_boot = marker.size() == 36 && marker != boot;
  auto saved = json::parse(text);
  const bool same_boot = saved.at("boot_id").string_or("") == boot;
  const bool valid = saved.is_object() && saved.at("version").u32_or(0) == 1 &&
                     saved.at("abi").u32_or(0) == abi &&
                     saved.at("published").is_bool() &&
                     saved.at("entries").is_array() &&
                     saved.at("sha256").string_or("") ==
                         sha256(json::dump(saved.at("entries")));
  if ((!exists && read_error != ENOENT) ||
      (exists && !valid && (!prior_boot || live_consumers)) ||
      ((!exists || !same_boot) && (marked || live_consumers))) {
    return failure(ESTALE, true);
  }
  if (!fingerprints(entries)) {
    verified_ = false;
    return failure(errno, exists && same_boot && identity_error(errno));
  }
  if (exists && valid && same_boot) {
    const auto &previous = saved.at("entries").a;
    std::vector<Entry> ordered;
    std::vector<bool> used(entries->size(), false);
    bool matched = previous.size() == entries->size();
    for (const auto &value : previous) {
      const auto serialized = json::dump(value);
      size_t index = 0;
      for (; index < entries->size(); ++index)
        if (!used[index] &&
            json::dump(entry_json((*entries)[index])) == serialized)
          break;
      if (index == entries->size()) {
        matched = false;
        break;
      }
      used[index] = true;
      ordered.push_back((*entries)[index]);
    }
    if (!matched || (marked && !saved.at("published").b)) {
      return failure(ESTALE, true);
    }
    *entries = std::move(ordered);
    published_ = saved.at("published").b;
    entries_ = *entries;
    digest_ = sha256(json::dump(entries_json(entries_)));
    verified_ = true;
    last_error_ = 0;
    return true;
  }
  return save(*entries, false);
}

bool Catalog::save(const std::vector<Entry> &entries, bool published) {
  auto document = json::Value::object();
  document["version"] = 1;
  document["boot_id"] = boot_;
  document["abi"] = static_cast<int>(abi_);
  document["published"] = published;
  document["entries"] = entries_json(entries);
  document["sha256"] = sha256(json::dump(document.at("entries")));
  const std::string text = json::dump(document);
  if (text.size() > crash::kMaxDocument) {
    return failure(EFBIG);
  }
  const int directory =
      open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0)
    return failure(errno);
  struct stat status{};
  if (fstat(directory, &status) != 0 || status.st_uid != 0 ||
      (status.st_mode & 0777) != 0700) {
    close(directory);
    return failure(EACCES);
  }
  std::string temporary;
  int fd = -1;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    temporary = std::string(health::catalog_name(abi_)) + ".tmp." +
                std::to_string(getpid()) + "." + std::to_string(attempt);
    fd = openat(directory, temporary.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST)
      break;
  }
  bool ok = fd >= 0;
  if (ok)
    ok = write_all(fd, text) && fsync(fd) == 0;
  int error = ok ? 0 : errno;
  if (fd >= 0 && close(fd) != 0 && ok) {
    ok = false;
    error = errno;
  }
  if (ok) {
    ok = renameat(directory, temporary.c_str(), directory,
                  health::catalog_name(abi_)) == 0 &&
         fsync(directory) == 0;
    if (!ok)
      error = errno;
  }
  if (!ok && fd >= 0)
    (void)unlinkat(directory, temporary.c_str(), 0);
  close(directory);
  if (!ok) {
    return failure(error);
  }
  entries_ = entries;
  published_ = published;
  digest_ = sha256(json::dump(entries_json(entries_)));
  verified_ = true;
  last_error_ = 0;
  return true;
}

bool Catalog::publish() {
  if (reboot_required_) {
    return failure(ESTALE, true);
  }
  if (!verified_)
    return failure(last_error_);
  if (published_ && marker_confirmed_)
    return true;
  if (!published_ && !save(entries_, true))
    return false;
  ssize_t count;
  do {
    count = pwrite(lifetime_lock_, boot_.data(), boot_.size(), 0);
  } while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(boot_.size()))
    return failure(count < 0 ? errno : EIO);
  if (ftruncate(lifetime_lock_, static_cast<off_t>(boot_.size())) != 0 ||
      fsync(lifetime_lock_) != 0) {
    return failure(errno);
  }
  marker_confirmed_ = true;
  last_error_ = 0;
  return true;
}

bool Catalog::replace(std::vector<Entry> *entries) {
  if (!fingerprints(entries)) {
    verified_ = false;
    return failure(errno, published_ && identity_error(errno));
  }
  if (published_) {
    std::vector<std::string> previous, current;
    for (const auto &entry : entries_)
      previous.push_back(json::dump(entry_json(entry)));
    for (const auto &entry : *entries)
      current.push_back(json::dump(entry_json(entry)));
    std::sort(previous.begin(), previous.end());
    std::sort(current.begin(), current.end());
    if (previous != current) {
      return failure(ESTALE, true);
    }
    *entries = entries_;
    verified_ = true;
    last_error_ = 0;
    return true;
  }
  return save(*entries, false);
}

bool Catalog::validate_library_fd(const std::string &path, int fd) {
  if (fd < 0)
    return failure(errno, identity_error(errno));
  struct stat status{};
  if (fstat(fd, &status) != 0)
    return failure(errno, identity_error(errno));
  const auto entry = std::find_if(
      entries_.begin(), entries_.end(),
      [&](const auto &candidate) { return candidate.library == path; });
  if (reboot_required_ || entry == entries_.end() || !S_ISREG(status.st_mode) ||
      status.st_uid != 0 || entry->library_identity.empty() ||
      file_identity(status) != entry->library_identity) {
    return failure(ESTALE, true);
  }
  last_error_ = 0;
  return true;
}

bool Catalog::failure(int error, bool reboot) {
  last_error_ = error != 0 ? error : EIO;
  reboot_required_ |= reboot;
  errno = last_error_;
  return false;
}

} // namespace yukizygisk::catalog
