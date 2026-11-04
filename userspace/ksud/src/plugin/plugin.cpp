#include "plugin.hpp"
#include "../terminal.hpp"

#include "../core/restorecon.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "lua_engine.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../archive_zip.hpp"

namespace ksud {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxPluginIdLength = 64;
constexpr size_t kMaxCallbackLength = 64;
constexpr size_t kMaxConfigKeyLength = 64;
constexpr size_t kMaxManifestSize = size_t{1024} * 1024;
constexpr size_t kMaxConfigSize = size_t{4} * 1024 * 1024;
constexpr uint64_t kMaxArchiveEntries = 8192U;
constexpr size_t kMaxArchiveNameBytes = size_t{4} * 1024 * 1024;
constexpr size_t kMaxArchiveAllocation = size_t{16} * 1024 * 1024;
constexpr uint64_t kMaxArchiveFileSize = uint64_t{64} * 1024 * 1024;
constexpr uint64_t kMaxArchiveTotalSize = uint64_t{256} * 1024 * 1024;
constexpr uint64_t kMaxArchivePackageSize = kMaxArchiveTotalSize + (uint64_t{16} * 1024 * 1024);
constexpr off_t kMaxPluginLogSize = off_t{1024} * 1024;
constexpr size_t kMaxPluginLogLineSize = size_t{64} * 1024;

bool path_exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error) && !error;
}

bool directory_exists(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error) && !error;
}

bool regular_file_exists(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

void print_error(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    (void)terminal::verrorf(format, arguments);
    va_end(arguments);
}

void print_output(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    (void)std::vfprintf(stdout, format, arguments);
    va_end(arguments);
}

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0)
            close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

class ScopedTree {
public:
    explicit ScopedTree(fs::path path) : path_(std::move(path)) {}
    ~ScopedTree() {
        if (path_.empty())
            return;
        std::error_code error;
        fs::remove_all(path_, error);
    }

    ScopedTree(const ScopedTree&) = delete;
    ScopedTree& operator=(const ScopedTree&) = delete;
    ScopedTree(ScopedTree&&) = delete;
    ScopedTree& operator=(ScopedTree&&) = delete;

    void release() { path_.clear(); }

private:
    fs::path path_;
};

class FileLock {
public:
    explicit FileLock(const fs::path& path) {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            error_ = error.message();
            return;
        }
        fd_ = ScopedFd(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (!fd_.valid()) {
            error_ = strerror(errno);
            return;
        }
        if (flock(fd_.get(), LOCK_EX) != 0) {
            error_ = strerror(errno);
            return;
        }
        locked_ = true;
    }

    [[nodiscard]] bool locked() const { return locked_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    ScopedFd fd_;
    bool locked_ = false;
    std::string error_;
};

struct ArchiveEntry {
    uint64_t index = 0;
    std::string name;
    ZipEntry stat{};
};

struct InspectedArchive {
    std::vector<ArchiveEntry> entries;
    uint64_t manifest_index = 0;
    std::string root_prefix;
    std::string manifest;
};

struct RuntimeTreeLock {
    ScopedFd fd;
    bool busy = false;
    std::string error;
};

bool ascii_alnum(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool identifier_chars_are_valid(const std::string& value, size_t max_length) {
    if (value.empty() || value.size() > max_length || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return ascii_alnum(character) || character == '-' || character == '_' || character == '.';
    });
}

std::string plugin_directory(const std::string& id) {
    return std::string(PLUGIN_DIR) + id;
}

std::string value_as_string(const plugin_json::Value& value) {
    if (value.type == plugin_json::Type::String)
        return value.s;
    if (value.type == plugin_json::Type::Null)
        return {};
    return plugin_json::dump(value);
}

std::optional<plugin_json::Value> parse_json(const std::string& content, std::string* error) {
    return plugin_json::parse(content, error);
}

bool read_optional_string(const plugin_json::Object& object, const char* key, std::string* output,
                          std::string* error) {
    const auto iterator = object.find(key);
    if (iterator == object.end())
        return true;
    if (iterator->second.type != plugin_json::Type::String) {
        if (error)
            *error = std::string("Manifest field '") + key + "' must be a string";
        return false;
    }
    *output = iterator->second.s;
    return true;
}

bool validate_localized_strings(const plugin_json::Value& value, const char* field,
                                std::string* error) {
    if (value.type != plugin_json::Type::Object) {
        if (error)
            *error = std::string("Manifest field '") + field + "' must be an object";
        return false;
    }
    const bool valid = std::all_of(value.o.begin(), value.o.end(), [](const auto& entry) {
        return !entry.first.empty() && entry.second.type == plugin_json::Type::String;
    });
    if (!valid && error)
        *error = std::string("Manifest field '") + field + "' has an invalid entry";
    return valid;
}

bool validate_config_fields(const plugin_json::Value& config, std::string* error) {
    if (config.type != plugin_json::Type::Array) {
        if (error)
            *error = "Manifest field 'config' must be an array";
        return false;
    }

    std::set<std::string> keys;
    for (const auto& field : config.a) {
        if (field.type != plugin_json::Type::Object) {
            if (error)
                *error = "Each config field must be an object";
            return false;
        }
        const auto key = field.o.find("key");
        if (key == field.o.end() || key->second.type != plugin_json::Type::String ||
            !plugin_config_key_is_valid(key->second.s) || !keys.insert(key->second.s).second) {
            if (error)
                *error = "Config fields must have unique, valid keys";
            return false;
        }

        const auto type = field.o.find("type");
        if (type != field.o.end()) {
            if (type->second.type != plugin_json::Type::String ||
                (type->second.s != "text" && type->second.s != "number" &&
                 type->second.s != "bool" && type->second.s != "select")) {
                if (error)
                    *error = "Config field has an unsupported type";
                return false;
            }
        }

        const auto label = field.o.find("label");
        if (label != field.o.end() && label->second.type != plugin_json::Type::String) {
            if (error)
                *error = "Config field label must be a string";
            return false;
        }
        const auto labels = field.o.find("labels");
        if (labels != field.o.end() &&
            !validate_localized_strings(labels->second, "config.labels", error)) {
            return false;
        }
        const auto options = field.o.find("options");
        if (options != field.o.end()) {
            if (options->second.type != plugin_json::Type::Array ||
                !std::all_of(options->second.a.begin(), options->second.a.end(),
                             [](const plugin_json::Value& option) {
                                 return option.type == plugin_json::Type::String;
                             })) {
                if (error)
                    *error = "Config field options must be an array of strings";
                return false;
            }
        }
    }
    return true;
}

std::optional<std::string> normalize_archive_path(std::string name) {
    if (name.empty())
        return std::nullopt;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.front() == '/' || (name.size() >= 2 && name[1] == ':'))
        return std::nullopt;

    const bool directory = name.back() == '/';
    if (directory)
        name.pop_back();

    std::string normalized;
    size_t offset = 0;
    while (offset <= name.size()) {
        const size_t separator = name.find('/', offset);
        const size_t end = separator == std::string::npos ? name.size() : separator;
        const std::string_view component(name.data() + offset, end - offset);
        if (component.empty() || component == "..")
            return std::nullopt;
        if (component != ".") {
            if (!normalized.empty())
                normalized.push_back('/');
            normalized.append(component);
        }
        if (separator == std::string::npos)
            break;
        offset = separator + 1;
    }
    if (normalized.empty())
        return std::nullopt;
    if (directory)
        normalized.push_back('/');
    return normalized;
}

std::optional<std::string> extract_archive_text(ZipReader* archive, uint64_t index, size_t max_size,
                                                std::string* error) {
    ZipEntry status{};
    if (!archive->entry(index, status) || status.directory || status.size == 0 ||
        status.size > max_size) {
        if (error)
            *error = "Manifest has an invalid size";
        return std::nullopt;
    }
    auto content = archive->read_text(index, max_size);
    if (!content && error != nullptr)
        *error = archive->error();
    return content;
}

std::optional<InspectedArchive> inspect_archive(ZipReader* reader, std::string* error) {
    auto* archive = reader;
    const uint64_t count = archive->entry_count();
    if (count == 0 || count > kMaxArchiveEntries) {
        *error = "Archive has an invalid number of entries";
        return std::nullopt;
    }

    InspectedArchive inspected;
    std::set<std::string> names;
    std::optional<uint64_t> root_manifest;
    std::vector<std::pair<uint64_t, std::string>> wrapper_manifests;
    uint64_t total_size = 0;
    size_t total_name_size = 0;
    for (uint64_t index = 0; index < count; ++index) {
        ZipEntry status{};
        if (!archive->entry(index, status)) {
            *error = "Cannot read ZIP central directory";
            return std::nullopt;
        }
        const auto raw_name = std::optional<std::string>(status.name);
        if (raw_name && raw_name->size() > kMaxArchiveNameBytes - total_name_size) {
            *error = "Archive filenames exceed the size limit";
            return std::nullopt;
        }
        if (raw_name)
            total_name_size += raw_name->size();
        const auto name = raw_name ? normalize_archive_path(*raw_name) : std::nullopt;
        if (!name || !names.insert(*name).second) {
            *error = "Archive contains an unsafe or duplicate path";
            return std::nullopt;
        }
        if (status.encrypted || !status.supported || status.symlink) {
            *error = "Archive contains an encrypted, unsupported, or symbolic-link entry";
            return std::nullopt;
        }
        if (!status.directory) {
            if (status.size > kMaxArchiveFileSize ||
                total_size > kMaxArchiveTotalSize - status.size) {
                *error = "Archive exceeds the extraction size limit";
                return std::nullopt;
            }
            total_size += status.size;
        }

        inspected.entries.push_back({index, *name, status});
        const std::string_view path = *name;
        if (path == PLUGIN_MANIFEST) {
            root_manifest = index;
        } else {
            const size_t slash = path.find('/');
            if (slash != std::string_view::npos &&
                path.find('/', slash + 1) == std::string_view::npos &&
                path.substr(slash + 1) == PLUGIN_MANIFEST) {
                wrapper_manifests.emplace_back(index, std::string(path.substr(0, slash + 1)));
            }
        }
    }
    if (root_manifest) {
        inspected.manifest_index = *root_manifest;
        inspected.root_prefix.clear();
    } else if (wrapper_manifests.size() == 1) {
        inspected.manifest_index = wrapper_manifests.front().first;
        inspected.root_prefix = wrapper_manifests.front().second;
    } else {
        *error = "Archive must contain exactly one root or single-wrapper plugin.json";
        return std::nullopt;
    }

    auto manifest =
        extract_archive_text(archive, inspected.manifest_index, kMaxManifestSize, error);
    if (!manifest)
        return std::nullopt;
    inspected.manifest = std::move(*manifest);
    return inspected;
}

bool write_all(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool atomic_write(const fs::path& path, const std::string& content, std::string* error) {
    std::string temporary_template = path.string() + ".tmp.XXXXXX";
    std::vector<char> temporary_buffer(temporary_template.begin(), temporary_template.end());
    temporary_buffer.push_back('\0');
    ScopedFd fd(mkstemp(temporary_buffer.data()));
    const fs::path temporary(temporary_buffer.data());
    if (!fd.valid()) {
        *error = strerror(errno);
        return false;
    }
    (void)fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    if (!write_all(fd.get(), content.data(), content.size()) || fsync(fd.get()) != 0) {
        *error = strerror(errno);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    if (close(fd.release()) != 0) {
        *error = strerror(errno);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        *error = strerror(errno);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    const ScopedFd directory(open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory.valid() || fsync(directory.get()) != 0) {
        *error = strerror(errno);
        return false;
    }
    return true;
}

std::optional<plugin_json::Object> read_config_object(const fs::path& path, std::string* error) {
    std::error_code size_error;
    const auto size = fs::file_size(path, size_error);
    if (!size_error && size > kMaxConfigSize) {
        if (error)
            *error = "config.json exceeds the size limit";
        return std::nullopt;
    }
    const auto content = read_file(path);
    if (!content)
        return plugin_json::Object{};
    const auto value = parse_json(*content, error);
    if (!value || value->type != plugin_json::Type::Object) {
        if (error && error->empty())
            *error = "config.json is not a JSON object";
        return std::nullopt;
    }
    return value->o;
}

bool write_config_object(const fs::path& path, const plugin_json::Object& object,
                         std::string* error) {
    const std::string content = plugin_json::dump(plugin_json::Value(object), 2);
    if (content.size() > kMaxConfigSize) {
        *error = "config.json exceeds the size limit";
        return false;
    }
    return atomic_write(path, content, error);
}

fs::path state_lock_path(const std::string& id) {
    return fs::path(PLUGIN_LOCK_DIR) / (id + ".state.lock");
}

RuntimeTreeLock lock_runtime_tree(const fs::path& directory) {
    RuntimeTreeLock result;
    result.fd = ScopedFd(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!result.fd.valid()) {
        result.error = strerror(errno);
        return result;
    }
    if (flock(result.fd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            result.busy = true;
        else
            result.error = strerror(errno);
        result.fd = ScopedFd();
    }
    return result;
}

void cleanup_stale_plugin_trees() {
    std::error_code iterator_error;
    const fs::directory_iterator iterator(PLUGIN_STAGE_DIR, iterator_error);
    if (iterator_error)
        return;
    for (auto it = iterator; it != fs::directory_iterator() && !iterator_error;
         it.increment(iterator_error)) {
        const auto& entry = *it;
        const std::string filename = entry.path().filename().string();
        const size_t transaction = filename.rfind(".txn.");
        if (transaction == std::string::npos || transaction + 5 >= filename.size() ||
            !plugin_id_is_valid(filename.substr(0, transaction))) {
            continue;
        }
        std::error_code type_error;
        if (!entry.is_directory(type_error) || type_error)
            continue;
        auto runtime_lock = lock_runtime_tree(entry.path());
        if (runtime_lock.busy || !runtime_lock.error.empty())
            continue;
        std::error_code remove_error;
        fs::remove_all(entry.path(), remove_error);
        if (remove_error)
            LOGW("Cannot remove stale plugin tree %s: %s", entry.path().c_str(),
                 remove_error.message().c_str());
    }
}

std::optional<fs::path> create_stage_directory(const std::string& id, std::string* error) {
    std::string pattern = (fs::path(PLUGIN_STAGE_DIR) / (id + ".txn.XXXXXX")).string();
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    char* created = mkdtemp(path.data());
    if (!created) {
        *error = strerror(errno);
        return std::nullopt;
    }
    if (chmod(created, 0700) != 0) {
        *error = strerror(errno);
        std::error_code remove_error;
        fs::remove_all(created, remove_error);
        return std::nullopt;
    }
    return fs::path(created);
}

bool sync_directory(const fs::path& path, std::string* error) {
    const ScopedFd descriptor(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid() || fsync(descriptor.get()) != 0) {
        *error = "Cannot sync directory '" + path.string() + "': " + strerror(errno);
        return false;
    }
    return true;
}

bool sync_directory_tree(const fs::path& root, std::string* error) {
    std::vector<fs::path> directories;
    std::error_code iterator_error;
    for (fs::recursive_directory_iterator iterator(root, iterator_error), end;
         iterator != end && !iterator_error; iterator.increment(iterator_error)) {
        std::error_code type_error;
        if (iterator->is_directory(type_error) && !type_error) {
            directories.push_back(iterator->path());
        } else if (!type_error && iterator->is_regular_file(type_error) && !type_error) {
            const ScopedFd descriptor(
                open(iterator->path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            if (!descriptor.valid() || fsync(descriptor.get()) != 0) {
                *error = "Cannot sync file '" + iterator->path().string() + "': " + strerror(errno);
                return false;
            }
        }
    }
    if (iterator_error) {
        *error = "Cannot enumerate staged directories: " + iterator_error.message();
        return false;
    }
    std::reverse(directories.begin(), directories.end());
    for (const auto& directory : directories) {
        if (!sync_directory(directory, error))
            return false;
    }
    return sync_directory(root, error);
}

bool label_plugin_tree(const fs::path& root, std::string* error) {
    if (!lsetfilecon(root, SYSTEM_CON)) {
        *error = "Cannot label plugin directory";
        return false;
    }
    std::error_code iterator_error;
    for (fs::recursive_directory_iterator iterator(root, iterator_error), end;
         iterator != end && !iterator_error; iterator.increment(iterator_error)) {
        if (!lsetfilecon(iterator->path(), SYSTEM_CON)) {
            *error = "Cannot label '" + iterator->path().string() + "'";
            return false;
        }
    }
    if (iterator_error) {
        *error = "Cannot enumerate plugin labels: " + iterator_error.message();
        return false;
    }
    return true;
}

std::vector<std::string> enabled_plugin_dependents(const std::string& plugin_id) {
    const auto plugins = plugin_discover();
    std::set<std::string> affected{plugin_id};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& plugin : plugins) {
            if (!plugin.enabled || !plugin.manifest_valid || affected.count(plugin.id) != 0)
                continue;
            if (std::any_of(plugin.manifest.depends.begin(), plugin.manifest.depends.end(),
                            [&](const std::string& dependency) {
                                return affected.count(dependency) != 0;
                            })) {
                affected.insert(plugin.id);
                changed = true;
            }
        }
    }
    affected.erase(plugin_id);
    return {affected.begin(), affected.end()};
}

std::string join_plugin_ids(const std::vector<std::string>& ids) {
    std::string output;
    for (const auto& id : ids) {
        if (!output.empty())
            output += ", ";
        output += id;
    }
    return output;
}

bool preserve_state_file(const fs::path& old_directory, const fs::path& new_directory,
                         const char* filename, std::string* error) {
    const fs::path source = old_directory / filename;
    if (!path_exists(source))
        return true;
    if (!copy_file_data(source, new_directory / filename)) {
        *error = "Cannot preserve " + std::string(filename) + ": " + strerror(errno);
        return false;
    }
    const ScopedFd copied(open((new_directory / filename).c_str(), O_RDONLY | O_CLOEXEC));
    if (!copied.valid() || fsync(copied.get()) != 0) {
        *error = "Cannot sync " + std::string(filename) + ": " + strerror(errno);
        return false;
    }
    return true;
}

bool extract_plugin(ZipReader* reader, const InspectedArchive& inspected,
                    const PluginManifest& manifest, const fs::path& stage, std::string* error) {
    const std::string expected_entry = inspected.root_prefix + manifest.entry;
    bool entry_found = false;
    for (const auto& entry : inspected.entries) {
        if (entry.name == expected_entry && !entry.stat.directory)
            entry_found = true;
    }
    if (!entry_found) {
        *error = "Entry file '" + manifest.entry + "' was not found beside plugin.json";
        return false;
    }

    auto* archive = reader;
    for (const auto& entry : inspected.entries) {
        if (!inspected.root_prefix.empty() &&
            entry.name.compare(0, inspected.root_prefix.size(), inspected.root_prefix) != 0) {
            continue;
        }
        std::string relative = inspected.root_prefix.empty()
                                   ? entry.name
                                   : entry.name.substr(inspected.root_prefix.size());
        if (relative.empty())
            continue;
        if (relative.back() == '/')
            relative.pop_back();
        if (relative.empty())
            continue;
        if (relative == PLUGIN_CONFIG_FILE || relative == PLUGIN_OUTPUT_LOG ||
            relative == DISABLE_FILE_NAME) {
            continue;
        }

        const fs::path output = stage / fs::path(relative);
        std::error_code directory_error;
        if (entry.stat.directory) {
            fs::create_directories(output, directory_error);
            if (!directory_error)
                chmod(output.c_str(), 0755);
        } else {
            fs::create_directories(output.parent_path(), directory_error);
            ScopedFd output_fd;
            if (!directory_error) {
                output_fd =
                    ScopedFd(open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
            }
            if (!directory_error && !output_fd.valid()) {
                *error = std::string("Cannot create '") + relative + "': " + strerror(errno);
                return false;
            }
            if (!directory_error &&
                !archive->extract_to_fd(entry.index, output_fd.get(), kMaxArchiveFileSize)) {
                *error = "Cannot extract '" + relative + "': " + archive->error();
                return false;
            }
            if (!directory_error) {
                struct stat extracted_status{};
                if (fstat(output_fd.get(), &extracted_status) != 0 ||
                    static_cast<uint64_t>(extracted_status.st_size) != entry.stat.size) {
                    *error = std::string("Extracted '") + relative + "' has an invalid size";
                    return false;
                }
                mode_t mode = static_cast<mode_t>(entry.stat.attributes >> 16U) & 0777;
                if (mode == 0)
                    mode = 0644;
                if (fchmod(output_fd.get(), mode) != 0 || fsync(output_fd.get()) != 0) {
                    *error = std::string("Cannot finalize '") + relative + "': " + strerror(errno);
                    return false;
                }
            }
        }
        if (directory_error) {
            *error = "Cannot create extraction path: " + directory_error.message();
            return false;
        }
    }
    return true;
}

int plugin_install(const std::string& zip_path) {
    std::error_code canonical_error;
    const fs::path canonical = fs::canonical(zip_path, canonical_error);
    if (canonical_error) {
        print_error("Plugin package is unavailable: %s\n", canonical_error.message().c_str());
        return 1;
    }

    const FileLock install_lock(fs::path(PLUGIN_LOCK_DIR) / "install.lock");
    if (!install_lock.locked()) {
        print_error("Cannot lock plugin installer: %s\n", install_lock.error().c_str());
        return 1;
    }

    ZipReader reader(canonical,
                     {kMaxArchivePackageSize, kMaxArchiveEntries, kMaxArchiveAllocation});
    if (!reader.valid()) {
        print_error("%s\n", reader.error().c_str());
        return 1;
    }
    std::string error;
    const auto inspected = inspect_archive(&reader, &error);
    if (!inspected) {
        print_error("%s\n", error.c_str());
        return 1;
    }

    const auto parsed = parse_json(inspected->manifest, &error);
    if (!parsed || parsed->type != plugin_json::Type::Object) {
        print_error("Invalid plugin.json: %s\n", error.c_str());
        return 1;
    }

    // Parse through the same validator used at runtime by staging the manifest in memory below.
    const auto id_value = parsed->o.find("id");
    if (id_value == parsed->o.end() || id_value->second.type != plugin_json::Type::String ||
        !plugin_id_is_valid(id_value->second.s)) {
        print_error("plugin.json has an invalid id\n");
        return 1;
    }
    const std::string id = id_value->second.s;

    std::error_code stage_root_error;
    fs::create_directories(PLUGIN_STAGE_DIR, stage_root_error);
    if (stage_root_error) {
        print_error("Cannot create plugin stage: %s\n", stage_root_error.message().c_str());
        return 1;
    }
    cleanup_stale_plugin_trees();
    const auto created_stage = create_stage_directory(id, &error);
    if (!created_stage) {
        print_error("Cannot create plugin stage: %s\n", error.c_str());
        return 1;
    }
    const fs::path& stage = *created_stage;
    ScopedTree stage_cleanup(stage);

    // Write only the manifest first so the common schema validator can be reused.
    if (!write_file(stage / PLUGIN_MANIFEST, inspected->manifest)) {
        print_error("Cannot stage plugin.json\n");
        return 1;
    }
    auto manifest = plugin_read_manifest(stage.string(), id, &error);
    if (!manifest) {
        print_error("Invalid plugin.json: %s\n", error.c_str());
        return 1;
    }
    if (!plugin_dependencies_available(*manifest, &error)) {
        print_error("%s\n", error.c_str());
        return 1;
    }
    if (!extract_plugin(&reader, *inspected, *manifest, stage, &error)) {
        print_error("%s\n", error.c_str());
        return 1;
    }

    auto verified = plugin_read_manifest(stage.string(), id, &error);
    if (!verified || !regular_file_exists(stage / verified->entry)) {
        print_error("Extracted plugin failed validation: %s\n", error.c_str());
        return 1;
    }

    const fs::path target = plugin_directory(id);
    const FileLock state_lock(state_lock_path(id));
    if (!state_lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", state_lock.error().c_str());
        return 1;
    }
    if (directory_exists(target)) {
        if (!preserve_state_file(target, stage, PLUGIN_CONFIG_FILE, &error) ||
            !preserve_state_file(target, stage, PLUGIN_OUTPUT_LOG, &error) ||
            !preserve_state_file(target, stage, DISABLE_FILE_NAME, &error)) {
            print_error("%s\n", error.c_str());
            return 1;
        }
    }
    if (chmod(stage.c_str(), 0755) != 0 || !label_plugin_tree(stage, &error) ||
        !sync_directory_tree(stage, &error)) {
        if (error.empty())
            error = strerror(errno);
        print_error("Cannot finalize staged plugin: %s\n", error.c_str());
        return 1;
    }

    std::error_code plugin_root_error;
    fs::create_directories(PLUGIN_DIR, plugin_root_error);
    if (plugin_root_error) {
        print_error("Cannot create plugin directory: %s\n", plugin_root_error.message().c_str());
        return 1;
    }

    const fs::file_status target_status = fs::symlink_status(target, plugin_root_error);
    if (plugin_root_error && plugin_root_error != std::errc::no_such_file_or_directory) {
        print_error("Cannot inspect installed plugin: %s\n", plugin_root_error.message().c_str());
        return 1;
    }
    plugin_root_error.clear();
    const bool replacing = fs::exists(target_status);
    if (replacing && !fs::is_directory(target_status)) {
        print_error("Installed plugin path is not a directory\n");
        return 1;
    }
    RuntimeTreeLock old_runtime_lock;
    if (replacing) {
        old_runtime_lock = lock_runtime_tree(target);
        if (!old_runtime_lock.error.empty()) {
            print_error("Cannot lock installed plugin: %s\n", old_runtime_lock.error.c_str());
            return 1;
        }
        if (old_runtime_lock.busy) {
            print_error("Plugin '%s' is busy; disable it or retry after its callback exits\n",
                        id.c_str());
            return 1;
        }
        if (!stop_plugin_daemons(id, &error)) {
            print_error("Cannot stop running plugin before update: %s\n", error.c_str());
            return 1;
        }
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
        if (syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, target.c_str(),
                    RENAME_EXCHANGE) != 0) {
            print_error("Cannot atomically replace plugin: %s\n", strerror(errno));
            return 1;
        }
#else
        print_error("Atomic plugin replacement is unsupported on this platform\n");
        return 1;
#endif
    } else {
        fs::rename(stage, target, plugin_root_error);
        if (plugin_root_error) {
            print_error("Cannot activate plugin: %s\n", plugin_root_error.message().c_str());
            return 1;
        }
    }
    const bool activation_synced =
        sync_directory(PLUGIN_DIR, &error) && sync_directory(PLUGIN_STAGE_DIR, &error);
    if (!activation_synced) {
        LOGW("Plugin '%s' was activated but its directory exchange could not be synced: %s",
             id.c_str(), error.c_str());
    }
    if (replacing) {
        if (activation_synced) {
            std::error_code remove_error;
            fs::remove_all(stage, remove_error);
            if (remove_error) {
                LOGW("Cannot remove retired plugin tree %s: %s", stage.c_str(),
                     remove_error.message().c_str());
            }
            std::string retire_sync_error;
            if (!sync_directory(PLUGIN_STAGE_DIR, &retire_sync_error)) {
                LOGW("Cannot sync retired plugin cleanup for '%s': %s", id.c_str(),
                     retire_sync_error.c_str());
            }
        } else {
            LOGW("Retaining the previous tree for plugin '%s' after a sync failure", id.c_str());
        }
    }
    stage_cleanup.release();
    if (!sync_directory(target, &error)) {
        LOGW("Plugin '%s' was activated but its directory could not be synced: %s", id.c_str(),
             error.c_str());
    }
    print_output("Plugin '%s' installed\n", id.c_str());
    return 0;
}

int plugin_uninstall(const std::string& id) {
    if (!plugin_id_is_valid(id)) {
        print_error("Invalid plugin id\n");
        return 1;
    }
    const fs::path directory = plugin_directory(id);
    if (!directory_exists(directory)) {
        print_error("Plugin '%s' is not installed\n", id.c_str());
        return 1;
    }
    const FileLock install_lock(fs::path(PLUGIN_LOCK_DIR) / "install.lock");
    if (!install_lock.locked()) {
        print_error("Cannot lock plugin installer: %s\n", install_lock.error().c_str());
        return 1;
    }
    const FileLock state_lock(state_lock_path(id));
    if (!state_lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", state_lock.error().c_str());
        return 1;
    }
    std::error_code stage_error;
    fs::create_directories(PLUGIN_STAGE_DIR, stage_error);
    if (stage_error) {
        print_error("Cannot create plugin stage: %s\n", stage_error.message().c_str());
        return 1;
    }
    cleanup_stale_plugin_trees();
    const auto dependents = enabled_plugin_dependents(id);
    if (!dependents.empty()) {
        print_error("Cannot uninstall plugin '%s'; enabled plugins depend on it: %s\n", id.c_str(),
                    join_plugin_ids(dependents).c_str());
        return 1;
    }
    const RuntimeTreeLock runtime_lock = lock_runtime_tree(directory);
    if (!runtime_lock.error.empty()) {
        print_error("Cannot lock plugin runtime: %s\n", runtime_lock.error.c_str());
        return 1;
    }
    if (runtime_lock.busy) {
        print_error("Plugin '%s' is busy; disable it or retry after its callback exits\n",
                    id.c_str());
        return 1;
    }
    std::string daemon_error;
    if (!stop_plugin_daemons(id, &daemon_error)) {
        print_error("Cannot stop running plugin: %s\n", daemon_error.c_str());
        return 1;
    }
    fs::remove_all(directory, stage_error);
    if (stage_error) {
        print_error("Cannot uninstall plugin: %s\n", stage_error.message().c_str());
        return 1;
    }
    std::string sync_error;
    if (!sync_directory(PLUGIN_DIR, &sync_error) ||
        !sync_directory(PLUGIN_STAGE_DIR, &sync_error)) {
        LOGW("Plugin '%s' was uninstalled but directory sync failed: %s", id.c_str(),
             sync_error.c_str());
    }
    print_output("Plugin '%s' uninstalled\n", id.c_str());
    return 0;
}

int plugin_set_enabled(const std::string& id, bool enabled) {
    if (!plugin_id_is_valid(id)) {
        print_error("Invalid plugin id\n");
        return 1;
    }
    const FileLock install_lock(fs::path(PLUGIN_LOCK_DIR) / "install.lock");
    if (!install_lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", install_lock.error().c_str());
        return 1;
    }
    const fs::path directory = plugin_directory(id);
    if (!directory_exists(directory)) {
        print_error("Plugin '%s' is not installed\n", id.c_str());
        return 1;
    }
    const FileLock state_lock(state_lock_path(id));
    if (!state_lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", state_lock.error().c_str());
        return 1;
    }
    if (!enabled) {
        const auto dependents = enabled_plugin_dependents(id);
        if (!dependents.empty()) {
            print_error("Cannot disable plugin '%s'; enabled plugins depend on it: %s\n",
                        id.c_str(), join_plugin_ids(dependents).c_str());
            return 1;
        }
    }
    if (enabled) {
        std::string error;
        const auto manifest = plugin_read_manifest(directory.string(), id, &error);
        if (!manifest) {
            print_error("Invalid plugin manifest: %s\n", error.c_str());
            return 1;
        }
        if (!plugin_dependencies_available(*manifest, &error)) {
            print_error("%s\n", error.c_str());
            return 1;
        }
    }

    const fs::path flag = directory / DISABLE_FILE_NAME;
    if (enabled) {
        if (unlink(flag.c_str()) != 0 && errno != ENOENT) {
            print_error("Cannot enable plugin: %s\n", strerror(errno));
            return 1;
        }
    } else {
        const ScopedFd fd(open(flag.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600));
        if (!fd.valid() || fsync(fd.get()) != 0) {
            print_error("Cannot disable plugin: %s\n", strerror(errno));
            return 1;
        }
    }
    std::string sync_error;
    if (!sync_directory(directory, &sync_error)) {
        print_error("Cannot persist plugin state: %s\n", sync_error.c_str());
        return 1;
    }
    if (!enabled) {
        std::string daemon_error;
        if (!stop_plugin_daemons(id, &daemon_error)) {
            print_error("Plugin was disabled but its daemon could not be stopped: %s\n",
                        daemon_error.c_str());
            return 1;
        }
    }
    print_output("Plugin '%s' %s\n", id.c_str(), enabled ? "enabled" : "disabled");
    return 0;
}

int plugin_list() {
    plugin_json::Array output;
    for (const auto& plugin : plugin_discover()) {
        plugin_json::Object item;
        item["id"] = plugin_json::Value(plugin.id);
        item["name"] =
            plugin_json::Value(plugin.manifest.name.empty() ? plugin.id : plugin.manifest.name);
        item["author"] = plugin_json::Value(plugin.manifest.author);
        item["version"] = plugin_json::Value(plugin.manifest.version);
        item["description"] = plugin_json::Value(plugin.manifest.description);
        item["descriptions"] = plugin.manifest.descriptions;
        item["license"] = plugin_json::Value(plugin.manifest.license);
        item["enabled"] = plugin_json::Value(plugin.enabled);
        item["has_manifest"] = plugin_json::Value(plugin.manifest_valid);
        item["has_action"] = plugin_json::Value(plugin.manifest.has_action);
        item["action_unknown"] = plugin_json::Value(plugin.manifest.action_unknown);
        item["min_version"] = plugin_json::Value(static_cast<int64_t>(plugin.manifest.min_version));
        plugin_json::Array callbacks;
        if (plugin.manifest.callbacks) {
            for (const auto& callback : *plugin.manifest.callbacks)
                callbacks.emplace_back(callback);
        }
        item["callbacks"] = plugin_json::Value(std::move(callbacks));
        item["quick_action"] = plugin.manifest.quick_action;
        item["config"] = plugin.manifest.config;
        plugin_json::Array dependencies;
        for (const auto& dependency : plugin.manifest.depends)
            dependencies.emplace_back(dependency);
        item["depends"] = plugin_json::Value(dependencies);
        if (!plugin.error.empty())
            item["error"] = plugin_json::Value(plugin.error);
        output.emplace_back(item);
    }
    print_output("%s\n", plugin_json::dump(plugin_json::Value(output)).c_str());
    return 0;
}

int plugin_run(const std::string& id, const std::string& callback) {
    if (!plugin_id_is_valid(id) || !plugin_callback_is_valid(callback)) {
        print_error("Invalid plugin id or callback\n");
        return 1;
    }
    const PluginRunResult result = run_plugin_callback_isolated(id, callback);
    if (result == PluginRunResult::MissingCallback) {
        print_error("Plugin '%s' has no callback '%s'\n", id.c_str(), callback.c_str());
        return 1;
    }
    return result == PluginRunResult::Success ? 0 : 1;
}

int plugin_show_log(const std::string& id) {
    if (!plugin_id_is_valid(id)) {
        print_error("Invalid plugin id\n");
        return 1;
    }
    const FileLock lock(state_lock_path(id));
    if (!lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", lock.error().c_str());
        return 1;
    }
    const auto content = read_file(fs::path(plugin_directory(id)) / PLUGIN_OUTPUT_LOG);
    if (content)
        (void)std::fwrite(content->data(), 1, content->size(), stdout);
    return 0;
}

int plugin_clear_log(const std::string& id) {
    if (id == "all") {
        for (const auto& plugin : plugin_discover()) {
            const FileLock lock(state_lock_path(plugin.id));
            if (!lock.locked()) {
                LOGW("Failed to lock plugin %s: %s", plugin.id.c_str(), lock.error().c_str());
                continue;
            }
            const fs::path log = fs::path(plugin.directory) / PLUGIN_OUTPUT_LOG;
            if (unlink(log.c_str()) != 0 && errno != ENOENT)
                LOGW("Failed to clear plugin log %s: %s", plugin.id.c_str(), strerror(errno));
        }
        return 0;
    }
    if (!plugin_id_is_valid(id)) {
        print_error("Invalid plugin id\n");
        return 1;
    }
    const FileLock lock(state_lock_path(id));
    if (!lock.locked()) {
        print_error("Cannot lock plugin state: %s\n", lock.error().c_str());
        return 1;
    }
    const fs::path log = fs::path(plugin_directory(id)) / PLUGIN_OUTPUT_LOG;
    if (unlink(log.c_str()) != 0 && errno != ENOENT) {
        print_error("Cannot clear plugin log: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int plugin_config_handle(const std::string& id, const std::vector<std::string>& args) {
    if (!plugin_id_is_valid(id) || !directory_exists(plugin_directory(id))) {
        print_error("Invalid or missing plugin\n");
        return 1;
    }
    if (args.empty()) {
        print_error("Usage: ksud plugin config --id <ID> <get|set|delete|list> [KEY] [VALUE]\n");
        return 1;
    }

    std::string error;
    if (args[0] == "get" && args.size() == 2) {
        std::string value;
        if (!plugin_get_config_value(id, args[1], &value, &error)) {
            print_error("%s\n", error.c_str());
            return 1;
        }
        (void)std::fwrite(value.data(), 1, value.size(), stdout);
        (void)std::fputc('\n', stdout);
        return 0;
    }
    if (args[0] == "set" && args.size() == 3) {
        if (!plugin_set_config_value(id, args[1], args[2], &error)) {
            print_error("%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if (args[0] == "delete" && args.size() == 2) {
        if (!plugin_delete_config_value(id, args[1], &error)) {
            print_error("%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if (args[0] == "list" && args.size() == 1) {
        const FileLock lock(state_lock_path(id));
        if (!lock.locked()) {
            print_error("Cannot lock plugin config: %s\n", lock.error().c_str());
            return 1;
        }
        const auto object =
            read_config_object(fs::path(plugin_directory(id)) / PLUGIN_CONFIG_FILE, &error);
        if (!object) {
            print_error("%s\n", error.c_str());
            return 1;
        }
        print_output("%s\n", plugin_json::dump(plugin_json::Value(*object)).c_str());
        return 0;
    }

    print_error("Usage: ksud plugin config --id <ID> <get|set|delete|list> [KEY] [VALUE]\n");
    return 1;
}

bool parse_interval(const std::string& value, int64_t* interval) {
    int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    *interval = std::max<int64_t>(1, parsed);
    return true;
}

bool parse_descriptor(const std::string& value, int* descriptor) {
    int parsed = -1;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        parsed <= STDERR_FILENO)
        return false;
    *descriptor = parsed;
    return true;
}

void print_usage() {
    print_output("Usage: ksud plugin <SUBCOMMAND>\n\n");
    print_output("SUBCOMMANDS:\n");
    print_output("  install <ZIP>                Install or update a Lua plugin\n");
    print_output("  uninstall <ID>               Uninstall a plugin\n");
    print_output("  enable <ID>                  Enable a plugin\n");
    print_output("  disable <ID>                 Disable a plugin\n");
    print_output("  list                         List plugins as JSON\n");
    print_output("  run <ID> <FUNCTION>          Run a plugin callback\n");
    print_output("  action <ID>                  Run the action callback\n");
    print_output("  daemon <ID> <FUNCTION> <SECONDS>  Start a callback daemon\n");
    print_output("  log <ID>                     Show a plugin log\n");
    print_output("  clear-log <ID|all>           Clear plugin logs\n");
    print_output("  config --id <ID> <get|set|delete|list> [KEY] [VALUE]\n");
}

}  // namespace

bool plugin_id_is_valid(const std::string& id) {
    return identifier_chars_are_valid(id, kMaxPluginIdLength);
}

bool plugin_callback_is_valid(const std::string& callback) {
    if (callback.empty() || callback.size() > kMaxCallbackLength) {
        return false;
    }
    return std::all_of(callback.begin(), callback.end(), [](char character) {
        return ascii_alnum(character) || character == '_' || character == '-' || character == '.' ||
               character == ':';
    });
}

bool plugin_config_key_is_valid(const std::string& key) {
    return !key.empty() && key.size() <= kMaxConfigKeyLength &&
           key.find_first_of("/\\") == std::string::npos && plugin_json::is_valid_utf8(key);
}

std::optional<PluginManifest> plugin_read_manifest(const std::string& directory,
                                                   const std::string& expected_id,
                                                   std::string* error) {
    const fs::path manifest_path = fs::path(directory) / PLUGIN_MANIFEST;
    std::error_code size_error;
    const auto size = fs::file_size(manifest_path, size_error);
    if (!size_error && size > kMaxManifestSize) {
        if (error)
            *error = "plugin.json exceeds the size limit";
        return std::nullopt;
    }
    const auto content = read_file(manifest_path);
    if (!content) {
        if (error)
            *error = "plugin.json is missing";
        return std::nullopt;
    }
    auto root = parse_json(*content, error);
    if (!root || root->type != plugin_json::Type::Object) {
        if (error && error->empty())
            *error = "plugin.json must contain an object";
        return std::nullopt;
    }

    PluginManifest manifest;
    manifest.entry = PLUGIN_ENTRY;
    const auto id = root->o.find("id");
    if (id == root->o.end() || id->second.type != plugin_json::Type::String ||
        !plugin_id_is_valid(id->second.s)) {
        if (error)
            *error = "Manifest id is missing or invalid";
        return std::nullopt;
    }
    manifest.id = id->second.s;
    if (!expected_id.empty() && manifest.id != expected_id) {
        if (error)
            *error = "Manifest id does not match its directory";
        return std::nullopt;
    }

    if (!read_optional_string(root->o, "name", &manifest.name, error) ||
        !read_optional_string(root->o, "version", &manifest.version, error) ||
        !read_optional_string(root->o, "author", &manifest.author, error) ||
        !read_optional_string(root->o, "description", &manifest.description, error) ||
        !read_optional_string(root->o, "license", &manifest.license, error)) {
        return std::nullopt;
    }
    const auto entry = root->o.find("entry");
    if (entry != root->o.end() && entry->second.type != plugin_json::Type::Null &&
        !read_optional_string(root->o, "entry", &manifest.entry, error))
        return std::nullopt;
    if (!identifier_chars_are_valid(manifest.entry, 128)) {
        if (error)
            *error = "Manifest entry must be a safe file name";
        return std::nullopt;
    }

    const auto descriptions = root->o.find("descriptions");
    if (descriptions != root->o.end()) {
        if (!validate_localized_strings(descriptions->second, "descriptions", error))
            return std::nullopt;
        manifest.descriptions = descriptions->second;
    }

    const auto minimum = root->o.find("min_version");
    if (minimum != root->o.end() && minimum->second.type != plugin_json::Type::Null) {
        if (minimum->second.type != plugin_json::Type::Integer || minimum->second.i < 0 ||
            static_cast<uint64_t>(minimum->second.i) > std::numeric_limits<uint32_t>::max()) {
            if (error)
                *error = "Manifest min_version must be a non-negative integer";
            return std::nullopt;
        }
        manifest.min_version = static_cast<uint32_t>(minimum->second.i);
    }

    const auto dependencies = root->o.find("depends");
    if (dependencies != root->o.end()) {
        if (dependencies->second.type != plugin_json::Type::Array) {
            if (error)
                *error = "Manifest depends must be an array";
            return std::nullopt;
        }
        std::set<std::string> unique;
        for (const auto& dependency : dependencies->second.a) {
            if (dependency.type != plugin_json::Type::String || !plugin_id_is_valid(dependency.s) ||
                dependency.s == manifest.id || !unique.insert(dependency.s).second) {
                if (error)
                    *error = "Manifest contains an invalid dependency";
                return std::nullopt;
            }
            manifest.depends.push_back(dependency.s);
        }
    }

    const auto config = root->o.find("config");
    if (config != root->o.end()) {
        if (!validate_config_fields(config->second, error))
            return std::nullopt;
        manifest.config = config->second;
    }

    const auto quick_action = root->o.find("quick_action");
    if (quick_action != root->o.end() && quick_action->second.type != plugin_json::Type::Null) {
        plugin_json::Value normalized;
        if (quick_action->second.type == plugin_json::Type::String) {
            if (!plugin_callback_is_valid(quick_action->second.s)) {
                if (error)
                    *error = "quick_action has an invalid callback";
                return std::nullopt;
            }
            plugin_json::Object action;
            action["function"] = quick_action->second;
            action["label"] = quick_action->second;
            action["labels"] = plugin_json::Value::object();
            normalized = plugin_json::Value(action);
        } else if (quick_action->second.type == plugin_json::Type::Object) {
            const auto function = quick_action->second.o.find("function");
            if (function == quick_action->second.o.end() ||
                function->second.type != plugin_json::Type::String ||
                !plugin_callback_is_valid(function->second.s)) {
                if (error)
                    *error = "quick_action.function is missing or invalid";
                return std::nullopt;
            }
            const auto label = quick_action->second.o.find("label");
            if (label != quick_action->second.o.end() &&
                label->second.type != plugin_json::Type::String) {
                if (error)
                    *error = "quick_action.label must be a string";
                return std::nullopt;
            }
            const auto labels = quick_action->second.o.find("labels");
            if (labels != quick_action->second.o.end() &&
                !validate_localized_strings(labels->second, "quick_action.labels", error)) {
                return std::nullopt;
            }
            normalized = quick_action->second;
        } else {
            if (error)
                *error = "quick_action must be an object";
            return std::nullopt;
        }
        manifest.quick_action = normalized;
        const auto function = normalized.o.find("function");
        manifest.has_action = function != normalized.o.end() && function->second.s == "action";
    }

    const auto declared_action = root->o.find("has_action");
    if (declared_action != root->o.end()) {
        if (declared_action->second.type != plugin_json::Type::Bool) {
            if (error)
                *error = "has_action must be a boolean";
            return std::nullopt;
        }
        manifest.has_action = manifest.has_action || declared_action->second.b;
    }
    const auto callbacks = root->o.find("callbacks");
    if (callbacks != root->o.end()) {
        if (callbacks->second.type != plugin_json::Type::Array) {
            if (error)
                *error = "callbacks must be an array";
            return std::nullopt;
        }
        manifest.callbacks.emplace();
        for (const auto& callback : callbacks->second.a) {
            if (callback.type != plugin_json::Type::String ||
                !plugin_callback_is_valid(callback.s)) {
                if (error)
                    *error = "callbacks contains an invalid name";
                return std::nullopt;
            }
            if (callback.s == "action")
                manifest.has_action = true;
            if (std::find(manifest.callbacks->begin(), manifest.callbacks->end(), callback.s) ==
                manifest.callbacks->end())
                manifest.callbacks->push_back(callback.s);
        }
    }
    manifest.action_unknown = declared_action == root->o.end() && !manifest.callbacks &&
                              manifest.quick_action.type == plugin_json::Type::Null;
    return manifest;
}

std::vector<PluginRecord> plugin_discover() {
    std::vector<PluginRecord> plugins;
    std::error_code error;
    const fs::directory_iterator iterator(PLUGIN_DIR, error);
    if (error)
        return plugins;

    for (auto it = iterator; it != fs::directory_iterator() && !error; it.increment(error)) {
        const auto& entry = *it;
        std::error_code type_error;
        if (!entry.is_directory(type_error) || type_error)
            continue;
        const std::string id = entry.path().filename().string();
        if (!plugin_id_is_valid(id))
            continue;
        PluginRecord record;
        record.id = id;
        record.directory = entry.path().string();
        record.enabled = !path_exists(entry.path() / DISABLE_FILE_NAME);
        auto manifest = plugin_read_manifest(record.directory, id, &record.error);
        if (manifest) {
            record.manifest = std::move(*manifest);
            record.manifest_valid = true;
        } else {
            record.manifest.id = id;
            record.manifest.name = id;
            record.manifest.entry = PLUGIN_ENTRY;
        }
        plugins.push_back(std::move(record));
    }
    std::sort(
        plugins.begin(), plugins.end(),
        [](const PluginRecord& left, const PluginRecord& right) { return left.id < right.id; });
    return plugins;
}

bool plugin_dependencies_available(const PluginManifest& manifest, std::string* error) {
    enum class VisitState : uint8_t { Visiting, Complete };
    std::map<std::string, VisitState> states;
    std::map<std::string, PluginManifest> manifests;
    const auto visit = [&](const auto& self, const PluginManifest& current) -> bool {
        const auto [state, inserted] = states.emplace(current.id, VisitState::Visiting);
        if (!inserted) {
            if (state->second == VisitState::Visiting) {
                if (error)
                    *error = "Dependency cycle involving '" + current.id + "'";
                return false;
            }
            return true;
        }

        for (const auto& dependency : current.depends) {
            const fs::path directory = plugin_directory(dependency);
            std::error_code directory_error;
            if (!fs::is_directory(directory, directory_error) || directory_error) {
                if (error)
                    *error =
                        "Missing dependency '" + dependency + "' required by '" + current.id + "'";
                return false;
            }
            if (path_exists(directory / DISABLE_FILE_NAME)) {
                if (error)
                    *error = "Dependency '" + dependency + "' required by '" + current.id +
                             "' is disabled";
                return false;
            }

            auto found = manifests.find(dependency);
            if (found == manifests.end()) {
                std::string dependency_error;
                auto parsed =
                    plugin_read_manifest(directory.string(), dependency, &dependency_error);
                if (!parsed) {
                    if (error) {
                        *error = "Invalid dependency '";
                        *error += dependency;
                        *error += "' required by '";
                        *error += current.id;
                        *error += "': ";
                        *error += dependency_error;
                    }
                    return false;
                }
                found = manifests.emplace(dependency, std::move(*parsed)).first;
            }
            if (!self(self, found->second))
                return false;
        }
        state->second = VisitState::Complete;
        return true;
    };
    return visit(visit, manifest);
}

std::vector<PluginRecord> plugin_resolve_enabled(std::vector<std::string>* errors) {
    const auto discovered = plugin_discover();
    std::map<std::string, PluginRecord> candidates;
    std::map<std::string, PluginRecord> installed;
    for (const auto& plugin : discovered) {
        installed.emplace(plugin.id, plugin);
        if (plugin.enabled && plugin.manifest_valid)
            candidates.emplace(plugin.id, plugin);
        else if (plugin.enabled && !plugin.manifest_valid && errors)
            errors->push_back(plugin.id + ": " + plugin.error);
    }

    std::set<std::string> blocked;
    for (const auto& [id, plugin] : candidates) {
        for (const auto& dependency : plugin.manifest.depends) {
            const auto found = installed.find(dependency);
            if (found == installed.end() || !found->second.enabled ||
                !found->second.manifest_valid) {
                blocked.insert(id);
                if (errors) {
                    std::string message = id;
                    message += ": unavailable dependency '";
                    message += dependency;
                    message += "'";
                    errors->push_back(std::move(message));
                }
                break;
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [id, plugin] : candidates) {
            if (blocked.count(id) != 0)
                continue;
            if (std::any_of(plugin.manifest.depends.begin(), plugin.manifest.depends.end(),
                            [&](const std::string& dependency) {
                                return blocked.count(dependency) != 0;
                            })) {
                blocked.insert(id);
                changed = true;
            }
        }
    }

    std::map<std::string, size_t> indegree;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& [id, plugin] : candidates) {
        if (blocked.count(id) != 0)
            continue;
        indegree[id] = 0;
        for (const auto& dependency : plugin.manifest.depends) {
            if (candidates.count(dependency) != 0 && blocked.count(dependency) == 0) {
                ++indegree[id];
                dependents[dependency].push_back(id);
            }
        }
    }

    std::priority_queue<std::string, std::vector<std::string>, std::greater<>> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0)
            ready.push(id);
    }
    std::vector<PluginRecord> ordered;
    while (!ready.empty()) {
        const std::string id = ready.top();
        ready.pop();
        ordered.push_back(candidates.at(id));
        for (const auto& dependent : dependents[id]) {
            auto& degree = indegree[dependent];
            if (--degree == 0)
                ready.push(dependent);
        }
    }
    if (ordered.size() != indegree.size() && errors) {
        for (const auto& [id, degree] : indegree) {
            if (degree != 0)
                errors->push_back(id + ": dependency cycle detected");
        }
    }
    return ordered;
}

bool plugin_get_config_value(const std::string& id, const std::string& key, std::string* value,
                             std::string* error) {
    if (!plugin_id_is_valid(id) || !plugin_config_key_is_valid(key)) {
        *error = "Invalid plugin id or config key";
        return false;
    }
    const FileLock lock(state_lock_path(id));
    if (!lock.locked()) {
        *error = "Cannot lock config: " + lock.error();
        return false;
    }
    const auto object =
        read_config_object(fs::path(plugin_directory(id)) / PLUGIN_CONFIG_FILE, error);
    if (!object)
        return false;
    const auto found = object->find(key);
    *value = found == object->end() ? std::string() : value_as_string(found->second);
    return true;
}

bool plugin_set_config_value(const std::string& id, const std::string& key,
                             const std::string& value, std::string* error) {
    if (!plugin_id_is_valid(id) || !plugin_config_key_is_valid(key) ||
        !directory_exists(plugin_directory(id))) {
        *error = "Invalid plugin id or config key";
        return false;
    }
    if (value.size() > kMaxConfigSize) {
        *error = "Config value exceeds the size limit";
        return false;
    }
    if (!plugin_json::is_valid_utf8(value)) {
        *error = "Config value must be valid UTF-8";
        return false;
    }
    const FileLock lock(state_lock_path(id));
    if (!lock.locked()) {
        *error = "Cannot lock config: " + lock.error();
        return false;
    }
    const fs::path path = fs::path(plugin_directory(id)) / PLUGIN_CONFIG_FILE;
    auto object = read_config_object(path, error);
    if (!object)
        return false;
    (*object)[key] = plugin_json::Value(value);
    return write_config_object(path, *object, error);
}

bool plugin_delete_config_value(const std::string& id, const std::string& key, std::string* error) {
    if (!plugin_id_is_valid(id) || !plugin_config_key_is_valid(key) ||
        !directory_exists(plugin_directory(id))) {
        *error = "Invalid plugin id or config key";
        return false;
    }
    const FileLock lock(state_lock_path(id));
    if (!lock.locked()) {
        *error = "Cannot lock config: " + lock.error();
        return false;
    }
    const fs::path path = fs::path(plugin_directory(id)) / PLUGIN_CONFIG_FILE;
    auto object = read_config_object(path, error);
    if (!object)
        return false;
    object->erase(key);
    return write_config_object(path, *object, error);
}

void plugin_append_log(const std::string& plugin_dir, const char* level,
                       const std::string& message) {
    const std::string id = fs::path(plugin_dir).filename().string();
    if (!plugin_id_is_valid(id))
        return;
    const FileLock state_lock(state_lock_path(id));
    if (!state_lock.locked())
        return;
    const fs::path path = fs::path(plugin_dir) / PLUGIN_OUTPUT_LOG;
    const ScopedFd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
    if (!fd.valid())
        return;
    if (flock(fd.get(), LOCK_EX) != 0)
        return;
    std::string line = "[" + std::string(level) + "] ";
    const size_t available =
        kMaxPluginLogLineSize - std::min(line.size() + 1, kMaxPluginLogLineSize);
    line.append(message.data(), std::min(message.size(), available));
    line.push_back('\n');

    struct stat status{};
    if (fstat(fd.get(), &status) == 0 &&
        status.st_size > kMaxPluginLogSize - static_cast<off_t>(line.size())) {
        if (ftruncate(fd.get(), 0) == 0)
            (void)lseek(fd.get(), 0, SEEK_SET);
    }
    (void)write_all(fd.get(), line.data(), line.size());
}

int plugin_handle(const std::vector<std::string>& args) {
    if (!switch_mnt_ns(1)) {
        print_error("Cannot enter init mount namespace\n");
        return 1;
    }
    if (args.empty()) {
        print_usage();
        return 1;
    }

    const std::string& command = args[0];
    if (command == "install" && args.size() == 2)
        return plugin_install(args[1]);
    if (command == "uninstall" && args.size() == 2)
        return plugin_uninstall(args[1]);
    if (command == "enable" && args.size() == 2)
        return plugin_set_enabled(args[1], true);
    if (command == "disable" && args.size() == 2)
        return plugin_set_enabled(args[1], false);
    if (command == "list" && args.size() == 1)
        return plugin_list();
    if (command == "run" && args.size() == 3)
        return plugin_run(args[1], args[2]);
    if (command == "action" && args.size() == 2)
        return plugin_run(args[1], "action");
    if (command == "daemon" && (args.size() == 4 || args.size() == 6)) {
        int64_t interval = 0;
        if (!parse_interval(args[3], &interval)) {
            print_error("Invalid daemon interval\n");
            return 1;
        }
        if (args.size() == 4)
            return launch_plugin_daemon(args[1], args[2], interval) ? 0 : 1;
        int ready_fd = -1;
        if (args[4] != "--ready-fd" || !parse_descriptor(args[5], &ready_fd)) {
            print_error("Invalid daemon readiness descriptor\n");
            return 1;
        }
        return start_plugin_daemon(args[1], args[2], interval, ready_fd) ? 0 : 1;
    }
    if (command == "log" && args.size() == 2)
        return plugin_show_log(args[1]);
    if (command == "clear-log" && args.size() == 2)
        return plugin_clear_log(args[1]);
    if (command == "config") {
        if (args.size() < 4 || args[1] != "--id") {
            print_error(
                "Usage: ksud plugin config --id <ID> <get|set|delete|list> [KEY] [VALUE]\n");
            return 1;
        }
        return plugin_config_handle(args[2],
                                    std::vector<std::string>(args.begin() + 3, args.end()));
    }

    print_usage();
    return 1;
}

}  // namespace ksud
