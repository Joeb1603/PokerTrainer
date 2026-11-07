// The developer --screenshot flag's PNG writer (src/app/screenshot.h).

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "app/screenshot.h"

using namespace poker;

namespace {

std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t at) {
    return (std::uint32_t{data[at]} << 24) | (std::uint32_t{data[at + 1]} << 16) | (std::uint32_t{data[at + 2]} << 8) |
           std::uint32_t{data[at + 3]};
}

}  // namespace

TEST(Screenshot, ChecksumsMatchKnownValues) {
    EXPECT_EQ(crc32(bytes("IEND")), 0xAE426082u);
    EXPECT_EQ(crc32(bytes("123456789")), 0xCBF43926u);
    EXPECT_EQ(adler32(bytes("Wikipedia")), 0x11E60398u);
    EXPECT_EQ(crc32(bytes("6789"), crc32(bytes("12345"))), 0xCBF43926u);  // Continues a running value
}

TEST(Screenshot, TinyImageHasTheExactLayout) {
    const std::vector<std::uint8_t> rgb{255, 0, 0, 0, 0, 255};  // 2x1: red, blue
    const std::vector<std::uint8_t> png = encode_png_rgb(2, 1, rgb);
    // Signature 8, IHDR 12 + 13, IDAT 12 + (2 zlib header + 5 block header + 7 raw + 4 Adler-32), IEND 12.
    ASSERT_EQ(png.size(), 75u);
    EXPECT_EQ(png[0], 0x89);
    EXPECT_EQ(read_u32(png, 8), 13u);
    EXPECT_EQ(read_u32(png, 16), 2u);  // Width
    EXPECT_EQ(read_u32(png, 20), 1u);  // Height
    EXPECT_EQ(png[24], 8);             // Bit depth
    EXPECT_EQ(png[25], 2);             // RGB
    EXPECT_EQ(read_u32(png, 33), 18u);  // IDAT length
    const std::vector<std::uint8_t> raw{0, 255, 0, 0, 0, 0, 255};
    EXPECT_EQ(read_u32(png, 41 + 2 + 5 + 7), adler32(raw));
    EXPECT_EQ(read_u32(png, png.size() - 4), 0xAE426082u);  // IEND's CRC
}

TEST(Screenshot, LargeImagesSplitIntoStoredBlocks) {
    const int width = 200;
    const int height = 150;  // 150 rows of 601 bytes: 90,150 bytes, two blocks
    const std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width * height * 3), 7);
    const std::vector<std::uint8_t> png = encode_png_rgb(width, height, rgb);
    const std::size_t idat = 33;
    const std::size_t zlib = idat + 8;
    EXPECT_EQ(read_u32(png, idat), 2u + 5u + 65535u + 5u + (90150u - 65535u) + 4u);
    EXPECT_EQ(png[zlib + 2], 0);  // First block, not final
    EXPECT_EQ(png[zlib + 3], 0xFF);
    EXPECT_EQ(png[zlib + 4], 0xFF);
    EXPECT_EQ(png[zlib + 2 + 5 + 65535], 1);  // Final block
    EXPECT_THROW((void)encode_png_rgb(2, 2, rgb), std::invalid_argument);
}
