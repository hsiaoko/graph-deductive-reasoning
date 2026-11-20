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
  [[nodiscard]] const std::vector<double>& values() const { return values_; }

 private:
  std::uint32_t dim_;
  std::vector<double> values_;
};

}  // namespace gar::core::hdc
