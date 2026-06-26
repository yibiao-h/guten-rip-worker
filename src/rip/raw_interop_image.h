#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gutenrip::rip {

struct RawInteropImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    int bitsPerChannel = 16;
    std::vector<std::uint16_t> pixels;
};

bool writeRawInteropImage(const std::string& path,
                          const RawInteropImage& image,
                          std::string* error = nullptr);

bool readRawInteropImage(const std::string& path,
                         RawInteropImage* image,
                         std::string* error = nullptr);

} // namespace gutenrip::rip
