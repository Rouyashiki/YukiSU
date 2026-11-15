#include "boot_backup.hpp"

#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "lkm_image_core.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

namespace fs = std::filesystem;

namespace ksud::boot_backup {
namespace {

constexpr std::string_view kReferencePrefix = "imgpatch_backup_";

class BackupLock {
public:
    explicit BackupLock(const fs::path& directory)
        : fd_(open((directory / ".boot_backup.lock").c_str(),
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        if (fd_ >= 0 && flock(fd_, LOCK_EX) != 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    ~BackupLock() {
        if (fd_ >= 0)
            close(fd_);
    }
    BackupLock(const BackupLock&) = delete;
    BackupLock& operator=(const BackupLock&) = delete;
    BackupLock(BackupLock&&) = delete;
    BackupLock& operator=(BackupLock&&) = delete;
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

private:
    int fd_;
};

bool valid_sha1(std::string_view value) {
    return value.size() == 40 && std::all_of(value.begin(), value.end(), [](char digit) {
               return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
           });
}

std::vector<fs::path> directories() {
    return {KSU_BACKUP_DIR, fs::path("/data/user_de") / std::to_string(getuid() / 100000) /
                                DEFAULT_PACKAGE_NAME / "boot_backup"};
}

bool sync_directory(const fs::path& directory) {
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

std::optional<std::string> image_id(const fs::path& image) {
    std::vector<uint8_t> data;
    if (!read_file_bytes(image, &data))
        return std::nullopt;
    const auto info = boot::lkm_image::parse_boot_image(data);
    if (!info || info.value().kernel.empty())
        return std::nullopt;
    // Bind the header and payloads, excluding trailing partition padding.
    size_t payload_end = info.value().header.size;
    for (const auto range :
         {info.value().kernel, info.value().ramdisk, info.value().second, info.value().extra,
          info.value().recovery_dtbo, info.value().dtb, info.value().signature}) {
        if (!range.empty())
            payload_end = std::max(payload_end, range.offset + range.size);
    }
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(data.data(), payload_end, digest.data(), 0) != 0)
        return std::nullopt;
    std::string result;
    for (const auto byte : digest)
        append_hex(&result, byte, false, 2);
    return result;
}

std::optional<std::string> read_reference(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size != 41)
        return std::nullopt;
    const auto data = read_file(path);
    if (!data || data->size() != 41 || data->back() != '\n')
        return std::nullopt;
    const std::string sha1 = data->substr(0, 40);
    return valid_sha1(sha1) ? std::make_optional(sha1) : std::nullopt;
}

std::optional<Backup> stage_backup(const fs::path& directory, const std::string& sha1,
                                   const fs::path& reference, const fs::path& workdir) {
    if (!valid_sha1(sha1))
        return std::nullopt;
    const fs::path stored = directory / (std::string(KSU_BACKUP_FILE_PREFIX) + sha1);
    const fs::path staged = workdir / ("verified-stock-" + sha1 + ".img");
    if (!copy_file_data(stored, staged, 0600) || calculate_sha1(staged) != sha1) {
        std::error_code ignored;
        fs::remove(staged, ignored);
        return std::nullopt;
    }
    return Backup{staged, stored, reference, sha1};
}

}  // namespace

std::string calculate_sha1(const fs::path& image) {
    const int fd = open(image.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return {};
    mbedtls_sha1_context context;
    mbedtls_sha1_init(&context);
    bool ok = mbedtls_sha1_starts(&context) == 0;
    std::array<unsigned char, 64UL * 1024> buffer{};
    while (ok) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = count == 0;
            break;
        }
        ok = mbedtls_sha1_update(&context, buffer.data(), static_cast<size_t>(count)) == 0;
    }
    std::array<unsigned char, 20> digest{};
    if (ok)
        ok = mbedtls_sha1_finish(&context, digest.data()) == 0;
    mbedtls_sha1_free(&context);
    if (close(fd) != 0 || !ok)
        return {};
    std::string result;
    for (const auto byte : digest)
        append_hex(&result, byte, false, 2);
    return result;
}

std::optional<Backup> save_stock(const fs::path& image) {
    for (const auto& directory : directories()) {
        if (!ensure_dir_exists(directory.string()))
            continue;
        const BackupLock lock(directory);
        if (!lock.valid())
            continue;
        std::string temporary = (directory / ".stock-XXXXXX").string();
        const int fd = mkostemp(temporary.data(), O_CLOEXEC);
        if (fd < 0)
            continue;
        const auto discard = [&temporary]() { unlink(temporary.c_str()); };
        const bool copied = copy_file_data(image, temporary, 0600);
        const bool synced = copied && fsync(fd) == 0;
        const bool closed = close(fd) == 0;
        if (!synced || !closed) {
            discard();
            continue;
        }
        const std::string sha1 = calculate_sha1(temporary);
        if (!valid_sha1(sha1)) {
            discard();
            continue;
        }
        const fs::path target = directory / (std::string(KSU_BACKUP_FILE_PREFIX) + sha1);
        if (rename(temporary.c_str(), target.c_str()) != 0) {
            discard();
            continue;
        }
        if (!sync_directory(directory))
            continue;
        printf("- Saved stock image backup: %s\n", target.c_str());
        return Backup{target, target, {}, sha1};
    }
    LOGE("Failed to save the stock image backup");
    return std::nullopt;
}

std::optional<Backup> bind_imgpatch(const Backup& backup, const fs::path& patched_image) {
    const auto id = image_id(patched_image);
    if (!id || !valid_sha1(backup.sha1))
        return std::nullopt;
    const fs::path directory = backup.stored_image.parent_path();
    const BackupLock lock(directory);
    if (!lock.valid() || calculate_sha1(backup.stored_image) != backup.sha1)
        return std::nullopt;
    const fs::path reference = directory / (std::string(kReferencePrefix) + *id + ".sha1");
    if (!write_file_atomic(reference, backup.sha1 + "\n") || !sync_directory(directory))
        return std::nullopt;
    return Backup{backup.image, backup.stored_image, reference, backup.sha1};
}

std::optional<Backup> find_imgpatch(const fs::path& patched_image, const fs::path& workdir) {
    const auto id = image_id(patched_image);
    if (!id)
        return std::nullopt;
    for (const auto& directory : directories()) {
        const BackupLock lock(directory);
        if (!lock.valid())
            continue;
        const fs::path reference = directory / (std::string(kReferencePrefix) + *id + ".sha1");
        if (const auto sha1 = read_reference(reference)) {
            if (auto backup = stage_backup(directory, *sha1, reference, workdir))
                return backup;
        }
    }
    return std::nullopt;
}

std::optional<Backup> find_stock(const std::string& sha1, const fs::path& workdir) {
    if (!valid_sha1(sha1))
        return std::nullopt;
    for (const auto& directory : directories()) {
        const BackupLock lock(directory);
        if (!lock.valid())
            continue;
        if (auto backup = stage_backup(directory, sha1, {}, workdir))
            return backup;
    }
    return std::nullopt;
}

void consume(const Backup& backup) {
    const fs::path directory = backup.stored_image.parent_path();
    const BackupLock lock(directory);
    if (!lock.valid())
        return;
    std::error_code error;
    if (!backup.reference.empty()) {
        if (read_reference(backup.reference) != backup.sha1)
            return;
        if (!fs::remove(backup.reference, error) || error)
            return;
        if (!sync_directory(directory))
            return;
    }
    for (auto it = fs::directory_iterator(directory, error);
         it != fs::directory_iterator() && !error; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.size() == kReferencePrefix.size() + 64 + 5 &&
            name.compare(0, kReferencePrefix.size(), kReferencePrefix) == 0 &&
            name.compare(name.size() - 5, 5, ".sha1") == 0 &&
            read_reference(it->path()) == backup.sha1)
            return;
    }
    if (error || calculate_sha1(backup.stored_image) != backup.sha1)
        return;
    if (!fs::remove(backup.stored_image, error) || error) {
        LOGW("Failed to remove consumed backup: %s", backup.stored_image.c_str());
        return;
    }
    if (!sync_directory(directory))
        LOGW("Failed to sync backup cleanup: %s", directory.c_str());
}

}  // namespace ksud::boot_backup
