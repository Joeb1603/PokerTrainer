#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace poker {

// CRC-32 (PNG chunks) and Adler-32 (zlib streams), continued from `value`.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t value = 0);
std::uint32_t adler32(std::span<const std::uint8_t> data, std::uint32_t value = 1);

// A PNG of an 8-bit RGB image, rows top to bottom, with an uncompressed (stored)
// zlib stream: no dependencies, for the developer --screenshot flag.
std::vector<std::uint8_t> encode_png_rgb(int width, int height, std::span<const std::uint8_t> rgb);

// Writes encode_png_rgb's output; returns an error message, or empty on success.
std::string write_png_rgb(const std::filesystem::path& path, int width, int height, std::span<const std::uint8_t> rgb);

}  // namespace poker
