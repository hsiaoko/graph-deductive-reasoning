#include "gar/hdc/evidence.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gar {
namespace hdc {

namespace {

// Salt separating the zero-coordinate tie stream from symbol streams.
constexpr std::uint64_t kZeroTieSalt = 0x5A45524F54494553ull;  // "ZEROTIES"

}  // namespace

EvidenceVector::EvidenceVector(std::uint32_t dim)
    : dim_(dim), acc_(dim, 0) {}

void EvidenceVector::Witness(const Hypervector& fact) {
  if (fact.dim() != dim_) {
    throw std::invalid_argument("EvidenceVector: dimension mismatch");
  }
  const auto& words = fact.words();
  for (std::size_t w = 0; w < words.size(); ++w) {
    const std::uint64_t x = words[w];
    const std::size_t base = w * 64;
    const std::size_t hi = std::min<std::size_t>(base + 64, dim_);
    for (std::size_t i = base; i < hi; ++i) {
      acc_[i] += static_cast<std::int16_t>((x >> (i - base)) & 1 ? 1 : -1);
    }
  }
  ++count_;
}

Hypervector EvidenceVector::Packed() const {
  Hypervector v(dim_);
  std::vector<std::uint64_t> words(v.words().size(), 0);
  for (std::uint32_t i = 0; i < dim_; ++i) {
    bool bit;
    if (acc_[i] > 0) {
      bit = true;
    } else if (acc_[i] < 0) {
      bit = false;
    } else {
      // Exact-zero coordinates get a deterministic hash bit instead of a
      // fixed choice: an always-+1 rule would bias every readout upward.
      std::uint64_t s = kZeroTieSalt ^ i;
      bit = SplitMix64Next(s) & 1;
    }
    words[i >> 6] |= static_cast<std::uint64_t>(bit) << (i & 63);
  }
  return Hypervector::FromWords(dim_, std::move(words));
}

double EvidenceVector::Cosine(const Hypervector& probe) const {
  return Hypervector::Cosine(Packed(), probe);
}

double EvidenceVector::Cosine(const EvidenceVector& other) const {
  if (other.dim_ != dim_) {
    throw std::invalid_argument("EvidenceVector: dimension mismatch");
  }
  std::int64_t dot = 0;
  std::uint64_t na = 0, nb = 0;
  for (std::uint32_t i = 0; i < dim_; ++i) {
    dot += static_cast<std::int64_t>(acc_[i]) * other.acc_[i];
    na += static_cast<std::uint64_t>(acc_[i]) * acc_[i];
    nb += static_cast<std::uint64_t>(other.acc_[i]) * other.acc_[i];
  }
  if (na == 0 || nb == 0) return 0.0;
  return static_cast<double>(dot) /
         (std::sqrt(static_cast<double>(na)) *
          std::sqrt(static_cast<double>(nb)));
}

}  // namespace hdc
}  // namespace gar
