#include "gar/core/hdc/dense_vector.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace gar::core::hdc {

namespace {

void RequireSameDim(const DenseVector& a, const DenseVector& b) {
  if (a.dim() != b.dim()) {
    throw std::invalid_argument("DenseVector: dimension mismatch");
  }
}

// float SIMD kernels. Encode (two Adds + a Normalized) and the action/goal
// cosine run once per unique fact and are memory-bandwidth-bound; float halves
// the traffic and doubles the lane count vs double. Function-level target
// attributes + runtime dispatch avoid a global -march flag. Dot accumulates in
// float lanes and widens to double only at the end (cosine/ranking precision is
// unaffected at float accuracy over a 128-dim normalized vector).
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
void AddAvx2(float* dst, const float* src, std::size_t n) {
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(dst + i),
                                            _mm256_loadu_ps(src + i)));
  }
  for (; i < n; ++i) dst[i] += src[i];
}

__attribute__((target("avx512f")))
void AddAvx512(float* dst, const float* src, std::size_t n) {
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    _mm512_storeu_ps(dst + i, _mm512_add_ps(_mm512_loadu_ps(dst + i),
                                            _mm512_loadu_ps(src + i)));
  }
  for (; i < n; ++i) dst[i] += src[i];
}

__attribute__((target("avx2")))
void ScaleAvx2(float* dst, float factor, std::size_t n) {
  const __m256 f = _mm256_set1_ps(factor);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(dst + i), f));
  }
  for (; i < n; ++i) dst[i] *= factor;
}

__attribute__((target("avx512f")))
void ScaleAvx512(float* dst, float factor, std::size_t n) {
  const __m512 f = _mm512_set1_ps(factor);
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    _mm512_storeu_ps(dst + i, _mm512_mul_ps(_mm512_loadu_ps(dst + i), f));
  }
  for (; i < n; ++i) dst[i] *= factor;
}

__attribute__((target("avx2")))
double DotAvx2(const float* a, const float* b, std::size_t n) {
  __m256 acc = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    acc = _mm256_add_ps(acc, _mm256_mul_ps(_mm256_loadu_ps(a + i),
                                           _mm256_loadu_ps(b + i)));
  }
  alignas(32) float tmp[8];
  _mm256_store_ps(tmp, acc);
  double sum = 0.0;
  for (int k = 0; k < 8; ++k) sum += tmp[k];
  for (; i < n; ++i) sum += static_cast<double>(a[i]) * b[i];
  return sum;
}

__attribute__((target("avx512f")))
double DotAvx512(const float* a, const float* b, std::size_t n) {
  __m512 acc = _mm512_setzero_ps();
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    acc = _mm512_add_ps(acc, _mm512_mul_ps(_mm512_loadu_ps(a + i),
                                           _mm512_loadu_ps(b + i)));
  }
  double sum = static_cast<double>(_mm512_reduce_add_ps(acc));
  for (; i < n; ++i) sum += static_cast<double>(a[i]) * b[i];
  return sum;
}

bool HasAvx512() {
  static const bool v = __builtin_cpu_supports("avx512f");
  return v;
}
bool HasAvx2() {
  static const bool v = __builtin_cpu_supports("avx2");
  return v;
}
#endif

void AddImpl(float* dst, const float* src, std::size_t n) {
#if defined(__x86_64__) || defined(_M_X64)
  if (HasAvx512()) return AddAvx512(dst, src, n);
  if (HasAvx2()) return AddAvx2(dst, src, n);
#endif
  for (std::size_t i = 0; i < n; ++i) dst[i] += src[i];
}

void ScaleImpl(float* dst, float factor, std::size_t n) {
#if defined(__x86_64__) || defined(_M_X64)
  if (HasAvx512()) return ScaleAvx512(dst, factor, n);
  if (HasAvx2()) return ScaleAvx2(dst, factor, n);
#endif
  for (std::size_t i = 0; i < n; ++i) dst[i] *= factor;
}

double DotImpl(const float* a, const float* b, std::size_t n) {
#if defined(__x86_64__) || defined(_M_X64)
  if (HasAvx512()) return DotAvx512(a, b, n);
  if (HasAvx2()) return DotAvx2(a, b, n);
#endif
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) sum += static_cast<double>(a[i]) * b[i];
  return sum;
}

}  // namespace

DenseVector::DenseVector(std::uint32_t dim) : dim_(dim), values_(dim, 0.0f) {}

DenseVector DenseVector::FromHypervector(const Hypervector& packed) {
  DenseVector result(packed.dim());
  for (std::uint32_t i = 0; i < packed.dim(); ++i) {
    const bool negative = (packed.words()[i >> 6] >> (i & 63)) & 1;
    result.values_[i] = negative ? -1.0f : 1.0f;
  }
  return result;
}

void DenseVector::Add(const DenseVector& other) {
  RequireSameDim(*this, other);
  AddImpl(values_.data(), other.values_.data(), dim_);
}

void DenseVector::Scale(double factor) {
  ScaleImpl(values_.data(), static_cast<float>(factor), dim_);
}

double DenseVector::Dot(const DenseVector& other) const {
  RequireSameDim(*this, other);
  return DotImpl(values_.data(), other.values_.data(), dim_);
}

double DenseVector::Norm() const { return std::sqrt(Dot(*this)); }

DenseVector DenseVector::Normalized() const {
  DenseVector result(*this);
  const double norm = Norm();
  if (norm > 0.0) result.Scale(1.0 / norm);
  return result;
}

}  // namespace gar::core::hdc