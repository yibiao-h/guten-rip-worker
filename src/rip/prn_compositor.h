#pragma once

#include "escp_parser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gutenrip::rip {

enum class LayerOrder {
    WhiteUnderColor,
    WhiteOverColor
};

enum class NozzleHalf {
    Front,
    Back
};

struct ComposeOptions {
    LayerOrder layerOrder = LayerOrder::WhiteUnderColor;
    bool frontHalfPrintsFirst = false;
    std::uint8_t sourceLightCyanWhiteColor = 0x12;
    std::uint8_t sourceLightMagentaWhiteColor = 0x11;
    std::uint8_t lightCyanWhiteColor = 0x12;
    std::uint8_t lightMagentaWhiteColor = 0x11;
    int nozzles = 86;
    int nozzleSeparation = 2;
    int baseSeparation = 360;
};

struct ComposeResult {
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> warnings;
};

class PrnCompositor {
public:
    ComposeResult compose90NozzleColorAndWhite(const std::vector<std::uint8_t>& colorPrn,
                                                const std::vector<std::uint8_t>& whitePrn,
                                                const ComposeOptions& options) const;

    ComposeResult padBaseChannelsInSinglePrn(const std::vector<std::uint8_t>& prn,
                                             const ComposeOptions& options) const;

private:
    static RasterBlock padTo180Nozzles(const RasterBlock& source,
                                       NozzleHalf half,
                                       std::uint8_t outputColor,
                                       const ComposeOptions& options);
    static void setRasterColor(RasterBlock* block, std::uint8_t outputColor);
    static void setRasterLineCount(RasterBlock* block, int lines);
};

std::vector<std::uint8_t> makeSyntheticEscIPrn(std::uint8_t color,
                                               int bytesPerLine,
                                               int lineCount,
                                               std::uint8_t fill);

} // namespace gutenrip::rip
