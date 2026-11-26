#pragma once

#include <cstdint>
#include <vector>

#include "gar/core/hdc/hypervector.h"

namespace gar::core::hdc {

// DenseVector stores the real-valued vectors used by the paper's evidence
// estimator. It is separate from the packed value-model representation.
class DenseVector {
 public:
  explicit DenseVector(std::uint32_t dim);

  static DenseVector FromHypervector(const Hypervector& packed);

  void Add(const DenseVector& other);
  void Scale(double factor);
  [[nodiscard]] double Dot(const DenseVector& other) const;
  [[nodiscard]] double Norm() const;
  [[nodiscard]] DenseVector Normalized() const;

  [[nodiscard]] std::uint32_t dim() const { return dim_; }
  [[nodiscard]] const std::vector<float>& values() const { return values_; }

 private:
  std::uint32_t dim_;
  // float (not double): the learned policy encodes one vector per unique fact
  // and the cost is memory-bandwidth-bound (random 1KB reads through a hash
  // cache). Halving the element size halves that traffic and doubles SIMD
  // width; cosine/ranking precision is unaffected at float accuracy.
  std::vector<float> values_;
};

}  // namespace gar::core::hdc
