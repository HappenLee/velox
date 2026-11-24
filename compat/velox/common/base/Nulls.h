#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace facebook::velox {
using VeloxUserError = std::runtime_error;

namespace bits {
constexpr uint64_t nbytes(uint64_t bitCount) {
  return (bitCount + 7) / 8;
}

// The dense non-null decoder does not call the null bitmap functions.
inline int32_t countNonNulls(const uint64_t*, int32_t, int32_t) {
  std::abort();
}

inline bool isBitNull(const uint64_t*, int32_t) {
  std::abort();
}
} // namespace bits
} // namespace facebook::velox

#define VELOX_USER_CHECK_LE(left, right, message) \
  do {                                          \
    if (!((left) <= (right))) {                  \
      throw facebook::velox::VeloxUserError(message); \
    }                                           \
  } while (false)
