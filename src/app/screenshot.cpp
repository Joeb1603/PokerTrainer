#include "app/screenshot.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace poker {
namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5], std::span<const std::uint8_t> data) {
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_u32(out, crc32(std::span<const std::uint8_t>(out).subspan(start)));
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t value) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t c = ~value;
    for (const std::uint8_t byte : data) c = table[(c ^ byte) & 0xFFu] ^ (c >> 8);
    return ~c;
}

std::uint32_t adler32(std::span<const std::uint8_t> data, std::uint32_t value) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = value & 0xFFFFu;
    std::uint32_t b = value >> 16;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

std::vector<std::uint8_t> encode_png_rgb(int width, int height, std::span<const std::uint8_t> rgb) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("encode_png_rgb: empty image");
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    if (rgb.size() != row * static_cast<std::size_t>(height)) throw std::invalid_argument("encode_png_rgb: size mismatch");

    // Each row starts with filter type 0 (none).
    std::vector<std::uint8_t> raw;
    raw.reserve((row + 1) * static_cast<std::size_t>(height));
    for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y * row),
                   rgb.begin() + static_cast<std::ptrdiff_t>((y + 1) * row));
    }

    // zlib: header (deflate, 32K window, no preset dictionary), stored blocks, Adler-32.
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    constexpr std::size_t kBlock = 65535;
    for (std::size_t pos = 0; pos < raw.size(); pos += kBlock) {
        const std::size_t n = std::min(kBlock, raw.size() - pos);
        const bool last = pos + n == raw.size();
        zlib.push_back(last ? 1 : 0);
        const auto len = static_cast<std::uint16_t>(n);
        const auto nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<std::uint8_t>(len & 0xFFu));
        zlib.push_back(static_cast<std::uint8_t>(len >> 8));
        zlib.push_back(static_cast<std::uint8_t>(nlen & 0xFFu));
        zlib.push_back(static_cast<std::uint8_t>(nlen >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    }
    put_u32(zlib, adler32(raw));

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put_u32(header, static_cast<std::uint32_t>(width));
    put_u32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, deflate, no filter choice, no interlace
    put_chunk(out, "IHDR", header);
    put_chunk(out, "IDAT", zlib);
    put_chunk(out, "IEND", {});
    return out;
}

std::string write_png_rgb(const std::filesystem::path& path, int width, int height, std::span<const std::uint8_t> rgb) {
    try {
        const std::vector<std::uint8_t> png = encode_png_rgb(width, height, rgb);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) return "could not write " + path.string();
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

}  // namespace poker
