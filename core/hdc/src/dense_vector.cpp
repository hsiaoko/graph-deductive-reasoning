#include "gar/core/hdc/dense_vector.h"

#include <cmath>
#include <stdexcept>

namespace gar::core::hdc {

namespace {

void RequireSameDim(const DenseVector& a, const DenseVector& b) {
  if (a.dim() != b.dim()) {
    throw std::invalid_argument("DenseVector: dimension mismatch");
  }
}

}  // namespace

DenseVector::DenseVector(std::uint32_t dim) : dim_(dim), values_(dim, 0.0) {}

DenseVector DenseVector::FromHypervector(const Hypervector& packed) {
  DenseVector result(packed.dim());
  for (std::uint32_t i = 0; i < packed.dim(); ++i) {
    const bool negative = (packed.words()[i >> 6] >> (i & 63)) & 1;
    result.values_[i] = negative ? -1.0 : 1.0;
  }
  return result;
}

void DenseVector::Add(const DenseVector& other) {
  RequireSameDim(*this, other);
  for (std::uint32_t i = 0; i < dim_; ++i) values_[i] += other.values_[i];
}

void DenseVector::Scale(double factor) {
  for (double& value : values_) value *= factor;
}

double DenseVector::Dot(const DenseVector& other) const {
  RequireSameDim(*this, other);
  double result = 0.0;
  for (std::uint32_t i = 0; i < dim_; ++i) result += values_[i] * other.values_[i];
  return result;
}

double DenseVector::Norm() const {
  return std::sqrt(Dot(*this));
}

DenseVector DenseVector::Normalized() const {
  DenseVector result(*this);
  const double norm = Norm();
  if (norm > 0.0) result.Scale(1.0 / norm);
  return result;
}

}  // namespace gar::core::hdc
