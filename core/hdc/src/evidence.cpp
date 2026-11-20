#include "gar/core/hdc/evidence.h"

namespace gar::core::hdc {

EvidenceVector::EvidenceVector(std::uint32_t dim) : acc_(dim) {}

void EvidenceVector::Witness(const DenseVector& fact) {
  acc_.Add(fact);
  ++count_;
}

double EvidenceVector::Dot(const EvidenceVector& other) const {
  return acc_.Dot(other.acc_);
}

}  // namespace gar::core::hdc
