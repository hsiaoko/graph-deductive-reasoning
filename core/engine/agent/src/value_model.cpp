#include "gar/engine/agent/value_model.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "gar/hdc/value_kernels.h"

namespace gar::core::engine {
namespace agent {

const char* HdcBackendName(HdcBackend backend) {
  switch (backend) {
    case HdcBackend::kOriginal:
      return "original";
    case HdcBackend::kSimd:
      return "simd";
    case HdcBackend::kBitLut:
      return "bit-lut";
  }
  return "unknown";
}

bool ValueModel::Load(const std::string& meta_path) {
  std::ifstream in(meta_path);
  if (!in) {
    std::fprintf(stderr, "[value_model] cannot open %s\n", meta_path.c_str());
    return false;
  }
  std::string format;
  std::uint32_t n_keys = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "format") {
      ls >> format;
      if (format != "hdc_value_v1" && format != "numeric_ridge_v1") {
        std::fprintf(stderr, "[value_model] bad format %s\n",
                     format.c_str());
        return false;
      }
      kind_ = format == "hdc_value_v1" ? "hdc" : "numeric";
    } else if (tag == "dim") {
      ls >> dim_;
    } else if (tag == "n_keys") {
      ls >> n_keys;
    } else if (tag == "bin_cap") {
      ls >> bin_cap_;
    } else if (tag == "bin_scale") {
      ls >> bin_scale_;
    } else if (tag == "names") {
      std::string n;
      while (ls >> n) names_.push_back(n);
    } else if (tag == "mean") {
      double v;
      while (ls >> v) mean_.push_back(v);
    } else if (tag == "std") {
      double v;
      while (ls >> v) std_.push_back(v);
    } else if (tag == "key") {
      std::uint32_t idx, bin;
      std::string name;
      ls >> idx >> name >> bin;
      if (idx != keys_.size()) {
        std::fprintf(stderr, "[value_model] key index mismatch\n");
        return false;
      }
      keys_.emplace_back(name, bin);
    } else if (tag == "conj") {
      std::uint32_t a, b;
      ls >> a >> b;
      conj_.emplace_back(a, b);
    }
  }
  if (names_.empty() || names_.size() > 11) {
    std::fprintf(stderr, "[value_model] incomplete meta %s\n",
                 meta_path.c_str());
    return false;
  }
  for (const auto& [a, b] : conj_) {
    if (a >= names_.size() || b >= names_.size()) {
      std::fprintf(stderr, "[value_model] conjunction index out of range\n");
      return false;
    }
  }
  if (kind_ == "hdc") {
    if (dim_ == 0 || keys_.size() != n_keys || keys_.empty()) {
      std::fprintf(stderr, "[value_model] incomplete meta %s\n",
                   meta_path.c_str());
      return false;
    }
    if (names_.size() * (bin_cap_ + 1) + 1 != n_keys) {
      std::fprintf(stderr,
                   "[value_model] key layout mismatch (%zu*%u+1 != %u)\n",
                   names_.size(), bin_cap_ + 1, n_keys);
      return false;
    }
  } else {
    if (mean_.size() != names_.size() || std_.size() != names_.size()) {
      std::fprintf(stderr,
                   "[value_model] mean/std size mismatch (%zu/%zu != %zu)\n",
                   mean_.size(), std_.size(), names_.size());
      return false;
    }
    for (double s : std_) {
      if (!(s > 0.0)) {
        std::fprintf(stderr, "[value_model] non-positive std\n");
        return false;
      }
    }
  }
  std::string bin_path = meta_path;
  const std::string suf = ".meta";
  if (bin_path.size() > suf.size() &&
      bin_path.compare(bin_path.size() - suf.size(), suf.size(), suf) == 0) {
    bin_path.replace(bin_path.size() - suf.size(), suf.size(), ".bin");
  } else {
    bin_path += ".bin";
  }
  std::ifstream bin(bin_path, std::ios::binary);
  if (!bin) {
    std::fprintf(stderr, "[value_model] cannot open %s\n", bin_path.c_str());
    return false;
  }
  if (kind_ == "hdc") {
    vecs_.resize(static_cast<std::size_t>(n_keys) * dim_);
    bin.read(reinterpret_cast<char*>(vecs_.data()), vecs_.size());
    w_.resize(dim_ + 1);
    bin.read(reinterpret_cast<char*>(w_.data()),
             static_cast<std::streamsize>(w_.size() * sizeof(double)));
  } else {
    wnum_.resize(names_.size() + conj_.size() + 1);
    bin.read(reinterpret_cast<char*>(wnum_.data()),
             static_cast<std::streamsize>(wnum_.size() * sizeof(double)));
  }
  if (!bin) {
    std::fprintf(stderr, "[value_model] truncated %s\n", bin_path.c_str());
    return false;
  }
  return kind_ != "hdc" || hdc_backend_ != HdcBackend::kBitLut ||
         PrepareBitLut();
}

std::array<std::uint32_t, 11> ValueModel::FeatureKeys(
    const std::array<double, 11>& raw) const {
  std::array<std::uint32_t, 11> idx{};
  const std::uint32_t nb = bin_cap_ + 1;
  for (std::size_t j = 0; j < names_.size(); ++j) {
    int b = static_cast<int>(std::floor(std::abs(raw[j]) * bin_scale_));
    if (b < 0) b = 0;
    if (b > static_cast<int>(bin_cap_)) b = static_cast<int>(bin_cap_);
    idx[j] = static_cast<std::uint32_t>(j) * nb + static_cast<std::uint32_t>(b);
  }
  return idx;
}

bool ValueModel::PrepareBitLut() {
  const std::size_t nw = (dim_ + 63) / 64;
  packed_vecs_.clear();
  packed_vecs_.reserve(keys_.size());
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    std::vector<std::uint64_t> words(nw, 0);
    const std::int8_t* v = vecs_.data() + k * dim_;
    for (std::uint32_t d = 0; d < dim_; ++d) {
      if (v[d] < 0) words[d >> 6] |= std::uint64_t{1} << (d & 63);
    }
    packed_vecs_.push_back(gar::core::hdc::Hypervector::FromWords(dim_, std::move(words)));
  }
  weight_sum_ = 0.0;
  for (std::uint32_t d = 0; d < dim_; ++d) weight_sum_ += w_[d];
  base_lut_.resize(keys_.size());
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    base_lut_[k] = gar::core::hdc::DotPackedBipolar(packed_vecs_[k].words().data(),
                                         w_.data(), dim_, weight_sum_);
  }
  const std::uint32_t nb = bin_cap_ + 1;
  conj_lut_.resize(conj_.size() * nb * nb);
  for (std::size_t c = 0; c < conj_.size(); ++c) {
    const auto [a, b] = conj_[c];
    for (std::uint32_t ba = 0; ba < nb; ++ba) {
      for (std::uint32_t bb = 0; bb < nb; ++bb) {
        const auto& va = packed_vecs_[a * nb + ba];
        const auto& vb = packed_vecs_[b * nb + bb];
        const auto bound = gar::core::hdc::Hypervector::Bind(va, vb);
        conj_lut_[(c * nb + ba) * nb + bb] =
            gar::core::hdc::DotPackedBipolar(bound.words().data(), w_.data(), dim_,
                                  weight_sum_);
      }
    }
  }
  return true;
}

double ValueModel::PredictHdcOriginal(
    const std::array<double, 11>& raw) const {
  const auto idx = FeatureKeys(raw);
  std::vector<std::int16_t> hv(dim_, 0);
  auto add = [&](std::uint32_t k) {
    const std::int8_t* v = vecs_.data() + static_cast<std::size_t>(k) * dim_;
    for (std::uint32_t d = 0; d < dim_; ++d) hv[d] += v[d];
  };
  for (std::size_t j = 0; j < names_.size(); ++j) add(idx[j]);
  for (const auto& [a, b] : conj_) {
    const std::int8_t* va = vecs_.data() + static_cast<std::size_t>(idx[a]) * dim_;
    const std::int8_t* vb = vecs_.data() + static_cast<std::size_t>(idx[b]) * dim_;
    for (std::uint32_t d = 0; d < dim_; ++d) hv[d] += va[d] * vb[d];
  }
  add(static_cast<std::uint32_t>(keys_.size()) - 1);
  double s = w_.back();
  for (std::uint32_t d = 0; d < dim_; ++d) s += w_[d] * hv[d];
  return s;
}

double ValueModel::PredictHdcSimd(const std::array<double, 11>& raw) const {
  const auto idx = FeatureKeys(raw);
  thread_local std::vector<std::int16_t> hv;
  hv.assign(dim_, 0);
  auto add = [&](std::uint32_t k) {
    gar::core::hdc::AddBipolar(hv.data(),
                    vecs_.data() + static_cast<std::size_t>(k) * dim_, dim_);
  };
  for (std::size_t j = 0; j < names_.size(); ++j) add(idx[j]);
  for (const auto& [a, b] : conj_) {
    gar::core::hdc::AddBoundBipolar(
        hv.data(), vecs_.data() + static_cast<std::size_t>(idx[a]) * dim_,
        vecs_.data() + static_cast<std::size_t>(idx[b]) * dim_, dim_);
  }
  add(static_cast<std::uint32_t>(keys_.size()) - 1);
  return w_.back() + gar::core::hdc::DotInt16Double(hv.data(), w_.data(), dim_);
}

double ValueModel::PredictHdcBitLut(const std::array<double, 11>& raw) const {
  const auto idx = FeatureKeys(raw);
  double s = w_.back() + base_lut_.back();
  for (std::size_t j = 0; j < names_.size(); ++j) s += base_lut_[idx[j]];
  const std::uint32_t nb = bin_cap_ + 1;
  for (std::size_t c = 0; c < conj_.size(); ++c) {
    const auto [a, b] = conj_[c];
    const std::uint32_t ba = idx[a] % nb;
    const std::uint32_t bb = idx[b] % nb;
    s += conj_lut_[(c * nb + ba) * nb + bb];
  }
  return s;
}

double ValueModel::PredictRemaining(const std::array<double, 11>& raw) const {
  const auto t0 = std::chrono::steady_clock::now();
  ++infer_calls;
  double s;
  if (kind_ == "numeric") {
    const std::size_t nf = names_.size();
    double z[16];
    for (std::size_t j = 0; j < nf; ++j) z[j] = (raw[j] - mean_[j]) / std_[j];
    s = wnum_.back();
    for (std::size_t j = 0; j < nf; ++j) s += wnum_[j] * z[j];
    for (std::size_t c = 0; c < conj_.size(); ++c) {
      s += wnum_[nf + c] * z[conj_[c].first] * z[conj_[c].second];
    }
  } else {
    switch (hdc_backend_) {
      case HdcBackend::kOriginal:
        s = PredictHdcOriginal(raw);
        break;
      case HdcBackend::kSimd:
        s = PredictHdcSimd(raw);
        break;
      case HdcBackend::kBitLut:
        s = PredictHdcBitLut(raw);
        break;
    }
  }
  infer_ns += std::chrono::duration<double, std::nano>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
  return s;
}

}  // namespace agent
}  // namespace gar::core::engine
