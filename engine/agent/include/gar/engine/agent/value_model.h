#pragma once

// ValueModel: serialized HDC random-feature or numeric ridge for ordering
// BNB branches and AdaDeduce actions. The meta file declares dimension,
// binning, feature order and conjunctions; the sibling bin file stores int8
// base vectors and float64 weights. Predictions never affect logical
// validation, derivation, pruning certificates or soundness.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gar/hdc/hypervector.h"

namespace gar {
namespace engine {
namespace agent {

enum class HdcBackend {
  kOriginal,
  kSimd,
  kBitLut,
};

const char* HdcBackendName(HdcBackend backend);

class ValueModel {
 public:
  // Select the HDC implementation before Load. All backends evaluate the
  // same bipolar bundle/bind ridge model; bit-Lut moves its finite feature
  // vocabulary's weighted dot products to model-loading time.
  void SetHdcBackend(HdcBackend backend) { hdc_backend_ = backend; }
  HdcBackend hdc_backend() const { return hdc_backend_; }

  // Loads meta text and the sibling .bin (meta path suffix ".meta" ->
  // ".bin"). Returns false (with a stderr reason) on any inconsistency.
  bool Load(const std::string& meta_path);

  // Predicted remaining cost; raw follows the meta names order.
  double PredictRemaining(const std::array<double, 11>& raw) const;

  const std::vector<std::string>& names() const { return names_; }
  const std::string& kind() const { return kind_; }

  // Inference accounting (mutable so const callers of Predict can meter).
  mutable std::uint64_t infer_calls = 0;
  mutable double infer_ns = 0.0;

 private:
  std::array<std::uint32_t, 11> FeatureKeys(
      const std::array<double, 11>& raw) const;
  bool PrepareBitLut();
  double PredictHdcOriginal(const std::array<double, 11>& raw) const;
  double PredictHdcSimd(const std::array<double, 11>& raw) const;
  double PredictHdcBitLut(const std::array<double, 11>& raw) const;

  std::string kind_;  // "hdc" | "numeric"
  std::uint32_t dim_ = 0;
  std::uint32_t bin_cap_ = 7;
  double bin_scale_ = 2.0;
  std::vector<std::string> names_;
  std::vector<std::pair<std::string, std::uint32_t>> keys_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> conj_;
  std::vector<std::int8_t> vecs_;  // hdc: n_keys * dim, row-major
  std::vector<double> w_;          // hdc: dim + 1 (last = bias)
  std::vector<double> mean_, std_;
  std::vector<double> wnum_;

  HdcBackend hdc_backend_ = HdcBackend::kBitLut;
  std::vector<hdc::Hypervector> packed_vecs_;
  std::vector<double> base_lut_;
  std::vector<double> conj_lut_;
  double weight_sum_ = 0.0;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
