#pragma once

#include <cstdint>

#include "gar/core/hdc/dense_vector.h"

namespace gar::core::hdc {

// EvidenceVector is the dense bundle E = sum of distinct unit-norm witness
// fact vectors. WitnessLog owns set deduplication before calling Witness.
class EvidenceVector {
 public:
  explicit EvidenceVector(std::uint32_t dim);

  void Witness(const DenseVector& fact);

  // Raw bundle inner product, without cosine normalization or sign packing.
  [[nodiscard]] double Dot(const EvidenceVector& other) const;

  [[nodiscard]] EvidenceVector Clone() const { return *this; }
  [[nodiscard]] std::uint64_t Count() const { return count_; }
  [[nodiscard]] std::uint32_t Dim() const { return acc_.dim(); }

 private:
  DenseVector acc_;
  std::uint64_t count_ = 0;
};

}  // namespace gar::core::hdc
