#pragma once

#include <cstdint>

namespace araya {

constexpr std::uint32_t version_major = 0;
constexpr std::uint32_t version_minor = 1;
constexpr std::uint32_t version_patch = 0;

const char* version_string();

}  // namespace araya
