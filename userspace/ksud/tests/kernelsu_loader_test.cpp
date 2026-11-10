#include "../src/kernelsu_loader.hpp"
#include "../src/kernelsu_loader_image.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <filesystem>
#include <fstream>

namespace image = ksud::kernelsu_loader::detail;
namespace {

std::string module_path;
std::string log_path;
std::string kallsyms_path;
int calls;
int first_error;
int retry_error;
bool log_available = true;
std::string new_log;
std::vector<uint8_t> submitted;
uint64_t symbol_address = 0;
std::vector<std::string> kptr_writes;

std::string temp_file() {
    char path[] = "/tmp/ksud-loader-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    return path;
}

template <typename Ehdr = Elf64_Ehdr, typename Shdr = Elf64_Shdr, typename Sym = Elf64_Sym>
std::vector<uint8_t> fixture(bool symbols = false) {
    std::vector<uint8_t> data(640);
    Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = sizeof(Ehdr) == sizeof(Elf64_Ehdr) ? ELFCLASS64 : ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_type = ET_REL;
    header.e_shoff = 64;
    header.e_shentsize = sizeof(Shdr);
    header.e_shnum = 5;
    header.e_shstrndx = 1;
    image::write_elf(data, 0, header);
    const char names[] = "\0.shstrtab\0.modinfo\0.symtab\0.strtab\0";
    std::memcpy(data.data() + 384, names, sizeof(names));
    Shdr section{};
    section.sh_name = 1;
    section.sh_type = SHT_STRTAB;
    section.sh_offset = 384;
    section.sh_size = sizeof(names);
    image::write_elf(data, 64 + sizeof(Shdr), section);
    const char info[] = "license=GPL\0vermagic=old\0name=kernelsu\0";
    std::memcpy(data.data() + 424, info, sizeof(info));
    section = {};
    section.sh_name = 11;
    section.sh_type = SHT_PROGBITS;
    section.sh_offset = 424;
    section.sh_size = sizeof(info);
    section.sh_addralign = 8;
    image::write_elf(data, 64 + 2 * sizeof(Shdr), section);
    section = {};
    section.sh_name = 20;
    section.sh_type = SHT_SYMTAB;
    section.sh_offset = 480;
    section.sh_size = (symbols ? 3 : 1) * sizeof(Sym);
    section.sh_entsize = sizeof(Sym);
    section.sh_link = 4;
    image::write_elf(data, 64 + 3 * sizeof(Shdr), section);
    if (symbols) {
        Sym symbol{};
        symbol.st_name = 1;
        image::write_elf(data, 480 + sizeof(Sym), symbol);
        image::write_elf(data, 480 + 2 * sizeof(Sym), symbol);
    }
    section = {};
    section.sh_name = 28;
    section.sh_type = SHT_STRTAB;
    section.sh_offset = 560;
    constexpr char symbol_names[] = "\0test_symbol\0";
    section.sh_size = sizeof(symbol_names);
    std::memcpy(data.data() + 560, symbol_names, sizeof(symbol_names));
    image::write_elf(data, 64 + 4 * sizeof(Shdr), section);
    return data;
}

void save_module(const std::vector<uint8_t>& data) {
    std::ofstream file(module_path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    assert(file.good());
}

void reset(int error, std::string logs = {}, int second_error = 0) {
    calls = 0;
    first_error = error;
    retry_error = second_error;
    new_log = std::move(logs);
    submitted.clear();
    kptr_writes.clear();
    log_available = true;
    std::ofstream(log_path, std::ios::trunc) << "stale version magic 'old' should be 'wrong'\n";
    save_module(fixture());
}

}  // namespace

namespace ksud {
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
uint32_t get_uapi_version() {
    return 6;
}
bool lookup_symbols(const std::vector<std::string>& names, std::vector<uint64_t>& addresses) {
    assert(names.size() == 1 && names.front() == "test_symbol");
    addresses = {symbol_address};
    return true;
}
std::optional<std::string> read_file(const std::string& path) {
    assert(path == "/proc/sys/kernel/kptr_restrict");
    return "2\n";
}
bool write_file(const std::filesystem::path& path, const std::string& value) {
    assert(path == "/proc/sys/kernel/kptr_restrict");
    kptr_writes.push_back(value);
    return true;
}
std::string_view next_token(std::string_view* input) {
    const size_t start = input->find_first_not_of(" \t\n");
    if (start == std::string_view::npos) {
        *input = {};
        return {};
    }
    input->remove_prefix(start);
    const size_t length = input->find_first_of(" \t\n");
    const auto token = input->substr(0, length);
    input->remove_prefix(token.size());
    return token;
}
}  // namespace ksud

extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    if (std::strcmp(path, "/dev/kmsg") == 0 || std::strcmp(path, "/kmsg") == 0) {
        if (!log_available) {
            errno = EACCES;
            return -1;
        }
        return __real_open(log_path.c_str(), flags, mode);
    }
    if (std::strcmp(path, "/proc/kallsyms") == 0)
        return __real_open(kallsyms_path.c_str(), flags, mode);
    return __real_open(path, flags, mode);
}

extern "C" long __wrap_syscall(long number, ...) {
    assert(number == SYS_init_module);
    va_list args;
    va_start(args, number);
    const auto* data = va_arg(args, const uint8_t*);
    const auto size = va_arg(args, unsigned long);
    const auto* params = va_arg(args, const char*);
    assert(std::strcmp(params, "allow_shell=1") == 0);
    va_end(args);
    submitted.assign(data, data + size);
    ++calls;
    if (calls == 1)
        std::ofstream(log_path, std::ios::app) << new_log;
    const int error = calls == 1 ? first_error : retry_error;
    errno = error;
    return error ? -1 : 0;
}

int main() {
    module_path = temp_file();
    log_path = temp_file();
    kallsyms_path = temp_file();
    const auto load = [] {
        return ksud::kernelsu_loader::load_module(module_path.c_str(), "allow_shell=1");
    };
    reset(0);
    assert(load() && calls == 1);
    reset(0);
    assert(ksud::kernelsu_loader::load_module(fixture(), "allow_shell=1") && calls == 1);
    const std::string required = "6.12.38-android16 SMP preempt mod_unload aarch64";
    const std::string mismatch =
        "3,42,123,-;kernelsu: version magic 'old' should be '" + required + "'\n";
    reset(ENOEXEC, mismatch);
    assert(load() && calls == 2);
    const image::ElfSections<Elf64_Ehdr, Elf64_Shdr> elf(submitted);
    assert(elf.valid());
    const auto section = elf.section(2);
    assert(section.sh_offset % 8 == 0);
    const std::string info(reinterpret_cast<const char*>(submitted.data() + section.sh_offset),
                           section.sh_size);
    assert(info.find("vermagic=" + required) != std::string::npos);
    assert(info.find("license=GPL") != std::string::npos);
    assert(info.find("name=kernelsu") != std::string::npos);
    assert(info.find("vermagic=old") == std::string::npos);
    reset(ENOEXEC, mismatch);
    save_module(fixture<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>());
    assert(load() && calls == 2);
    const image::ElfSections<Elf32_Ehdr, Elf32_Shdr> elf32(submitted);
    assert(elf32.valid());
    const auto section32 = elf32.section(2);
    const std::string info32(reinterpret_cast<const char*>(submitted.data() + section32.sh_offset),
                             section32.sh_size);
    assert(info32.find("vermagic=" + required) != std::string::npos);

    reset(0);
    save_module(fixture(true));
    symbol_address = 0xffff000012345678ULL;
    assert(ksud::kernelsu_loader::load_module(module_path.c_str(), "allow_shell=1", true));
    for (size_t i = 1; i <= 2; ++i) {
        const auto symbol = image::read_elf<Elf64_Sym>(submitted, 480 + i * sizeof(Elf64_Sym));
        assert(symbol.st_shndx == SHN_ABS && symbol.st_value == symbol_address);
    }
    reset(0);
    save_module(fixture(true));
    symbol_address = 0;
    assert(ksud::kernelsu_loader::load_module(module_path.c_str(), "allow_shell=1", true));
    assert(image::read_elf<Elf64_Sym>(submitted, 480 + sizeof(Elf64_Sym)).st_shndx == SHN_UNDEF);

    reset(0);
    save_module(fixture(true));
    std::ofstream(kallsyms_path, std::ios::trunc)
        << "ffff000012345678 T test_symbol.llvm.123\nffff000087654321 T test_symbol\n";
    assert(load());
    assert(image::read_elf<Elf64_Sym>(submitted, 480 + sizeof(Elf64_Sym)).st_value ==
           0xffff000012345678ULL);
    assert((kptr_writes == std::vector<std::string>{"1", "2\n"}));
    reset(ENOEXEC, mismatch, EINVAL);
    assert(!load() && calls == 2);
    reset(EEXIST);
    assert(!load() && calls == 1);
    reset(EPERM);
    assert(!load() && calls == 1);
    reset(ENOEXEC, mismatch);
    log_available = false;
    assert(!load() && calls == 1);
    reset(ENOEXEC, "kernelsu: version magic 'unterminated\n");
    assert(!load() && calls == 1);
    reset(0);
    auto invalid = fixture();
    auto header = image::read_elf<Elf64_Ehdr>(invalid, 0);
    header.e_shoff = UINT64_MAX;
    image::write_elf(invalid, 0, header);
    save_module(invalid);
    assert(!load() && calls == 0);
    assert((!image::replace_vermagic<Elf64_Ehdr, Elf64_Shdr>(invalid, required)));
    unlink(module_path.c_str());
    unlink(log_path.c_str());
    unlink(kallsyms_path.c_str());
}
