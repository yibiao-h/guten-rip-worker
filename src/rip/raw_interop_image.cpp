#include "raw_interop_image.h"

#include <array>
#include <fstream>
#include <limits>
#include <utility>

namespace gutenrip::rip {
namespace {

constexpr std::array<char, 8> kMagic{{'G', 'R', 'R', 'A', 'W', '0', '0', '1'}};
constexpr int kMaxChannels = 40;

void setError(std::string* error, const std::string& message)
{
    if (error) {
        *error = message;
    }
}

void writeU32(std::ofstream& out, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu),
        static_cast<char>((value >> 24) & 0xffu),
    };
    out.write(bytes, sizeof(bytes));
}

std::uint32_t readU32(std::ifstream& in)
{
    unsigned char bytes[4] = {};
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool validateShape(int width, int height, int channels, std::size_t* expectedSize, std::string* error)
{
    if (width <= 0 || height <= 0 || channels <= 0 || channels > kMaxChannels) {
        setError(error, "Invalid raw interop image shape.");
        return false;
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto c = static_cast<std::size_t>(channels);
    if (w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > std::numeric_limits<std::size_t>::max() / c) {
        setError(error, "Raw interop image size overflow.");
        return false;
    }

    if (expectedSize) {
        *expectedSize = w * h * c;
    }
    return true;
}

} // namespace

bool writeRawInteropImage(const std::string& path,
                          const RawInteropImage& image,
                          std::string* error)
{
    std::size_t expectedSize = 0;
    if (!validateShape(image.width, image.height, image.channels, &expectedSize, error)) {
        return false;
    }
    if (image.pixels.size() != expectedSize) {
        setError(error, "Raw interop image payload size does not match its shape.");
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        setError(error, "Unable to create raw interop image: " + path);
        return false;
    }

    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    writeU32(out, static_cast<std::uint32_t>(image.width));
    writeU32(out, static_cast<std::uint32_t>(image.height));
    writeU32(out, static_cast<std::uint32_t>(image.channels));
    writeU32(out, 0u);
    out.write(reinterpret_cast<const char*>(image.pixels.data()),
              static_cast<std::streamsize>(image.pixels.size()));
    if (!out) {
        setError(error, "Unable to write raw interop image payload: " + path);
        return false;
    }
    return true;
}

bool readRawInteropImage(const std::string& path,
                         RawInteropImage* image,
                         std::string* error)
{
    if (!image) {
        setError(error, "Raw interop image output pointer is null.");
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        setError(error, "Unable to open raw interop image: " + path);
        return false;
    }

    std::array<char, kMagic.size()> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kMagic) {
        setError(error, "Unsupported raw interop image format: " + path);
        return false;
    }

    const auto width = static_cast<int>(readU32(in));
    const auto height = static_cast<int>(readU32(in));
    const auto channels = static_cast<int>(readU32(in));
    (void)readU32(in);

    std::size_t expectedSize = 0;
    if (!validateShape(width, height, channels, &expectedSize, error)) {
        return false;
    }

    RawInteropImage loaded;
    loaded.width = width;
    loaded.height = height;
    loaded.channels = channels;
    loaded.pixels.resize(expectedSize);
    in.read(reinterpret_cast<char*>(loaded.pixels.data()),
            static_cast<std::streamsize>(loaded.pixels.size()));
    if (!in) {
        setError(error, "Raw interop image payload is truncated: " + path);
        return false;
    }

    *image = std::move(loaded);
    return true;
}

} // namespace gutenrip::rip
