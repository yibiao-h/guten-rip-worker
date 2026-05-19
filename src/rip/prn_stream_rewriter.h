#pragma once

#include "prn_compositor.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gutenrip::rip {

class PrnStreamRewriter
{
public:
    explicit PrnStreamRewriter(ComposeOptions options = {});

    std::vector<std::uint8_t> append(const std::uint8_t* data, std::size_t size);
    std::vector<std::uint8_t> append(const std::vector<std::uint8_t>& data);
    std::vector<std::uint8_t> finish();

private:
    enum class CommandState {
        Complete,
        Incomplete,
        Unsupported
    };

    CommandState tryTakeCommand(std::size_t offset,
                                RasterBlock* block,
                                std::size_t* nextOffset,
                                bool final) const;
    std::vector<std::uint8_t> drain(bool final);
    RasterBlock rewriteRaster(const RasterBlock& block) const;

    ComposeOptions m_options;
    std::vector<std::uint8_t> m_pending;
    int m_currentColor = 0;
};

} // namespace gutenrip::rip
