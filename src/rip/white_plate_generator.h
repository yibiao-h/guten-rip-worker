#pragma once

#include <cstdint>
#include <vector>

namespace gutenrip::rip {

enum class WhitePlateMode {
    FillAll,
    AnyColoredPixel,
    GradientNegative,
    GradientPositive,
    Alpha
};

struct WhitePlateOptions {
    WhitePlateMode mode = WhitePlateMode::AnyColoredPixel;
    std::uint8_t colorThreshold = 1;
    std::uint8_t blackThreshold = 255;
    bool removeBlack = false;
    bool removeWhiteBackground = false;
    std::uint8_t whiteBackgroundThreshold = 245;
    int chokePixels = 0;
    int expandPixels = 0;
};

class WhitePlateGenerator {
public:
    std::vector<std::uint8_t> generateFromRgba(const std::vector<std::uint8_t>& rgba,
                                               int width,
                                               int height,
                                               const WhitePlateOptions& options) const;

private:
    static std::vector<std::uint8_t> erode(const std::vector<std::uint8_t>& src,
                                           int width,
                                           int height,
                                           int radius);
    static std::vector<std::uint8_t> dilate(const std::vector<std::uint8_t>& src,
                                            int width,
                                            int height,
                                            int radius);
};

} // namespace gutenrip::rip
