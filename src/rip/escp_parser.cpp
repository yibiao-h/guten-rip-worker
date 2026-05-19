#include "escp_parser.h"

#include "packbits.h"

#include <algorithm>

namespace gutenrip::rip {
namespace {

constexpr std::uint8_t kEsc = 0x1b;

int readLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<int>(bytes[offset]) |
           (static_cast<int>(bytes[offset + 1]) << 8);
}

void appendRaw(PrnDocument* document,
               const std::vector<std::uint8_t>& bytes,
               std::size_t begin,
               std::size_t end)
{
    if (begin >= end) {
        return;
    }
    if (!document->segments.empty() &&
        document->segments.back().kind == PrnSegment::Kind::Raw) {
        auto& raw = document->segments.back().raw;
        raw.insert(raw.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                   bytes.begin() + static_cast<std::ptrdiff_t>(end));
        return;
    }

    PrnSegment segment;
    segment.kind = PrnSegment::Kind::Raw;
    segment.raw.assign(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end));
    document->segments.push_back(std::move(segment));
}

} // namespace

ParseResult EscpParser::parse(const std::vector<std::uint8_t>& bytes) const
{
    ParseResult result;
    std::size_t rawStart = 0;
    int currentColor = 0;

    for (std::size_t i = 0; i < bytes.size();) {
        if (bytes[i] != kEsc || i + 1 >= bytes.size()) {
            ++i;
            continue;
        }

        const std::uint8_t command = bytes[i + 1];
        if (command == 'r' && i + 2 < bytes.size()) {
            currentColor = bytes[i + 2];
            i += 3;
            continue;
        }

        RasterBlock block;
        std::size_t nextOffset = i;
        std::string warning;
        const bool parsedRaster =
            (command == 'i' && parseEscI(bytes, i, &block, &nextOffset, &warning)) ||
            (command == '.' && parseEscDot(bytes, i, currentColor, &block, &nextOffset, &warning));

        if (!warning.empty()) {
            result.warnings.push_back(warning);
        }

        if (parsedRaster) {
            appendRaw(&result.document, bytes, rawStart, i);
            PrnSegment segment;
            segment.kind = PrnSegment::Kind::Raster;
            segment.raster = std::move(block);
            result.document.segments.push_back(std::move(segment));
            i = nextOffset;
            rawStart = i;
            continue;
        }

        if (command == '(' && i + 4 < bytes.size()) {
            const int payloadBytes = readLe16(bytes, i + 3);
            const std::size_t totalBytes = 5u + static_cast<std::size_t>(std::max(payloadBytes, 0));
            if (i + totalBytes <= bytes.size()) {
                i += totalBytes;
                continue;
            }
        }

        i += 2;
    }

    appendRaw(&result.document, bytes, rawStart, bytes.size());
    return result;
}

bool EscpParser::parseEscI(const std::vector<std::uint8_t>& bytes,
                           std::size_t offset,
                           RasterBlock* block,
                           std::size_t* nextOffset,
                           std::string* warning)
{
    if (offset + 9 > bytes.size()) {
        return false;
    }

    const int nwidth = readLe16(bytes, offset + 5);
    const int nlines = readLe16(bytes, offset + 7);
    const int compression = bytes[offset + 3];
    const std::size_t dataBytes = static_cast<std::size_t>(nwidth) * static_cast<std::size_t>(nlines);

    if (compression != 0 && compression != 1) {
        if (warning) {
            *warning = "Unsupported ESC i raster compression mode; the compositor expects uncompressed or PackBits RLE PRN.";
        }
        return false;
    }
    if (compression == 0 && offset + 9 + dataBytes > bytes.size()) {
        return false;
    }

    block->kind = RasterCommandKind::EscI;
    block->header.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + 9));
    if (compression == 0) {
        block->data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 9),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + 9 + dataBytes));
        *nextOffset = offset + 9 + dataBytes;
    } else {
        std::size_t compressedEnd = offset + 9;
        if (!unpackBits(bytes, offset + 9, dataBytes, &block->data, &compressedEnd)) {
            return false;
        }
        block->header[3] = 0;
        *nextOffset = compressedEnd;
    }
    if (*nextOffset < bytes.size() && bytes[*nextOffset] == '\r') {
        block->hasTrailingCarriageReturn = true;
        ++(*nextOffset);
    }
    block->color = bytes[offset + 2];
    block->compression = 0;
    block->bitDepth = bytes[offset + 4];
    block->bytesPerLine = nwidth;
    block->lineCount = nlines;
    block->widthDots = nwidth * 8 / std::max(block->bitDepth, 1);
    return true;
}

bool EscpParser::parseEscDot(const std::vector<std::uint8_t>& bytes,
                             std::size_t offset,
                             int currentColor,
                             RasterBlock* block,
                             std::size_t* nextOffset,
                             std::string* warning)
{
    if (offset + 8 > bytes.size()) {
        return false;
    }

    const int compression = bytes[offset + 2];
    const int nlines = bytes[offset + 5];
    const int widthDots = readLe16(bytes, offset + 6);
    const int bytesPerLine = (widthDots + 7) / 8;
    const std::size_t dataBytes =
        compression == 0 ? static_cast<std::size_t>(bytesPerLine) * static_cast<std::size_t>(nlines) : 0;

    if (compression != 0) {
        if (warning) {
            *warning = "Compressed ESC . raster blocks are preserved as raw bytes; the compositor expects uncompressed intermediate PRN.";
        }
        return false;
    }
    if (offset + 8 + dataBytes > bytes.size()) {
        return false;
    }

    block->kind = RasterCommandKind::EscDot;
    block->header.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8));
    block->data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8 + dataBytes));
    block->color = currentColor;
    block->compression = compression;
    block->bitDepth = 1;
    block->bytesPerLine = bytesPerLine;
    block->lineCount = nlines;
    block->widthDots = widthDots;
    *nextOffset = offset + 8 + dataBytes;
    return true;
}

std::vector<std::uint8_t> serialize(const RasterBlock& block)
{
    std::vector<std::uint8_t> out = block.header;
    out.insert(out.end(), block.data.begin(), block.data.end());
    if (block.kind == RasterCommandKind::EscI) {
        out.push_back('\r');
    }
    return out;
}

std::vector<std::uint8_t> serialize(const PrnDocument& document)
{
    std::vector<std::uint8_t> out;
    for (const auto& segment : document.segments) {
        if (segment.kind == PrnSegment::Kind::Raw) {
            out.insert(out.end(), segment.raw.begin(), segment.raw.end());
        } else {
            const auto raster = serialize(segment.raster);
            out.insert(out.end(), raster.begin(), raster.end());
        }
    }
    return out;
}

} // namespace gutenrip::rip
