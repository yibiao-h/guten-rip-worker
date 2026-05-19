#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gutenrip::rip {

enum class RasterCommandKind {
    EscDot,
    EscI
};

struct RasterBlock {
    RasterCommandKind kind = RasterCommandKind::EscI;
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> data;
    int color = 0;
    int compression = 0;
    int bitDepth = 1;
    int bytesPerLine = 0;
    int lineCount = 0;
    int widthDots = 0;
    bool hasTrailingCarriageReturn = false;
};

struct PrnSegment {
    enum class Kind {
        Raw,
        Raster
    };

    Kind kind = Kind::Raw;
    std::vector<std::uint8_t> raw;
    RasterBlock raster;
};

struct PrnDocument {
    std::vector<PrnSegment> segments;
};

struct ParseResult {
    PrnDocument document;
    std::vector<std::string> warnings;
};

class EscpParser {
public:
    ParseResult parse(const std::vector<std::uint8_t>& bytes) const;

private:
    static bool parseEscI(const std::vector<std::uint8_t>& bytes,
                          std::size_t offset,
                          RasterBlock* block,
                          std::size_t* nextOffset,
                          std::string* warning);
    static bool parseEscDot(const std::vector<std::uint8_t>& bytes,
                            std::size_t offset,
                            int currentColor,
                            RasterBlock* block,
                            std::size_t* nextOffset,
                            std::string* warning);
};

std::vector<std::uint8_t> serialize(const PrnDocument& document);
std::vector<std::uint8_t> serialize(const RasterBlock& block);

} // namespace gutenrip::rip
