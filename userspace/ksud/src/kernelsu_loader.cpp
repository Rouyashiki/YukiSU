#include "kernelsu_loader.hpp"
#include "kernelsu_loader_image.hpp"

#include "core/ksucalls.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <elf.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ksud::kernelsu_loader {

namespace {

class KptrGuard {
public:
    KptrGuard() {
        const auto current = read_file(kPath);
        if (!current)
            return;
        original_value_ = *current;
        valid_ = write_file(kPath, "1");
    }

    ~KptrGuard() {
        if (!original_value_.empty()) {
            (void)write_file(kPath, original_value_);
        }
    }

    KptrGuard(const KptrGuard&) = delete;
    KptrGuard& operator=(const KptrGuard&) = delete;
    KptrGuard(KptrGuard&&) = delete;
    KptrGuard& operator=(KptrGuard&&) = delete;
    [[nodiscard]] bool valid() const { return valid_; }

private:
    static constexpr const char* kPath = "/proc/sys/kernel/kptr_restrict";
    std::string original_value_;
    bool valid_ = false;
};

void normalize_symbol_name(std::string* name) {
    size_t pos = name->find('$');
    if (pos == std::string::npos) {
        pos = name->find(".llvm.");
    }
    if (pos != std::string::npos) {
        name->resize(pos);
    }
}

bool read_file_bytes(const char* path, std::vector<uint8_t>* buffer) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("loader: cannot open %s", path);
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        LOGE("loader: cannot stat %s", path);
        close(fd);
        return false;
    }

    buffer->resize(static_cast<size_t>(st.st_size));
    size_t filled = 0;
    while (filled < buffer->size()) {
        const ssize_t n = read(fd, buffer->data() + filled, buffer->size() - filled);
        if (n > 0) {
            filled += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        LOGE("loader: cannot read %s", path);
        close(fd);
        return false;
    }
    close(fd);

    return true;
}

int init_module_syscall(void* module_image, unsigned long len, const char* param_values) {
    return syscall(__NR_init_module, module_image, len, param_values);
}

template <typename Ehdr, typename Shdr, typename Sym>
bool patch_undefined_symbols(std::vector<uint8_t>* buffer, bool kernel_lookup) {
    const detail::ElfSections<Ehdr, Shdr> elf(*buffer);
    if (!elf.valid()) {
        LOGE("loader: invalid ELF section table");
        return false;
    }
    std::optional<Shdr> symtab;
    for (size_t i = 0; i < elf.count(); ++i) {
        const auto section = elf.section(i);
        if (section.sh_type == SHT_SYMTAB) {
            symtab = section;
            break;
        }
    }
    if (!symtab || symtab->sh_link >= elf.count() || symtab->sh_entsize != sizeof(Sym) ||
        symtab->sh_size % sizeof(Sym) != 0 || !elf.contains(symtab->sh_offset, symtab->sh_size)) {
        LOGE("loader: invalid or missing symbol table");
        return false;
    }
    const auto strtab = elf.section(symtab->sh_link);
    const size_t sym_count = symtab->sh_size / sizeof(Sym);
    std::unordered_map<std::string, std::vector<size_t>> unresolved_symbols;

    for (size_t i = 1; i < sym_count; ++i) {
        const size_t offset = symtab->sh_offset + (i * sizeof(Sym));
        const auto sym = detail::read_elf<Sym>(*buffer, offset);
        if (sym.st_shndx != SHN_UNDEF) {
            continue;
        }

        const auto name = elf.string(strtab, sym.st_name);
        if (!name) {
            LOGE("loader: invalid symbol name");
            return false;
        }
        if (name->empty()) {
            continue;
        }
        unresolved_symbols[std::string(*name)].push_back(offset);
    }

    if (unresolved_symbols.empty()) {
        return true;
    }
    const auto bind = [buffer](size_t offset, uint64_t address) {
        auto symbol = detail::read_elf<Sym>(*buffer, offset);
        symbol.st_shndx = SHN_ABS;
        symbol.st_value = static_cast<decltype(symbol.st_value)>(address);
        detail::write_elf(*buffer, offset, symbol);
    };

    if (kernel_lookup && get_uapi_version() >= 6) {
        std::vector<std::string> names;
        names.reserve(unresolved_symbols.size());
        for (const auto& entry : unresolved_symbols)
            names.push_back(entry.first);
        std::vector<uint64_t> addresses;
        if (lookup_symbols(names, addresses) && addresses.size() == names.size()) {
            for (size_t i = 0; i < names.size(); ++i) {
                if (!addresses[i]) {
                    LOGW("loader: cannot find symbol: %s", names[i].c_str());
                    continue;
                }
                for (const auto offset : unresolved_symbols.at(names[i]))
                    bind(offset, addresses[i]);
            }
            return true;
        }
        LOGW("loader: kernel symbol lookup failed, using /proc/kallsyms");
    }

    KptrGuard const guard;
    if (!guard.valid()) {
        LOGE("loader: cannot enable kallsyms access: %s", strerror(errno));
        return false;
    }
    // Streamed, not read whole: /proc/kallsyms is several MB and this scan stops
    // at the first module-owned symbol.
    std::string name;
    const bool opened = for_each_file_line("/proc/kallsyms", [&](std::string_view line) {
        if (unresolved_symbols.empty()) {
            return false;
        }
        std::string_view rest = line;
        const std::string_view addr_str = next_token(&rest);
        const std::string_view type = next_token(&rest);
        const std::string_view sym_name = next_token(&rest);
        (void)type;
        if (addr_str.empty() || sym_name.empty()) {
            return true;
        }

        // Kernel symbols come before module symbols in /proc/kallsyms. Stop scanning once we
        // reach module-owned entries so we don't accidentally relocate against them.
        if (!next_token(&rest).empty()) {
            return false;
        }

        // next_token yields a view into `line`, which is not NUL-terminated, so
        // bound strtoull explicitly rather than relying on *end == '\0'.
        std::array<char, 32> addr_buf{};
        if (addr_str.size() >= addr_buf.size()) {
            return true;
        }
        std::memcpy(addr_buf.data(), addr_str.data(), addr_str.size());
        char* end = nullptr;
        errno = 0;
        uint64_t const addr = std::strtoull(addr_buf.data(), &end, 16);
        if (end == addr_buf.data() || *end != '\0' || errno == ERANGE) {
            return true;
        }

        name.assign(sym_name);
        normalize_symbol_name(&name);

        const auto it = unresolved_symbols.find(name);
        if (it == unresolved_symbols.end()) {
            return true;
        }

        if (addr == 0) {
            LOGW("loader: kallsyms address is hidden for %s", name.c_str());
            return true;
        }
        for (const auto offset : it->second)
            bind(offset, addr);
        unresolved_symbols.erase(it);
        return true;
    });
    if (!opened) {
        LOGE("loader: cannot open /proc/kallsyms");
        return false;
    }

    for (const auto& [name, _] : unresolved_symbols) {
        LOGW("loader: cannot find symbol: %s", name.c_str());
    }

    return true;
}

class KernelLog {
public:
    KernelLog() {
        for (const char* path : {"/dev/kmsg", "/kmsg"}) {
            fd_ = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd_ < 0)
                continue;
            if (lseek(fd_, 0, SEEK_END) >= 0)
                return;
            close(fd_);
            fd_ = -1;
        }
        LOGW("loader: kernel log unavailable; vermagic fallback disabled: %s", strerror(errno));
    }

    ~KernelLog() {
        if (fd_ >= 0)
            close(fd_);
    }
    KernelLog(const KernelLog&) = delete;
    KernelLog& operator=(const KernelLog&) = delete;
    KernelLog(KernelLog&&) = delete;
    KernelLog& operator=(KernelLog&&) = delete;

    [[nodiscard]] std::string read_new() const {
        std::string logs;
        std::array<char, 8192> record{};
        while (fd_ >= 0) {
            const ssize_t count = read(fd_, record.data(), record.size());
            if (count > 0) {
                logs.append(record.data(), static_cast<size_t>(count));
                logs.push_back('\n');
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        return logs;
    }

private:
    int fd_ = -1;
};

void log_kernel_errors(const std::string& logs) {
    for_each_line(logs, [](std::string_view line) {
        LOGE("loader: kernel: %.*s", static_cast<int>(line.size()), line.data());
    });
}

}  // namespace

bool load_module(const char* path, const std::string& param_values, bool kernel_lookup) {
    std::vector<uint8_t> buffer;
    if (!read_file_bytes(path, &buffer)) {
        return false;
    }
    return load_module(std::move(buffer), param_values, kernel_lookup);
}

bool load_module(std::vector<uint8_t> buffer, const std::string& param_values, bool kernel_lookup) {
    if (buffer.size() < EI_NIDENT || memcmp(buffer.data(), ELFMAG, SELFMAG) != 0) {
        LOGE("loader: invalid ELF image");
        return false;
    }

    const unsigned char elf_class = buffer[EI_CLASS];
    bool patched = false;
    if (elf_class == ELFCLASS64) {
        patched =
            patch_undefined_symbols<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(&buffer, kernel_lookup);
    } else if (elf_class == ELFCLASS32) {
        patched =
            patch_undefined_symbols<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(&buffer, kernel_lookup);
    } else {
        LOGE("loader: unsupported ELF class %u", elf_class);
        return false;
    }

    if (!patched) {
        return false;
    }

    const KernelLog kernel_log;
    LOGI("loader: calling init_module (%zu bytes)", buffer.size());
    if (init_module_syscall(buffer.data(), buffer.size(), param_values.c_str()) != 0) {
        const int first_error = errno;
        const std::string logs = kernel_log.read_new();
        log_kernel_errors(logs);
        const auto required = detail::required_vermagic(logs);
        if (!required) {
            LOGE("loader: init_module failed: %s", strerror(first_error));
            errno = first_error;
            return false;
        }
        const bool replaced =
            elf_class == ELFCLASS64
                ? detail::replace_vermagic<Elf64_Ehdr, Elf64_Shdr>(buffer, *required)
                : detail::replace_vermagic<Elf32_Ehdr, Elf32_Shdr>(buffer, *required);
        if (!replaced) {
            LOGE("loader: cannot replace module vermagic");
            return false;
        }
        LOGW("loader: retrying init_module with kernel-required vermagic '%s'", required->c_str());
        if (init_module_syscall(buffer.data(), buffer.size(), param_values.c_str()) != 0) {
            const int error = errno;
            log_kernel_errors(kernel_log.read_new());
            LOGE("loader: init_module failed after replacing vermagic: %s", strerror(error));
            errno = error;
            return false;
        }
    }

    LOGI("loader: module loaded successfully");
    return true;
}

}  // namespace ksud::kernelsu_loader
