#include "gar/hdc/value_kernels.h"

#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

namespace gar {
namespace hdc {

void AddBipolar(std::int16_t* accumulator, const std::int8_t* vector,
                std::uint32_t dim) {
  std::uint32_t d = 0;
#if defined(__AVX2__)
  for (; d + 16 <= dim; d += 16) {
    const __m128i packed =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(vector + d));
    const __m256i values = _mm256_cvtepi8_epi16(packed);
    const __m256i current = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(accumulator + d));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator + d),
                        _mm256_add_epi16(current, values));
  }
#endif
  for (; d < dim; ++d) accumulator[d] += vector[d];
}

void AddBoundBipolar(std::int16_t* accumulator, const std::int8_t* a,
                     const std::int8_t* b, std::uint32_t dim) {
  std::uint32_t d = 0;
#if defined(__AVX2__)
  for (; d + 16 <= dim; d += 16) {
    const __m128i pa =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + d));
    const __m128i pb =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + d));
    const __m256i bound =
        _mm256_mullo_epi16(_mm256_cvtepi8_epi16(pa), _mm256_cvtepi8_epi16(pb));
    const __m256i current = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(accumulator + d));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator + d),
                        _mm256_add_epi16(current, bound));
  }
#endif
  for (; d < dim; ++d) accumulator[d] += a[d] * b[d];
}

double DotInt16Double(const std::int16_t* vector, const double* weights,
                      std::uint32_t dim) {
  std::uint32_t d = 0;
  double sum = 0.0;
#if defined(__AVX2__)
  __m256d lanes = _mm256_setzero_pd();
  for (; d + 4 <= dim; d += 4) {
    const __m128i packed =
        _mm_loadl_epi64(reinterpret_cast<const __m128i*>(vector + d));
    const __m128i ints = _mm_cvtepi16_epi32(packed);
    const __m256d values = _mm256_cvtepi32_pd(ints);
    const __m256d w = _mm256_loadu_pd(weights + d);
    lanes = _mm256_add_pd(lanes, _mm256_mul_pd(values, w));
  }
  alignas(32) double partial[4];
  _mm256_store_pd(partial, lanes);
  sum = (partial[0] + partial[1]) + (partial[2] + partial[3]);
#endif
  for (; d < dim; ++d) sum += vector[d] * weights[d];
  return sum;
}

double DotPackedBipolar(const std::uint64_t* words, const double* weights,
                        std::uint32_t dim, double weight_sum) {
  std::uint32_t d = 0;
  double negative_sum = 0.0;
#if defined(__AVX512F__)
  __m512d lanes = _mm512_setzero_pd();
  for (; d + 8 <= dim; d += 8) {
    const auto mask = static_cast<__mmask8>(
        (words[d >> 6] >> (d & 63)) & std::uint64_t{0xff});
    lanes = _mm512_add_pd(lanes, _mm512_maskz_loadu_pd(mask, weights + d));
  }
  negative_sum = _mm512_reduce_add_pd(lanes);
#endif
  for (; d < dim; ++d) {
    if ((words[d >> 6] >> (d & 63)) & 1) negative_sum += weights[d];
  }
  return weight_sum - 2.0 * negative_sum;
}

}  // namespace hdc
}  // namespace gar
