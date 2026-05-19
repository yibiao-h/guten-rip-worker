#include "packbits.h"

#include <algorithm>

namespace gutenrip::rip {
namespace {

std::size_t repeatLength(const std::uint8_t* data, std::size_t size, std::size_t offset)
{
    std::size_t count = 1;
    while (offset + count < size && count < 128 && data[offset + count] == data[offset]) {
        ++count;
    }
    return count;
}

} // namespace

std::vector<std::uint8_t> packBits(const std::uint8_t* data, std::size_t size)
{
    std::vector<std::uint8_t> out;
    if (!data || size == 0) {
        return out;
    }
    out.reserve(size + size / 128 + 1);

    std::size_t offset = 0;
    while (offset < size) {
        std::size_t run = repeatLength(data, size, offset);
        if (run >= 3) {
            while (run > 0) {
                const std::size_t chunk = std::min<std::size_t>(run, 128);
                out.push_back(static_cast<std::uint8_t>(1 - static_cast<int>(chunk)));
                out.push_back(data[offset]);
                offset += chunk;
                run -= chunk;
            }
            continue;
        }

        const std::size_t literalStart = offset;
        std::size_t literalEnd = offset;
        while (literalEnd < size && literalEnd - literalStart < 128) {
            run = repeatLength(data, size, literalEnd);
            if (run >= 3) {
                break;
            }
            const std::size_t remainingLiteral = 128 - (literalEnd - literalStart);
            literalEnd += std::min(run, remainingLiteral);
        }

        const std::size_t count = literalEnd - literalStart;
        out.push_back(static_cast<std::uint8_t>(count - 1));
        out.insert(out.end(), data + literalStart, data + literalEnd);
        offset = literalEnd;
    }

    return out;
}

bool unpackBits(const std::vector<std::uint8_t>& bytes,
                std::size_t offset,
                std::size_t expectedSize,
                std::vector<std::uint8_t>* out,
                std::size_t* nextOffset)
{
    if (!out || !nextOffset) {
        return false;
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(expectedSize);
    std::size_t pos = offset;
    while (decoded.size() < expectedSize && pos < bytes.size()) {
        const std::uint8_t control = bytes[pos++];
        if (control < 128) {
            const std::size_t count = static_cast<std::size_t>(control) + 1;
            if (pos + count > bytes.size() || decoded.size() + count > expectedSize) {
                return false;
            }
            decoded.insert(decoded.end(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos + count));
            pos += count;
        } else {
            const std::size_t count = 257u - static_cast<std::size_t>(control);
            if (pos >= bytes.size() || decoded.size() + count > expectedSize) {
                return false;
            }
            decoded.insert(decoded.end(), count, bytes[pos++]);
        }
    }

    if (decoded.size() != expectedSize) {
        return false;
    }

    *out = std::move(decoded);
    *nextOffset = pos;
    return true;
}

} // namespace gutenrip::rip
