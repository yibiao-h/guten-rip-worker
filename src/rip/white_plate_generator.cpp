#include "white_plate_generator.h"

#include <algorithm>
#include <cmath>

namespace gutenrip::rip {
namespace {

std::uint8_t luminance(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint8_t>(
        std::clamp<int>(static_cast<int>(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b)), 0, 255));
}

std::uint8_t scaleByte(std::uint8_t value, std::uint8_t scale)
{
    return static_cast<std::uint8_t>((static_cast<int>(value) * static_cast<int>(scale) + 127) / 255);
}

std::uint8_t whiteBackgroundCoverage(std::uint8_t r,
                                     std::uint8_t g,
                                     std::uint8_t b,
                                     std::uint8_t threshold)
{
    const int minChannel = std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    if (minChannel >= static_cast<int>(threshold)) {
        return 0;
    }

    const int feather = std::max(8, 255 - static_cast<int>(threshold));
    const int distance = static_cast<int>(threshold) - minChannel;
    if (distance >= feather) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::clamp((distance * 255 + feather / 2) / feather, 0, 255));
}

} // namespace

std::vector<std::uint8_t> WhitePlateGenerator::generateFromRgba(
    const std::vector<std::uint8_t>& rgba,
    int width,
    int height,
    const WhitePlateOptions& options) const
{
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width * height * 4)) {
        return {};
    }

    std::vector<std::uint8_t> plate(static_cast<std::size_t>(width * height), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y * width + x);
            const std::size_t offset = pixel * 4;
            const auto r = rgba[offset + 0];
            const auto g = rgba[offset + 1];
            const auto b = rgba[offset + 2];
            const auto a = rgba[offset + 3];
            const auto luma = luminance(r, g, b);

            if (options.removeBlack && luma <= options.blackThreshold) {
                plate[pixel] = 0;
                continue;
            }
            if (a == 0) {
                continue;
            }
            if (options.mode == WhitePlateMode::FillAll) {
                plate[pixel] = 255;
                continue;
            }
            const std::uint8_t coverage = options.removeWhiteBackground
                ? scaleByte(a, whiteBackgroundCoverage(r, g, b, options.whiteBackgroundThreshold))
                : a;
            if (coverage == 0) {
                continue;
            }

            switch (options.mode) {
            case WhitePlateMode::AnyColoredPixel:
                plate[pixel] = coverage;
                break;
            case WhitePlateMode::GradientNegative:
                plate[pixel] = scaleByte(static_cast<std::uint8_t>(255 - luma), coverage);
                break;
            case WhitePlateMode::GradientPositive:
                plate[pixel] = scaleByte(luma, coverage);
                break;
            case WhitePlateMode::Alpha:
                plate[pixel] = coverage;
                break;
            case WhitePlateMode::FillAll:
                plate[pixel] = 255;
                break;
            }
        }
    }

    if (options.chokePixels > 0) {
        plate = erode(plate, width, height, options.chokePixels);
    }
    if (options.expandPixels > 0) {
        plate = dilate(plate, width, height, options.expandPixels);
    }
    return plate;
}

std::vector<std::uint8_t> WhitePlateGenerator::erode(const std::vector<std::uint8_t>& src,
                                                     int width,
                                                     int height,
                                                     int radius)
{
    std::vector<std::uint8_t> out(src.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t minValue = 255;
            for (int yy = std::max(0, y - radius); yy <= std::min(height - 1, y + radius); ++yy) {
                for (int xx = std::max(0, x - radius); xx <= std::min(width - 1, x + radius); ++xx) {
                    minValue = std::min(minValue, src[static_cast<std::size_t>(yy * width + xx)]);
                }
            }
            out[static_cast<std::size_t>(y * width + x)] = minValue;
        }
    }
    return out;
}

std::vector<std::uint8_t> WhitePlateGenerator::dilate(const std::vector<std::uint8_t>& src,
                                                      int width,
                                                      int height,
                                                      int radius)
{
    std::vector<std::uint8_t> out(src.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t maxValue = 0;
            for (int yy = std::max(0, y - radius); yy <= std::min(height - 1, y + radius); ++yy) {
                for (int xx = std::max(0, x - radius); xx <= std::min(width - 1, x + radius); ++xx) {
                    maxValue = std::max(maxValue, src[static_cast<std::size_t>(yy * width + xx)]);
                }
            }
            out[static_cast<std::size_t>(y * width + x)] = maxValue;
        }
    }
    return out;
}

} // namespace gutenrip::rip
