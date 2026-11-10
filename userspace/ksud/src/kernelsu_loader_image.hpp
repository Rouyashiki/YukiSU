#pragma once

#include <elf.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ksud::kernelsu_loader::detail {

template <typename T>
T read_elf(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

template <typename T>
void write_elf(std::vector<uint8_t>& data, size_t offset, const T& value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

template <typename Ehdr, typename Shdr>
class ElfSections {
public:
    explicit ElfSections(const std::vector<uint8_t>& data) : data_(&data) {
        if (data.size() < sizeof(Ehdr))
            return;
        header_ = read_elf<Ehdr>(data, 0);
        valid_ = std::memcmp(header_.e_ident, ELFMAG, SELFMAG) == 0 &&
                 header_.e_ident[EI_DATA] == ELFDATA2LSB && header_.e_type == ET_REL &&
                 header_.e_shentsize == sizeof(Shdr) && header_.e_shnum != 0 &&
                 contains(header_.e_shoff, uint64_t{header_.e_shnum} * sizeof(Shdr));
    }

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] size_t count() const { return header_.e_shnum; }
    [[nodiscard]] size_t names_index() const { return header_.e_shstrndx; }
    [[nodiscard]] size_t offset(size_t index) const {
        return header_.e_shoff + (index * sizeof(Shdr));
    }
    [[nodiscard]] Shdr section(size_t index) const { return read_elf<Shdr>(*data_, offset(index)); }

    [[nodiscard]] bool contains(uint64_t offset, uint64_t size) const {
        return offset <= data_->size() && size <= data_->size() - offset;
    }

    [[nodiscard]] std::optional<std::string_view> string(const Shdr& table, uint64_t offset) const {
        if (table.sh_type != SHT_STRTAB || !contains(table.sh_offset, table.sh_size) ||
            offset >= table.sh_size)
            return std::nullopt;
        const auto* begin = reinterpret_cast<const char*>(data_->data() + table.sh_offset + offset);
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, table.sh_size - offset));
        if (!end)
            return std::nullopt;
        return std::string_view(begin, static_cast<size_t>(end - begin));
    }

private:
    const std::vector<uint8_t>* data_;
    Ehdr header_{};
    bool valid_ = false;
};

inline std::optional<std::string> required_vermagic(std::string_view logs) {
    constexpr std::string_view prefix = "version magic '";
    constexpr std::string_view separator = "' should be '";
    std::optional<std::string> result;
    while (!logs.empty()) {
        const size_t newline = logs.find('\n');
        auto line = logs.substr(0, newline);
        const size_t start = line.find(prefix);
        if (start != std::string_view::npos) {
            line.remove_prefix(start + prefix.size());
            const size_t mismatch = line.find(separator);
            if (mismatch != std::string_view::npos) {
                line.remove_prefix(mismatch + separator.size());
                const size_t end = line.find('\'');
                if (end != std::string_view::npos && end != 0)
                    result = std::string(line.substr(0, end));
            }
        }
        if (newline == std::string_view::npos)
            break;
        logs.remove_prefix(newline + 1);
    }
    return result;
}

template <typename Ehdr, typename Shdr>
bool replace_vermagic(std::vector<uint8_t>& data, std::string_view required) {
    const ElfSections<Ehdr, Shdr> elf(data);
    if (!elf.valid() || elf.names_index() >= elf.count() || required.empty() ||
        required.find('\0') != std::string_view::npos)
        return false;
    const auto names = elf.section(elf.names_index());
    for (size_t i = 0; i < elf.count(); ++i) {
        auto section = elf.section(i);
        const auto name = elf.string(names, section.sh_name);
        if (!name || *name != ".modinfo")
            continue;
        if (section.sh_type != SHT_PROGBITS || !elf.contains(section.sh_offset, section.sh_size))
            return false;
        const auto* begin = reinterpret_cast<const char*>(data.data() + section.sh_offset);
        std::string_view entries(begin, section.sh_size);
        std::string replacement;
        while (!entries.empty()) {
            const size_t end = entries.find('\0');
            if (end == std::string_view::npos)
                return false;
            const auto entry = entries.substr(0, end);
            if (!entry.empty() && entry.substr(0, 9) != "vermagic=") {
                replacement.append(entry);
                replacement.push_back('\0');
            }
            entries.remove_prefix(end + 1);
        }
        replacement.append("vermagic=");
        replacement.append(required);
        replacement.push_back('\0');

        const uint64_t alignment = section.sh_addralign ? section.sh_addralign : 1;
        if ((alignment & (alignment - 1)) != 0 ||
            alignment - 1 > std::numeric_limits<size_t>::max() - data.size())
            return false;
        const size_t offset = (data.size() + alignment - 1) & ~(alignment - 1);
        if (offset > std::numeric_limits<decltype(section.sh_offset)>::max() ||
            replacement.size() > std::numeric_limits<decltype(section.sh_size)>::max() ||
            replacement.size() > std::numeric_limits<size_t>::max() - offset)
            return false;
        section.sh_offset = static_cast<decltype(section.sh_offset)>(offset);
        section.sh_size = static_cast<decltype(section.sh_size)>(replacement.size());
        const size_t header_offset = elf.offset(i);
        data.resize(offset, 0);
        data.insert(data.end(), replacement.begin(), replacement.end());
        write_elf(data, header_offset, section);
        return true;
    }
    return false;
}

}  // namespace ksud::kernelsu_loader::detail
