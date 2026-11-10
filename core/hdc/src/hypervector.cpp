#include "gar/hdc/hypervector.h"

#include <stdexcept>

#if defined(__AVX512VPOPCNTDQ__)
#include <immintrin.h>
#endif

namespace gar::core::hdc {

namespace {

void RequireSameDim(const Hypervector& a, const Hypervector& b) {
  if (a.dim() != b.dim()) {
    throw std::invalid_argument("Hypervector: dimension mismatch");
  }
}

bool GetBit(const Hypervector& v, std::uint32_t i) {
  return (v.words()[i >> 6] >> (i & 63)) & 1;
}

}  // namespace

Hypervector::Hypervector(std::uint32_t dim)
    : dim_(dim), words_((dim + 63) / 64, 0) {}

Hypervector Hypervector::FromHash(std::uint64_t hash_seed, std::uint32_t dim) {
  Hypervector v(dim);
  std::uint64_t state = hash_seed;
  for (auto& w : v.words_) w = SplitMix64Next(state);
  // Zero the tail bits past D so equal-D vectors compare equal regardless
  // of padding and Hamming never counts phantom coordinates.
  if (dim % 64 != 0 && !v.words_.empty()) {
    v.words_.back() &= (1ull << (dim % 64)) - 1;
  }
  return v;
}

Hypervector Hypervector::FromWords(std::uint32_t dim,
                                   std::vector<std::uint64_t> words) {
  Hypervector v(dim);
  words.resize(v.words_.size(), 0);
  v.words_ = std::move(words);
  if (dim % 64 != 0 && !v.words_.empty()) {
    v.words_.back() &= (1ull << (dim % 64)) - 1;
  }
  return v;
}

Hypervector Hypervector::Bind(const Hypervector& a, const Hypervector& b) {
  RequireSameDim(a, b);
  Hypervector r(a.dim_);
  for (std::size_t w = 0; w < r.words_.size(); ++w) {
    r.words_[w] = a.words_[w] ^ b.words_[w];
  }
  return r;
}

Hypervector Hypervector::Permute(const Hypervector& a, std::uint32_t shift) {
  const std::uint32_t d = a.dim_;
  if (d == 0) return a;
  shift %= d;
  if (shift == 0) return a;
  Hypervector r(d);
  const std::size_t nw = a.words_.size();
  if (d % 64 == 0) {
    // Word-level rotate: the packed length equals D exactly, so the wrap
    // around word 0 is the cyclic wrap around D.
    const std::size_t ws = shift / 64;
    const std::uint32_t bs = shift % 64;
    for (std::size_t w = 0; w < nw; ++w) {
      const std::uint64_t hi = a.words_[(w + nw - ws) % nw];
      const std::uint64_t lo = a.words_[(w + nw - ws - 1) % nw];
      r.words_[w] = (hi << bs) | (bs == 0 ? 0 : lo >> (64 - bs));
    }
  } else {
    // General path (non-64-multiple D): plain bit loop; correctness first,
    // such dims are outside the presets anyway.
    for (std::uint32_t i = 0; i < d; ++i) {
      if (GetBit(a, i)) {
        const std::uint32_t j = (i + shift) % d;
        r.words_[j >> 6] |= 1ull << (j & 63);
      }
    }
  }
  return r;
}

std::uint64_t Hypervector::Hamming(const Hypervector& a,
                                   const Hypervector& b) {
  RequireSameDim(a, b);
  const std::uint64_t* aw = a.words_.data();
  const std::uint64_t* bw = b.words_.data();
  const std::size_t n = a.words_.size();
  std::uint64_t h = 0;
  std::size_t w = 0;
#if defined(__AVX512VPOPCNTDQ__)
  // Four words at a time: VPOPCNTDQ counts the set bits of each 64-bit lane
  // of a^b in one instruction; sum the four lane counts. The scalar loop
  // below stays as the portable POPCNT fallback.
  for (; w + 4 <= n; w += 4) {
    const __m256i xa =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(aw + w));
    const __m256i xb =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bw + w));
    const __m256i pc = _mm256_popcnt_epi64(_mm256_xor_si256(xa, xb));
    const __m128i lo = _mm256_castsi256_si128(pc);
    const __m128i hi = _mm256_extracti128_si256(pc, 1);
    const __m128i s = _mm_add_epi64(lo, hi);
    alignas(16) std::uint64_t lanes[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes), s);
    h += lanes[0] + lanes[1];
  }
#endif
  for (; w < n; ++w) {
    h += static_cast<std::uint64_t>(__builtin_popcountll(aw[w] ^ bw[w]));
  }
  return h;
}

double Hypervector::Cosine(const Hypervector& a, const Hypervector& b) {
  const std::uint64_t h = Hamming(a, b);
  return 1.0 - 2.0 * static_cast<double>(h) / static_cast<double>(a.dim_);
}

}  // namespace gar::core::hdc
