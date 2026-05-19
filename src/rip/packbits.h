#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gutenrip::rip {

std::vector<std::uint8_t> packBits(const std::uint8_t* data, std::size_t size);

bool unpackBits(const std::vector<std::uint8_t>& bytes,
                std::size_t offset,
                std::size_t expectedSize,
                std::vector<std::uint8_t>* out,
                std::size_t* nextOffset);

} // namespace gutenrip::rip
