#include "../src/assets.hpp"
#include "../src/xz_range.hpp"

#include <lzma.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
bool corrupt_digest = false;
int digests = 0;
std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace

namespace ksud {
void log_e(const char*, ...) {}
}  // namespace ksud

extern "C" int __real_mbedtls_sha256(const unsigned char*, size_t, unsigned char*, int);
extern "C" int __wrap_mbedtls_sha256(const unsigned char* input, size_t size, unsigned char* output,
                                     int is224) {
    ++digests;
    const int result = __real_mbedtls_sha256(input, size, output, is224);
    if (corrupt_digest)
        output[0] ^= 1;
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const auto root = std::filesystem::path(argv[1]);
    for (const char* name :
         {"android14-6.1_kernelsu.ko", "android16-6.12_kernelsu.ko", "payload.bin", "empty.bin"}) {
        const auto expected = read_bytes(root / name);
        const auto image = ksud::get_asset_data(name);
        assert(image && *image == expected);
        const auto extracted = root.parent_path() / "extracted";
        assert(ksud::copy_asset_to_file(name, extracted.string()));
        assert(read_bytes(extracted) == expected);
    }
    assert(digests == 2);
    assert(!ksud::get_asset_data("missing.ko"));
    corrupt_digest = true;
    assert(!ksud::get_asset_data("android14-6.1_kernelsu.ko") && digests == 3);

    const auto raw = read_bytes(root / "android16-6.12_kernelsu.ko");
    std::vector<uint8_t> packed(lzma_stream_buffer_bound(raw.size()));
    size_t size = 0;
    assert(lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr, raw.data(), raw.size(),
                                   packed.data(), &size, packed.size()) == LZMA_OK);
    packed.resize(size);
    const size_t offset = 65000;
    std::vector<uint8_t> range(80000);
    const auto unpack = [&] {
        return yukisu_unpack_xz_range_to_buffer(packed.data(), packed.size(), offset, range.size(),
                                                raw.size(), range.data());
    };
    assert(unpack() == 0);
    assert(
        (range == std::vector<uint8_t>(raw.begin() + offset, raw.begin() + offset + range.size())));
    assert(yukisu_unpack_xz_range_to_buffer(packed.data(), packed.size(), raw.size(), 1, raw.size(),
                                            range.data()) != 0);
    assert(yukisu_unpack_xz_range_to_buffer(packed.data(), packed.size(), 0, 1, raw.size(),
                                            nullptr) != 0);
    assert(yukisu_unpack_xz_range_to_buffer(packed.data(), packed.size(), 0, 0, raw.size(),
                                            nullptr) == 0);
    assert(yukisu_unpack_xz_range_to_buffer(packed.data(), packed.size(), offset, 0, raw.size(),
                                            nullptr) == 0);
    packed.back() ^= 1;
    assert(unpack() != 0);
    packed.pop_back();
    assert(unpack() != 0);
}
