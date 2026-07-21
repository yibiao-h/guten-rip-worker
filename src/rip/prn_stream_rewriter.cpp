#include "prn_stream_rewriter.h"

#include "packbits.h"

#include <algorithm>
#include <utility>

namespace gutenrip::rip {
namespace {

constexpr std::uint8_t kEsc = 0x1b;
constexpr int kPaddingAnchorColor = 0x09;

int readLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<int>(bytes[offset]) |
           (static_cast<int>(bytes[offset + 1]) << 8);
}

int basePaddingLines(const ComposeOptions& options)
{
    if (options.nozzles <= 0 || options.nozzleSeparation <= 0 ||
        options.baseSeparation <= 0) {
        return 94;
    }
    return std::max(0, (options.baseSeparation / options.nozzleSeparation) - options.nozzles);
}

bool isColorSourceAnchorColor(int color)
{
    return color == kPaddingAnchorColor;
}

bool isWhiteOutputColor(int color, const ComposeOptions& options)
{
    return color == options.lightCyanWhiteColor ||
           color == options.lightMagentaWhiteColor;
}

bool isWhiteSourceColor(int color, const ComposeOptions& options)
{
    return color == options.sourceLightCyanWhiteColor ||
           color == options.sourceLightMagentaWhiteColor ||
           isWhiteOutputColor(color, options);
}

bool isBaseChannelForLayer(int color, const ComposeOptions& options)
{
    if (isColorSourceAnchorColor(color)) {
        return false;
    }
    if (!options.baseChannelColors.empty()) {
        return std::find(options.baseChannelColors.begin(),
                         options.baseChannelColors.end(),
                         color) != options.baseChannelColors.end();
    }
    const bool whiteChannel = isWhiteSourceColor(color, options);
    return options.layerOrder == LayerOrder::WhiteUnderColor ? whiteChannel : !whiteChannel;
}

int channelCorrectionLines(int color, const ChannelOffsetCorrectionOptions& options)
{
    if (!options.channelCorrections.empty()) {
        const auto it = std::find_if(
            options.channelCorrections.begin(),
            options.channelCorrections.end(),
            [color](const ChannelOffsetCorrection& correction) {
                return correction.escColor == color;
            });
        return it == options.channelCorrections.end()
            ? 0
            : std::max(0, it->leadingBlankRows);
    }
    return isBaseChannelForLayer(color, options) ? basePaddingLines(options) : 0;
}

void setRasterLineCount(RasterBlock* block, int lines)
{
    if (block->kind == RasterCommandKind::EscI && block->header.size() >= 9) {
        block->header[7] = static_cast<std::uint8_t>(lines & 0xff);
        block->header[8] = static_cast<std::uint8_t>((lines >> 8) & 0xff);
    } else if (block->kind == RasterCommandKind::EscDot && block->header.size() >= 6) {
        block->header[5] = static_cast<std::uint8_t>(std::clamp(lines, 0, 255));
    }
}

RasterBlock compressEscI(const RasterBlock& source)
{
    if (source.kind != RasterCommandKind::EscI || source.compression != 0 || source.header.size() < 9) {
        return source;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(source.bytesPerLine);
    const std::size_t rows = static_cast<std::size_t>(source.lineCount);
    if (rowBytes == 0 || rows == 0 || source.data.size() != rowBytes * rows) {
        return source;
    }

    RasterBlock out = source;
    out.compression = 1;
    out.header[3] = 1;
    out.data.clear();
    for (std::size_t row = 0; row < rows; ++row) {
        const std::uint8_t* rowData = source.data.data() + row * rowBytes;
        std::vector<std::uint8_t> packed = packBits(rowData, rowBytes);
        out.data.insert(out.data.end(), packed.begin(), packed.end());
    }
    return out;
}

} // namespace

ChannelOffsetCorrector::ChannelOffsetCorrector(ChannelOffsetCorrectionOptions options)
    : m_options(std::move(options))
{
}

std::vector<std::uint8_t> ChannelOffsetCorrector::append(const std::vector<std::uint8_t>& data)
{
    return append(data.data(), data.size());
}

std::vector<std::uint8_t> ChannelOffsetCorrector::append(const std::uint8_t* data, std::size_t size)
{
    if (data && size > 0) {
        m_pending.insert(m_pending.end(), data, data + size);
    }
    return drain(false);
}

std::vector<std::uint8_t> ChannelOffsetCorrector::finish()
{
    std::vector<std::uint8_t> out = drain(true);
    out.insert(out.end(), m_pending.begin(), m_pending.end());
    m_pending.clear();
    return out;
}

std::vector<std::uint8_t> ChannelOffsetCorrector::drain(bool final)
{
    std::vector<std::uint8_t> out;
    for (;;) {
        const auto esc = std::find(m_pending.begin(), m_pending.end(), kEsc);
        if (esc == m_pending.end()) {
            if (m_pending.size() > 8u) {
                const std::size_t safeBytes = m_pending.size() - 8u;
                out.insert(out.end(), m_pending.begin(),
                           m_pending.begin() + static_cast<std::ptrdiff_t>(safeBytes));
                m_pending.erase(m_pending.begin(),
                                m_pending.begin() + static_cast<std::ptrdiff_t>(safeBytes));
            }
            return out;
        }

        const std::size_t offset = static_cast<std::size_t>(esc - m_pending.begin());
        if (offset > 0) {
            out.insert(out.end(), m_pending.begin(), esc);
            m_pending.erase(m_pending.begin(), esc);
            continue;
        }

        if (m_pending.size() < 2u) {
            return out;
        }

        if (m_pending[1] == 'r') {
            if (m_pending.size() < 3u) {
                return out;
            }
            m_currentColor = m_pending[2];
            out.insert(out.end(), m_pending.begin(), m_pending.begin() + 3);
            m_pending.erase(m_pending.begin(), m_pending.begin() + 3);
            continue;
        }

        RasterBlock block;
        std::size_t nextOffset = 0;
        const CommandState state = tryTakeCommand(0, &block, &nextOffset, final);
        if (state == CommandState::Incomplete) {
            return out;
        }
        if (state == CommandState::Complete) {
            block = rewriteRaster(block);
            block = compressEscI(block);
            const auto bytes = serialize(block);
            out.insert(out.end(), bytes.begin(), bytes.end());
            m_pending.erase(m_pending.begin(),
                            m_pending.begin() + static_cast<std::ptrdiff_t>(nextOffset));
            continue;
        }

        out.push_back(m_pending.front());
        m_pending.erase(m_pending.begin());
    }
}

ChannelOffsetCorrector::CommandState ChannelOffsetCorrector::tryTakeCommand(std::size_t offset,
                                                                  RasterBlock* block,
                                                                  std::size_t* nextOffset,
                                                                  bool final) const
{
    if (offset + 1u >= m_pending.size() || m_pending[offset] != kEsc) {
        return CommandState::Unsupported;
    }

    const std::uint8_t command = m_pending[offset + 1u];
    if (command == 'i') {
        if (offset + 9u > m_pending.size()) {
            return CommandState::Incomplete;
        }
        const int nwidth = readLe16(m_pending, offset + 5u);
        const int nlines = readLe16(m_pending, offset + 7u);
        const int compression = m_pending[offset + 3u];
        const std::size_t dataBytes =
            static_cast<std::size_t>(nwidth) * static_cast<std::size_t>(nlines);
        if (compression != 0 && compression != 1) {
            return CommandState::Unsupported;
        }
        block->kind = RasterCommandKind::EscI;
        block->header.assign(m_pending.begin() + static_cast<std::ptrdiff_t>(offset),
                             m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 9u));
        if (compression == 0) {
            if (offset + 9u + dataBytes > m_pending.size()) {
                return CommandState::Incomplete;
            }
            block->data.assign(m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 9u),
                               m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 9u + dataBytes));
            *nextOffset = offset + 9u + dataBytes;
        } else {
            std::size_t compressedEnd = offset + 9u;
            if (!unpackBits(m_pending, offset + 9u, dataBytes, &block->data, &compressedEnd)) {
                return CommandState::Incomplete;
            }
            block->header[3] = 0;
            *nextOffset = compressedEnd;
        }
        if (!final && *nextOffset == m_pending.size()) {
            return CommandState::Incomplete;
        }
        if (*nextOffset < m_pending.size() && m_pending[*nextOffset] == '\r') {
            block->hasTrailingCarriageReturn = true;
            ++(*nextOffset);
        }
        block->color = m_pending[offset + 2u];
        block->compression = 0;
        block->bitDepth = m_pending[offset + 4u];
        block->bytesPerLine = nwidth;
        block->lineCount = nlines;
        block->widthDots = nwidth * 8 / std::max(block->bitDepth, 1);
        return CommandState::Complete;
    }

    if (command == '.') {
        if (offset + 8u > m_pending.size()) {
            return CommandState::Incomplete;
        }
        const int compression = m_pending[offset + 2u];
        if (compression != 0) {
            return CommandState::Unsupported;
        }
        const int nlines = m_pending[offset + 5u];
        const int widthDots = readLe16(m_pending, offset + 6u);
        const int bytesPerLine = (widthDots + 7) / 8;
        const std::size_t dataBytes =
            static_cast<std::size_t>(bytesPerLine) * static_cast<std::size_t>(nlines);
        if (offset + 8u + dataBytes > m_pending.size()) {
            return CommandState::Incomplete;
        }
        block->kind = RasterCommandKind::EscDot;
        block->header.assign(m_pending.begin() + static_cast<std::ptrdiff_t>(offset),
                             m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 8u));
        block->data.assign(m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 8u),
                           m_pending.begin() + static_cast<std::ptrdiff_t>(offset + 8u + dataBytes));
        block->color = m_currentColor;
        block->compression = 0;
        block->bitDepth = 1;
        block->bytesPerLine = bytesPerLine;
        block->lineCount = nlines;
        block->widthDots = widthDots;
        *nextOffset = offset + 8u + dataBytes;
        return CommandState::Complete;
    }

    return CommandState::Unsupported;
}

RasterBlock ChannelOffsetCorrector::rewriteRaster(const RasterBlock& block) const
{
    const int paddingLines = channelCorrectionLines(block.color, m_options);
    if (paddingLines <= 0) {
        return block;
    }

    RasterBlock out = block;
    const int outputLineCount = block.lineCount + paddingLines;
    setRasterLineCount(&out, outputLineCount);

    const std::size_t offsetBytes =
        static_cast<std::size_t>(block.bytesPerLine) * static_cast<std::size_t>(paddingLines);
    out.data.clear();
    out.data.reserve(offsetBytes + block.data.size());
    out.data.insert(out.data.end(), offsetBytes, 0);
    out.data.insert(out.data.end(), block.data.begin(), block.data.end());
    out.lineCount = outputLineCount;
    return out;
}

} // namespace gutenrip::rip
