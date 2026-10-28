#pragma once

// ValueModel: serialized HDC random-feature ridge predicting a bnb branch's
// remaining cost (experiments/hdc_online). The model file pair
// (X.meta + X.bin) is produced by train_export.py from GAR_DEBUG_BNB traces;
// meta declares the encoding (dimension, binning, feature names/order,
// conjunctions), bin holds the int8 base vectors and the float64 weights.
// Usage is ORDERING ONLY: callers substitute the predicted remaining cost
// for the open-set priority's min-price term; no bounding, pruning or
// certificate semantics depend on it.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gar {
namespace engine {
namespace agent {

class ValueModel {
 public:
  // Loads meta text and the sibling .bin (meta path suffix ".meta" ->
  // ".bin"). Returns false (with a stderr reason) on any inconsistency.
  bool Load(const std::string& meta_path);

  // Predicted remaining cost for a branch; `raw` must follow the meta
  // `names` order (11 features for the shipped models).
  double PredictRemaining(const std::array<double, 11>& raw) const;

  const std::vector<std::string>& names() const { return names_; }

  // Inference accounting (mutable so const callers of Predict can meter).
  mutable std::uint64_t infer_calls = 0;
  mutable double infer_ns = 0.0;

 private:
  std::uint32_t dim_ = 0;
  std::uint32_t bin_cap_ = 7;
  double bin_scale_ = 2.0;
  std::vector<std::string> names_;
  std::vector<std::pair<std::string, std::uint32_t>> keys_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> conj_;
  std::vector<std::int8_t> vecs_;  // n_keys * dim, row-major
  std::vector<double> w_;          // dim + 1 (last = bias)
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
