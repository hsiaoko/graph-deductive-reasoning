#pragma once

// Hypervector: a D-dimensional bipolar hypervector, bit-packed into uint64
// words. Bit convention: 1 = -1, 0 = +1 (so XOR is bipolar multiplication,
// i.e. binding). The dimension is a runtime value (constructor argument),
// not a template parameter, so CLIs can pick it from kHdcDimPresets.

#include <cstdint>
#include <vector>

namespace gar {
namespace hdc {

// splitmix64 stream step: the hash primitive behind symbol generation and
// deterministic tie-breaking. Each call yields the next 64 pseudorandom
// bits of the stream seeded at `state`.
inline std::uint64_t SplitMix64Next(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

class Hypervector {
 public:
  // All-zero vector (all +1 in bipolar reading); the identity of Bind.
  // `dim` need not be a multiple of 64 — the tail bits of the last
  // word are kept zeroed.
  explicit Hypervector(std::uint32_t dim);

  // Pseudorandom vector from a hash seed: a splitmix64 stream fills the
  // words, one stream step per 64 bits. Equal seeds give equal vectors;
  // distinct seeds give near-orthogonal ones (cosine ~ N(0, 1/D)).
  static Hypervector FromHash(std::uint64_t hash_seed, std::uint32_t dim);

  // Takes ownership of a packed word array (resized/masked to `dim`).
  static Hypervector FromWords(std::uint32_t dim,
                               std::vector<std::uint64_t> words);

  // Binding: wordwise XOR. Self-inverse: Bind(Bind(a,b),b) == a.
  static Hypervector Bind(const Hypervector& a, const Hypervector& b);

  // Cyclic left shift by `shift` (taken modulo D): the bit at position i
  // moves to position (i + shift) mod D. Permute(a, s) is near-orthogonal
  // to a for s != 0 mod D, which is what makes it usable as a positional
  // encoding for fact arguments.
  static Hypervector Permute(const Hypervector& a, std::uint32_t shift);

  static std::uint64_t Hamming(const Hypervector& a, const Hypervector& b);

  // Bipolar cosine. Derivation: with h = Hamming(a,b), the bits agree on
  // D-h coordinates (+1 each in the dot product) and disagree on h (-1
  // each), so dot = (D-h) - h = D - 2h; both norms are sqrt(D), hence
  // cosine = (D - 2h) / D = 1 - 2h/D.
  static double Cosine(const Hypervector& a, const Hypervector& b);

  [[nodiscard]] std::uint32_t dim() const { return dim_; }
  [[nodiscard]] const std::vector<std::uint64_t>& words() const {
    return words_;
  }

  bool operator==(const Hypervector& o) const {
    return dim_ == o.dim_ && words_ == o.words_;
  }
  bool operator!=(const Hypervector& o) const { return !(*this == o); }

 private:
  std::uint32_t dim_;
  std::vector<std::uint64_t> words_;
};

}  // namespace hdc
}  // namespace gar
