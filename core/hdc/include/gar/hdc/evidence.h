#pragma once

// EvidenceVector: the integer bundle E = sum of witness fact vectors
// (paper §4.1.1: E = bundle(witness facts), readout = cosine against a
// probe). Kept as per-coordinate int16 accumulators; the sign-packed
// majority vector is derived on readout.

#include <cstdint>
#include <vector>

#include "gar/hdc/hypervector.h"

namespace gar::core::hdc {

class EvidenceVector {
 public:
  // D int16 accumulators: at D = 16384 that is 32 KB — L2-resident, so
  // Witness stays a streaming pass over cache-hot memory.
  explicit EvidenceVector(std::uint32_t dim);

  // Adds one witness fact: acc[i] += +1/-1 per the fact's bit at i.
  // Overflow bound: int16 saturates beyond |E| = 32767 witnesses; the
  // paper's chain lengths are orders of magnitude below that, so no
  // saturation logic is implemented (noted, not handled).
  void Witness(const Hypervector& fact);

  // Sign-packed majority vector of the accumulator (bit = +1 where
  // acc[i] > 0, -1 where acc[i] < 0; exact zeros take the LSB of a
  // coordinate hash — deterministic, unbiased in expectation).
  [[nodiscard]] Hypervector Packed() const;

  // Readout against a probe fact vector: sign-pack the accumulator and
  // take the hypervector cosine (1 - 2*Hamming/D). Monotone in the
  // overlap between probe and the witnessed facts.
  [[nodiscard]] double Cosine(const Hypervector& probe) const;

  // Raw integer-bundle cosine against another evidence vector:
  // dot(acc_a, acc_b) / (|acc_a| * |acc_b|) — the direct App I readout,
  // ≈ c / sqrt(m*n) for c shared witnesses out of m / n.
  [[nodiscard]] double Cosine(const EvidenceVector& other) const;

  // Branch-and-bound forks copy the accumulator per branch.
  [[nodiscard]] EvidenceVector Clone() const { return *this; }

  [[nodiscard]] std::uint64_t Count() const { return count_; }
  [[nodiscard]] std::uint32_t Dim() const { return dim_; }

 private:
  std::uint32_t dim_;
  std::uint64_t count_ = 0;
  std::vector<std::int16_t> acc_;
};

}  // namespace gar::core::hdc
