#pragma once

#include <cstdint>

namespace gar {
namespace hdc {

// Dense bipolar-vector kernels used by the agent's value model. The online
// accumulator is int16; binding is coordinate-wise multiplication.
void AddBipolar(std::int16_t* accumulator, const std::int8_t* vector,
                std::uint32_t dim);
void AddBoundBipolar(std::int16_t* accumulator, const std::int8_t* a,
                     const std::int8_t* b, std::uint32_t dim);
double DotInt16Double(const std::int16_t* vector, const double* weights,
                      std::uint32_t dim);

// Packed bit convention matches Hypervector XOR binding: 1 = -1, 0 = +1.
// weight_sum is the sum of all dim weights. AVX-512 masks eight weights per
// instruction; the scalar fallback sums negative coordinates.
double DotPackedBipolar(const std::uint64_t* words, const double* weights,
                        std::uint32_t dim, double weight_sum);

}  // namespace hdc
}  // namespace gar
