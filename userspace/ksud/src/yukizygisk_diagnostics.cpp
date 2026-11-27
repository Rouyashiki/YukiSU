#include "yukizygisk_diagnostics.hpp"

#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <mbedtls/sha256.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ksud {
namespace {

namespace fs = std::filesystem;

enum class PreparationMode : uint8_t { Boot, Recovery };

constexpr char kBootIdPath[] = "/proc/sys/kernel/random/boot_id";
constexpr char kTombstonesPath[] = "/data/tombstones";
constexpr char kPstorePath[] = "/sys/fs/pstore";
constexpr char kBootIdName[] = "boot_id";
constexpr char kBootStateName[] = "boot.json";
constexpr char kEvidenceName[] = "evidence";
constexpr char kConfigName[] = "config.json";
constexpr char kEarlyLinkerName[] = "early_linker.json";
constexpr char kTombstonesBaselineName[] = ".tombstones.baseline";
constexpr char kPstoreBaselineName[] = ".pstore.baseline";
constexpr char kCaptureStateName[] = "capture.json";
constexpr char kLogsName[] = "logs";
constexpr char kTombstonesName[] = "tombstones";
constexpr char kPstoreName[] = "pstore";
constexpr char kLockName[] = ".lock";
constexpr char kCurrentStagingName[] = ".current.tmp";
constexpr char kLegacyStagingName[] = ".legacy.tmp";
constexpr size_t kMaxCopiedFileSize = size_t{32} * 1024U * 1024U;
constexpr size_t kMaxInventorySize = size_t{4} * 1024U * 1024U;
constexpr size_t kMaxTombstoneFiles = 32;
constexpr uint64_t kMaxTombstoneBytes = uint64_t{128} * 1024U * 1024U;
constexpr size_t kMaxPstoreFiles = 32;
constexpr uint64_t kMaxPstoreBytes = uint64_t{32} * 1024U * 1024U;
constexpr char kInvalidDigest[] = "!";
constexpr char kUnhashedDigest[] = "?";

struct FileStamp {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    int64_t mtime_sec = 0;
    int64_t mtime_nsec = 0;
    std::string digest;
};

bool same_file_stamp(const FileStamp& left, const FileStamp& right) {
    if (left.device != right.device || left.inode != right.inode || left.size != right.size ||
        left.mtime_sec != right.mtime_sec || left.mtime_nsec != right.mtime_nsec)
        return false;
    if (left.digest.empty() && right.digest.empty())
        return true;
    return left.digest != kInvalidDigest && right.digest != kInvalidDigest &&
           left.digest == right.digest;
}

using Inventory = std::map<std::string, FileStamp>;

struct ScanResult {
    bool available = false;
    Inventory files;
};

struct CaptureResult {
    bool available = false;
    size_t copied = 0;
    size_t failed = 0;
    size_t skipped = 0;
    uint64_t bytes = 0;
};

bool write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool read_all(int fd, void* data, size_t size) {
    auto* cursor = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

std::optional<std::string> read_root_regular_file_no_follow(const fs::path& path, size_t max_size) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return std::nullopt;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != 0 ||
        status.st_size < 0 || static_cast<uint64_t>(status.st_size) > max_size) {
        close(fd);
        return std::nullopt;
    }

    std::string content;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 || content.size() + static_cast<size_t>(count) > max_size) {
            close(fd);
            return std::nullopt;
        }
        if (count == 0)
            break;
        content.append(buffer.data(), static_cast<size_t>(count));
    }
    close(fd);
    return content;
}

bool sync_directory(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool is_root_directory(const fs::path& path) {
    struct stat status{};
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == 0;
}

bool ensure_secure_directory(const fs::path& path) {
    struct stat status{};
    if (lstat(path.c_str(), &status) != 0) {
        if (errno != ENOENT || mkdir(path.c_str(), 0700) != 0)
            return false;
        if (lstat(path.c_str(), &status) != 0)
            return false;
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != 0)
        return false;
    return chmod(path.c_str(), 0700) == 0;
}

bool is_root_regular_file(const fs::path& path, bool require_data = false) {
    struct stat status{};
    return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == 0 &&
           (!require_data || status.st_size > 0);
}

bool write_atomic_file(const fs::path& path, const std::string& content) {
    const fs::path temporary = path.string() + ".tmp." + std::to_string(getpid());
    (void)unlink(temporary.c_str());
    const int fd =
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;

    bool ok = write_all(fd, content.data(), content.size());
    if (ok)
        ok = fsync(fd) == 0;
    if (ok)
        ok = fchmod(fd, 0600) == 0;
    close(fd);
    if (ok)
        ok = rename(temporary.c_str(), path.c_str()) == 0;
    if (ok)
        (void)sync_directory(path.parent_path());
    if (!ok)
        (void)unlink(temporary.c_str());
    return ok;
}

bool copy_regular_file(const fs::path& source, const fs::path& destination,
                       uint64_t max_size = kMaxCopiedFileSize, uint64_t* copied_size = nullptr) {
    const int source_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source_fd < 0)
        return false;

    struct stat status{};
    if (fstat(source_fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<uint64_t>(status.st_size) > max_size) {
        close(source_fd);
        return false;
    }

    const fs::path temporary = destination.string() + ".tmp." + std::to_string(getpid());
    (void)unlink(temporary.c_str());
    const int destination_fd =
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (destination_fd < 0) {
        close(source_fd);
        return false;
    }

    bool ok = true;
    size_t total = 0;
    std::array<char, 16384> buffer{};
    for (;;) {
        const ssize_t count = read(source_fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        total += static_cast<size_t>(count);
        if (total > max_size ||
            !write_all(destination_fd, buffer.data(), static_cast<size_t>(count))) {
            ok = false;
            break;
        }
    }
    close(source_fd);
    if (ok)
        ok = fsync(destination_fd) == 0;
    if (ok)
        ok = fchmod(destination_fd, 0600) == 0;
    close(destination_fd);
    if (ok)
        ok = rename(temporary.c_str(), destination.c_str()) == 0;
    if (ok) {
        (void)sync_directory(destination.parent_path());
        if (copied_size != nullptr)
            *copied_size = total;
    }
    if (!ok)
        (void)unlink(temporary.c_str());
    return ok;
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch >= 0x20)
                escaped.push_back(static_cast<char>(ch));
            break;
        }
    }
    return escaped;
}

std::string current_boot_id() {
    const auto value = read_root_regular_file_no_follow(kBootIdPath, 128);
    return value ? trim(*value) : std::string();
}

std::optional<std::string> sha256_file(const fs::path& path, uint64_t max_size,
                                       uint64_t* hashed_size) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return std::nullopt;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<uint64_t>(status.st_size) > max_size) {
        close(fd);
        return std::nullopt;
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    uint64_t total = 0;
    std::array<unsigned char, 16384> buffer{};
    while (ok) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        total += static_cast<uint64_t>(count);
        if (total > max_size ||
            mbedtls_sha256_update(&context, buffer.data(), static_cast<size_t>(count)) != 0)
            ok = false;
    }
    close(fd);

    std::array<unsigned char, 32> digest{};
    if (ok)
        ok = mbedtls_sha256_finish(&context, digest.data()) == 0;
    mbedtls_sha256_free(&context);
    if (hashed_size != nullptr)
        *hashed_size = std::min(total, max_size);
    if (!ok)
        return std::nullopt;

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[(i * 2) + 1] = hex[digest[i] & 0xf];
    }
    return result;
}

ScanResult scan_regular_files(const fs::path& directory, bool hash_contents = false) {
    ScanResult result;
    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr)
        return result;
    result.available = true;

    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat status{};
        if (lstat((directory / name).c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            continue;
        FileStamp stamp{static_cast<uint64_t>(status.st_dev),
                        static_cast<uint64_t>(status.st_ino),
                        static_cast<uint64_t>(status.st_size),
                        static_cast<int64_t>(status.st_mtim.tv_sec),
                        static_cast<int64_t>(status.st_mtim.tv_nsec),
                        {}};
        result.files.emplace(name, std::move(stamp));
    }
    closedir(handle);

    if (hash_contents) {
        std::vector<std::string> names;
        names.reserve(result.files.size());
        for (const auto& [name, stamp] : result.files) {
            (void)stamp;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end(), [&result](const auto& left, const auto& right) {
            const auto& left_stamp = result.files.at(left);
            const auto& right_stamp = result.files.at(right);
            if (left_stamp.mtime_sec != right_stamp.mtime_sec)
                return left_stamp.mtime_sec > right_stamp.mtime_sec;
            if (left_stamp.mtime_nsec != right_stamp.mtime_nsec)
                return left_stamp.mtime_nsec > right_stamp.mtime_nsec;
            return left < right;
        });

        uint64_t hashed_bytes = 0;
        size_t hashed_files = 0;
        for (const auto& name : names) {
            auto& stamp = result.files.at(name);
            if (hashed_files >= kMaxPstoreFiles || hashed_bytes >= kMaxPstoreBytes ||
                stamp.size > kMaxPstoreBytes - hashed_bytes) {
                stamp.digest = kUnhashedDigest;
                continue;
            }
            uint64_t file_size = 0;
            const auto digest =
                sha256_file(directory / name, kMaxPstoreBytes - hashed_bytes, &file_size);
            stamp.digest = digest ? *digest : kInvalidDigest;
            ++hashed_files;
            hashed_bytes += file_size;
        }
    }
    return result;
}

bool consume_int64(std::string_view* rest, int64_t* value) {
    const std::string_view token = next_token(rest);
    if (token.empty())
        return false;
    const auto [end, error] =
        std::from_chars(token.data(), token.data() + token.size(), *value, 10);
    return error == std::errc{} && end == token.data() + token.size();
}

// Compatible with std::quoted's default format: double-quoted, with backslash
// escaping the next byte. `rest` is advanced past the closing quote.
bool consume_quoted(std::string_view* rest, std::string* value) {
    *rest = trim_view(*rest);
    if (rest->empty() || rest->front() != '"')
        return false;
    value->clear();
    bool escaped = false;
    for (size_t index = 1; index < rest->size(); ++index) {
        const char ch = (*rest)[index];
        if (escaped) {
            value->push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            rest->remove_prefix(index + 1);
            return true;
        } else {
            value->push_back(ch);
        }
    }
    return false;
}

void append_quoted(std::string* out, std::string_view value) {
    out->push_back('"');
    for (const char ch : value) {
        if (ch == '"' || ch == '\\')
            out->push_back('\\');
        out->push_back(ch);
    }
    out->push_back('"');
}

std::optional<Inventory> read_inventory(const fs::path& path) {
    const auto content = read_root_regular_file_no_follow(path, kMaxInventorySize);
    if (!content)
        return std::nullopt;

    Inventory inventory;
    bool valid = true;
    for_each_line(*content, [&](std::string_view line) {
        if (line.empty())
            return true;
        std::string_view rest = line;
        FileStamp stamp;
        std::string name;
        if (!parse_uint64(next_token(&rest), &stamp.device) ||
            !parse_uint64(next_token(&rest), &stamp.inode) ||
            !parse_uint64(next_token(&rest), &stamp.size) ||
            !consume_int64(&rest, &stamp.mtime_sec) || !consume_int64(&rest, &stamp.mtime_nsec) ||
            !consume_quoted(&rest, &name)) {
            valid = false;
            return false;
        }
        rest = trim_view(rest);
        if (!rest.empty() && !consume_quoted(&rest, &stamp.digest)) {
            valid = false;
            return false;
        }
        if (!trim_view(rest).empty()) {
            valid = false;
            return false;
        }
        inventory[name] = std::move(stamp);
        return true;
    });
    return valid ? std::optional<Inventory>(std::move(inventory)) : std::nullopt;
}

bool write_inventory(const fs::path& path, const ScanResult& scan) {
    std::string output;
    for (const auto& [name, stamp] : scan.files) {
        append_uint(&output, stamp.device);
        output += ' ';
        append_uint(&output, stamp.inode);
        output += ' ';
        append_uint(&output, stamp.size);
        output += ' ';
        append_int(&output, stamp.mtime_sec);
        output += ' ';
        append_int(&output, stamp.mtime_nsec);
        output += ' ';
        append_quoted(&output, name);
        output += ' ';
        append_quoted(&output, stamp.digest);
        output += '\n';
    }
    return write_atomic_file(path, output);
}

CaptureResult capture_changed_files(const fs::path& source, const fs::path& baseline_path,
                                    const fs::path& destination, bool hash_contents,
                                    size_t max_files, uint64_t max_bytes) {
    const ScanResult scan = scan_regular_files(source, hash_contents);
    CaptureResult result;
    result.available = scan.available;
    if (!scan.available)
        return result;

    const auto baseline = read_inventory(baseline_path);
    std::vector<std::pair<std::string, FileStamp>> files(scan.files.begin(), scan.files.end());
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        if (left.second.mtime_sec != right.second.mtime_sec)
            return left.second.mtime_sec > right.second.mtime_sec;
        if (left.second.mtime_nsec != right.second.mtime_nsec)
            return left.second.mtime_nsec > right.second.mtime_nsec;
        return left.first < right.first;
    });

    size_t selected_files = 0;
    for (const auto& [name, stamp] : files) {
        if (hash_contents && stamp.digest == kUnhashedDigest) {
            ++result.skipped;
            continue;
        }
        if (baseline) {
            const auto previous = baseline->find(name);
            if (previous != baseline->end() && same_file_stamp(previous->second, stamp))
                continue;
        }
        if (selected_files >= max_files || result.bytes >= max_bytes ||
            stamp.size > max_bytes - result.bytes) {
            ++result.skipped;
            continue;
        }
        ++selected_files;
        if (!ensure_secure_directory(destination)) {
            ++result.failed;
            continue;
        }
        uint64_t copied_size = 0;
        const uint64_t remaining = std::min<uint64_t>(kMaxCopiedFileSize, max_bytes - result.bytes);
        if (copy_regular_file(source / name, destination / name, remaining, &copied_size)) {
            ++result.copied;
            result.bytes += copied_size;
        } else {
            ++result.failed;
        }
    }
    return result;
}

bool read_early_snapshot(yz_early_native_snapshot_header* header) {
    const fs::path candidates[] = {
        fs::path(PREINIT_DIR_WATCHDOG) / "yukizygisk/native_snapshot.bin",
        fs::path(PREINIT_DIR_DEFAULT) / "yukizygisk/native_snapshot.bin",
    };
    for (const auto& path : candidates) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0)
            continue;
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != 0) {
            close(fd);
            continue;
        }
        yz_early_native_snapshot_header value{};
        const bool read_ok = read_all(fd, &value, sizeof(value));
        close(fd);
        const bool abi64_valid =
            !(value.flags & YZ_EARLY_NATIVE_FLAG_ABI64) ||
            (value.dlopen_offset != 0 && value.dlsym_offset != 0 && value.linker_size != 0);
        const bool abi32_valid =
            !(value.flags & YZ_EARLY_NATIVE_FLAG_ABI32) ||
            (value.dlopen32_offset != 0 && value.dlsym32_offset != 0 && value.linker32_size != 0);
        if (read_ok && value.magic == YZ_EARLY_NATIVE_MAGIC &&
            value.version == YZ_EARLY_NATIVE_VERSION && value.header_size == sizeof(value) &&
            value.entry_size == sizeof(yz_early_native_entry) &&
            value.count <= YZ_NATIVE_TARGET_MAX &&
            (value.flags & YZ_EARLY_NATIVE_FLAG_ENABLED) != 0 &&
            (!value.count ||
             (value.flags & (YZ_EARLY_NATIVE_FLAG_ABI32 | YZ_EARLY_NATIVE_FLAG_ABI64))) &&
            (value.flags & ~(YZ_EARLY_NATIVE_FLAG_ENABLED | YZ_EARLY_NATIVE_FLAG_ABI32 |
                             YZ_EARLY_NATIVE_FLAG_ABI64)) == 0 &&
            abi64_valid && abi32_valid) {
            *header = value;
            return true;
        }
    }
    return false;
}

bool write_early_linker(const fs::path& directory, const yz_early_native_snapshot_header& header) {
    std::string output = "{\n  \"version\": ";
    append_uint(&output, header.version);
    output += ",\n  \"flags\": ";
    append_uint(&output, header.flags);
    output += ",\n  \"module_count\": ";
    append_uint(&output, header.count);
    output += ",\n  \"arm64\": {\"dlopen\": \"";
    append_hex(&output, header.dlopen_offset);
    output += "\", \"dlsym\": \"";
    append_hex(&output, header.dlsym_offset);
    output += "\", \"linker_size\": \"";
    append_hex(&output, header.linker_size);
    output += "\"},\n  \"arm\": {\"dlopen\": \"";
    append_hex(&output, header.dlopen32_offset);
    output += "\", \"dlsym\": \"";
    append_hex(&output, header.dlsym32_offset);
    output += "\", \"linker_size\": \"";
    append_hex(&output, header.linker32_size);
    output += "\"}\n}\n";
    return write_atomic_file(directory / kEarlyLinkerName, output);
}

bool mark_evidence(const fs::path& directory, const char* reason) {
    return write_atomic_file(directory / kEvidenceName, std::string(reason) + "\n");
}

bool has_evidence(const fs::path& directory) {
    if (is_root_regular_file(directory / kEvidenceName))
        return true;
    const fs::path logs = directory / kLogsName;
    const ScanResult scan = scan_regular_files(logs);
    for (const auto& [name, stamp] : scan.files) {
        (void)name;
        if (stamp.size > 0)
            return true;
    }
    return is_root_regular_file(directory / kEarlyLinkerName, true) ||
           is_root_regular_file(directory / "linker64.json", true) ||
           is_root_regular_file(directory / "linker32.json", true);
}

bool import_legacy_logs(const fs::path& current, std::vector<fs::path>* imported_paths) {
    const struct LegacyLog {
        const char* source;
        const char* destination;
    } logs[] = {{"zygiskd64.log", "legacy-zygiskd64.log"},
                {"zygiskd64.old.log", "legacy-zygiskd64.old.log"},
                {"zygiskd32.log", "legacy-zygiskd32.log"},
                {"zygiskd32.old.log", "legacy-zygiskd32.old.log"}};
    bool imported = false;
    for (const auto& log : logs) {
        const fs::path source = fs::path(YUKIZYGISK_LEGACY_LOG_DIR) / log.source;
        if (!is_root_regular_file(source, true))
            continue;
        const fs::path destination = current / kLogsName;
        if (ensure_secure_directory(destination) &&
            copy_regular_file(source, destination / log.destination)) {
            imported = true;
            if (imported_paths != nullptr)
                imported_paths->push_back(source);
        }
    }
    if (imported)
        (void)mark_evidence(current, "legacy-log");
    return imported;
}

bool remove_generation(const fs::path& path) {
    struct stat status{};
    if (lstat(path.c_str(), &status) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(status.st_mode) || status.st_uid != 0)
        return false;
    std::error_code error;
    fs::remove_all(path, error);
    if (!error)
        (void)sync_directory(path.parent_path());
    return !error;
}

void copy_current_config(const fs::path& current) {
    if (is_root_regular_file(YUKIZYGISK_CONFIG_PATH))
        (void)copy_regular_file(YUKIZYGISK_CONFIG_PATH, current / kConfigName);
}

void capture_previous_boot(const fs::path& current) {
    copy_current_config(current);

    yz_early_native_snapshot_header header{};
    if (read_early_snapshot(&header) && write_early_linker(current, header))
        (void)mark_evidence(current, "early-linker");

    if (!has_evidence(current))
        return;

    const CaptureResult tombstones = capture_changed_files(
        kTombstonesPath, current / kTombstonesBaselineName, current / kTombstonesName, false,
        kMaxTombstoneFiles, kMaxTombstoneBytes);
    const CaptureResult pstore =
        capture_changed_files(kPstorePath, current / kPstoreBaselineName, current / kPstoreName,
                              true, kMaxPstoreFiles, kMaxPstoreBytes);
    if (tombstones.copied > 0 || pstore.copied > 0)
        (void)mark_evidence(current, "previous-boot-crash-data");

    std::string output = "{\n  \"captured_at_unix\": ";
    append_int(&output, static_cast<int64_t>(time(nullptr)));
    output += ",\n  \"tombstones\": {\"available\": ";
    output += tombstones.available ? "true" : "false";
    output += ", \"copied\": ";
    append_uint(&output, tombstones.copied);
    output += ", \"failed\": ";
    append_uint(&output, tombstones.failed);
    output += ", \"skipped\": ";
    append_uint(&output, tombstones.skipped);
    output += ", \"bytes\": ";
    append_uint(&output, tombstones.bytes);
    output += "},\n  \"pstore\": {\"available\": ";
    output += pstore.available ? "true" : "false";
    output += ", \"copied\": ";
    append_uint(&output, pstore.copied);
    output += ", \"failed\": ";
    append_uint(&output, pstore.failed);
    output += ", \"skipped\": ";
    append_uint(&output, pstore.skipped);
    output += ", \"bytes\": ";
    append_uint(&output, pstore.bytes);
    output += "}\n}\n";
    (void)write_atomic_file(current / kCaptureStateName, output);
}

bool initialize_current_generation(const fs::path& current, const std::string& boot_id,
                                   PreparationMode mode) {
    if (!ensure_secure_directory(current) || !ensure_secure_directory(current / kLogsName))
        return false;
    std::string state = "{\n  \"boot_id\": \"";
    state += json_escape(boot_id);
    state += "\",\n  \"updated_at_unix\": ";
    append_int(&state, static_cast<int64_t>(time(nullptr)));
    state += ",\n  \"phase\": \"early\",\n  \"safe_mode\": null,\n"
             "  \"feature_supported\": null,\n  \"feature_enabled\": null\n}\n";
    if (!write_atomic_file(current / kBootStateName, state))
        return false;

    if (mode == PreparationMode::Boot) {
        (void)write_inventory(current / kTombstonesBaselineName,
                              scan_regular_files(kTombstonesPath));
        (void)write_inventory(current / kPstoreBaselineName, scan_regular_files(kPstorePath, true));
        copy_current_config(current);
    }
    return write_atomic_file(current / kBootIdName, boot_id + "\n");
}

bool create_current_generation(const fs::path& diagnostics, const fs::path& current,
                               const std::string& boot_id, PreparationMode mode) {
    const fs::path staging = diagnostics / kCurrentStagingName;
    if (!remove_generation(staging) || !initialize_current_generation(staging, boot_id, mode))
        return false;
    if (rename(staging.c_str(), current.c_str()) == 0) {
        (void)sync_directory(diagnostics);
        return true;
    }
    (void)remove_generation(staging);
    return false;
}

bool import_legacy_generation(const fs::path& diagnostics, const fs::path& old) {
    const fs::path staging = diagnostics / kLegacyStagingName;
    if (!remove_generation(staging) || !ensure_secure_directory(staging) ||
        !ensure_secure_directory(staging / kLogsName))
        return false;

    std::vector<fs::path> imported_paths;
    bool evidence = import_legacy_logs(staging, &imported_paths);
    yz_early_native_snapshot_header header{};
    if (read_early_snapshot(&header) && write_early_linker(staging, header)) {
        (void)mark_evidence(staging, "early-linker");
        evidence = true;
    }
    if (!evidence) {
        (void)remove_generation(staging);
        return true;
    }

    capture_previous_boot(staging);
    if (rename(staging.c_str(), old.c_str()) == 0) {
        (void)sync_directory(diagnostics);
        for (const auto& path : imported_paths) {
            if (is_root_regular_file(path))
                (void)unlink(path.c_str());
        }
        if (!imported_paths.empty())
            (void)sync_directory(YUKIZYGISK_LEGACY_LOG_DIR);
        return true;
    }
    (void)remove_generation(staging);
    return false;
}

int lock_diagnostics(const fs::path& diagnostics, int operation) {
    const fs::path path = diagnostics / kLockName;
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != 0 ||
        fchmod(fd, 0600) != 0 || flock(fd, operation) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

std::optional<std::string> matching_current_boot_id() {
    const fs::path current = YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR;
    if (!is_root_directory(current))
        return std::nullopt;
    std::string boot_id = current_boot_id();
    const auto stored_boot_id = read_root_regular_file_no_follow(current / kBootIdName, 128);
    if (boot_id.empty() || !stored_boot_id || trim(*stored_boot_id) != boot_id)
        return std::nullopt;
    return boot_id;
}

bool prepare_diagnostics(bool create_if_missing, PreparationMode mode) {
    const std::string boot_id = current_boot_id();
    if (boot_id.empty()) {
        LOGW("YukiZygisk diagnostics: boot ID unavailable");
        return false;
    }

    const fs::path state = YUKIZYGISK_STATE_DIR;
    struct stat state_status{};
    if (lstat(state.c_str(), &state_status) != 0) {
        if (errno != ENOENT)
            return false;
        yz_early_native_snapshot_header header{};
        if (!create_if_missing && !read_early_snapshot(&header))
            return false;
        if (!ensure_secure_directory(state))
            return false;
    } else if (!S_ISDIR(state_status.st_mode) || state_status.st_uid != 0 ||
               chmod(state.c_str(), 0700) != 0) {
        LOGW("YukiZygisk diagnostics: refusing unsafe state directory");
        return false;
    }

    const fs::path diagnostics = YUKIZYGISK_DIAGNOSTICS_DIR;
    if (!ensure_secure_directory(diagnostics)) {
        LOGW("YukiZygisk diagnostics: failed to prepare diagnostics directory");
        return false;
    }

    const bool recovery = mode == PreparationMode::Recovery;
    const int lock_fd = lock_diagnostics(diagnostics, recovery ? LOCK_EX | LOCK_NB : LOCK_EX);
    if (lock_fd < 0) {
        LOGW("YukiZygisk diagnostics: failed to lock diagnostics directory");
        return false;
    }
    const auto finish = [lock_fd](bool result) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return result;
    };

    const fs::path current = YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR;
    const fs::path old = YUKIZYGISK_OLD_DIAGNOSTICS_DIR;
    if (!remove_generation(diagnostics / kCurrentStagingName) ||
        !remove_generation(diagnostics / kLegacyStagingName))
        return finish(false);

    struct stat current_status{};
    const int current_result = lstat(current.c_str(), &current_status);
    if (current_result != 0 && errno != ENOENT)
        return finish(false);
    const bool have_current = current_result == 0;
    if (have_current && (!S_ISDIR(current_status.st_mode) || current_status.st_uid != 0)) {
        LOGW("YukiZygisk diagnostics: refusing unsafe current generation");
        return finish(false);
    }
    if (have_current && !ensure_secure_directory(current))
        return finish(false);

    if (have_current) {
        const auto stored_boot_id = read_root_regular_file_no_follow(current / kBootIdName, 128);
        if (stored_boot_id && trim(*stored_boot_id) == boot_id)
            return finish(ensure_secure_directory(current / kLogsName));
    }

    if (!have_current) {
        struct stat old_status{};
        const int old_result = lstat(old.c_str(), &old_status);
        if (old_result != 0 && errno != ENOENT)
            return finish(false);
        if (old_result == 0 && (!S_ISDIR(old_status.st_mode) || old_status.st_uid != 0)) {
            LOGW("YukiZygisk diagnostics: refusing unsafe old generation");
            return finish(false);
        }
        if (!recovery && old_result != 0 && !import_legacy_generation(diagnostics, old)) {
            LOGW("YukiZygisk diagnostics: failed to import legacy evidence");
            return finish(false);
        }
        if (!create_current_generation(diagnostics, current, boot_id, mode)) {
            LOGW("YukiZygisk diagnostics: failed to initialize current generation");
            return finish(false);
        }
        LOGI("YukiZygisk diagnostics: boot generation initialized");
        return finish(true);
    }

    if (!recovery)
        capture_previous_boot(current);
    if (!remove_generation(old)) {
        LOGW("YukiZygisk diagnostics: failed to remove old generation");
        return finish(false);
    }
    if (rename(current.c_str(), old.c_str()) != 0) {
        LOGW("YukiZygisk diagnostics: failed to rotate current generation: %s", strerror(errno));
        return finish(false);
    }
    (void)sync_directory(diagnostics);

    if (!create_current_generation(diagnostics, current, boot_id, mode)) {
        LOGW("YukiZygisk diagnostics: failed to initialize current generation");
        return finish(false);
    }
    LOGI("YukiZygisk diagnostics: boot generation rotated");
    return finish(true);
}

}  // namespace

bool prepare_yukizygisk_diagnostics(bool create_if_missing) {
    return prepare_diagnostics(create_if_missing, PreparationMode::Boot);
}

bool prepare_yukizygisk_recovery_diagnostics() {
    return matching_current_boot_id().has_value() ||
           prepare_diagnostics(true, PreparationMode::Recovery);
}

void update_yukizygisk_boot_diagnostics(bool safe_mode, bool feature_supported,
                                        bool feature_enabled, const char* phase) {
    const fs::path current = YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR;
    const auto boot_id = matching_current_boot_id();
    if (!boot_id)
        return;

    std::string state = "{\n  \"boot_id\": \"";
    state += json_escape(*boot_id);
    state += "\",\n  \"updated_at_unix\": ";
    append_int(&state, static_cast<int64_t>(time(nullptr)));
    state += ",\n  \"phase\": \"";
    state += json_escape(phase ? phase : "unknown");
    state += "\",\n  \"safe_mode\": ";
    state += safe_mode ? "true" : "false";
    state += ",\n  \"feature_supported\": ";
    state += feature_supported ? "true" : "false";
    state += ",\n  \"feature_enabled\": ";
    state += feature_enabled ? "true" : "false";
    state += '\n';
    state += "}\n";
    (void)write_atomic_file(current / kBootStateName, state);
    if (feature_supported && feature_enabled)
        (void)mark_evidence(current, "feature-enabled");
}

void record_yukizygisk_early_linker(const yz_early_native_snapshot_header& header) {
    const fs::path current = YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR;
    if (!matching_current_boot_id())
        return;
    if (write_early_linker(current, header))
        (void)mark_evidence(current, "early-linker");
}

}  // namespace ksud
