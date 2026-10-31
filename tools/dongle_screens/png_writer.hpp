#pragma once

// Writes an 8-bit RGB image as a PNG file.

#include <cstdint>
#include <string>
#include <vector>

bool write_png(const std::string &path, int width, int height,
               const std::vector<std::uint8_t> &rgb);
