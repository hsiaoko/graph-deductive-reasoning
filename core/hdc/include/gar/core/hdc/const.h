#pragma once

// HDC dimension constants. All dimensions are multiples of 64 so
// hypervectors bit-pack exactly into uint64 words; the presets are powers
// of two, which keeps Permute (cyclic shift) on the word-level fast path.

#include <array>
#include <cstdint>

namespace gar::core::hdc {

inline constexpr std::array<std::uint32_t, 8> kHdcDimPresets = {
    128, 256, 512, 1024, 2048, 4096, 8192, 16384};

// Default dimension. 10000 (the classic Kanerva dimension) is not a power
// of two and not a multiple of 64; the nearest preset down, 8192, is the
// default: sqrt(8192) ≈ 90.5 covers evidence bundles up to m+N = 90 (the
// App I reliability condition) at 1 KB per vector, and the next preset
// (16384) is one ChooseDim step away for larger bundles.
inline constexpr std::uint32_t kHdcDefaultDim = 8192;

// Smallest preset satisfying the App I reliability bound (m+N)^2 <= D for
// `max_bundle_size` = m+N; clamps to the largest preset when the bound
// exceeds it (the caller warns about the reduced reliability margin).
inline std::uint32_t ChooseDim(std::uint64_t max_bundle_size) {
  const std::uint64_t need = max_bundle_size * max_bundle_size;
  for (const std::uint32_t p : kHdcDimPresets) {
    if (need <= p) return p;
  }
  return kHdcDimPresets.back();
}

}  // namespace gar::core::hdc
