#include "gar/engine/agent/value_model.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gar {
namespace engine {
namespace agent {

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
      if (format != "hdc_value_v1") {
        std::fprintf(stderr, "[value_model] bad format %s\n",
                     format.c_str());
        return false;
      }
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
  if (dim_ == 0 || keys_.size() != n_keys || keys_.empty()) {
    std::fprintf(stderr, "[value_model] incomplete meta %s\n",
                 meta_path.c_str());
    return false;
  }
  // The feature-bin keys are laid out feature-major: feature j owns the
  // contiguous block [j*(bin_cap+1), (j+1)*(bin_cap+1)).
  if (names_.size() * (bin_cap_ + 1) + 1 != n_keys) {
    std::fprintf(stderr, "[value_model] key layout mismatch (%zu*%u+1 != %u)\n",
                 names_.size(), bin_cap_ + 1, n_keys);
    return false;
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
  vecs_.resize(static_cast<std::size_t>(n_keys) * dim_);
  bin.read(reinterpret_cast<char*>(vecs_.data()), vecs_.size());
  w_.resize(dim_ + 1);
  bin.read(reinterpret_cast<char*>(w_.data()),
           static_cast<std::streamsize>(w_.size() * sizeof(double)));
  if (!bin) {
    std::fprintf(stderr, "[value_model] truncated %s\n", bin_path.c_str());
    return false;
  }
  return true;
}

double ValueModel::PredictRemaining(const std::array<double, 11>& raw) const {
  const auto t0 = std::chrono::steady_clock::now();
  ++infer_calls;
  const std::uint32_t nb = bin_cap_ + 1;
  std::uint32_t idx[16];
  for (std::size_t j = 0; j < names_.size() && j < raw.size(); ++j) {
    int b = static_cast<int>(std::floor(std::abs(raw[j]) * bin_scale_));
    if (b < 0) b = 0;
    if (b > static_cast<int>(bin_cap_)) b = static_cast<int>(bin_cap_);
    idx[j] = static_cast<std::uint32_t>(j) * nb + static_cast<std::uint32_t>(b);
  }
  std::vector<std::int16_t> hv(dim_, 0);
  auto add = [&](std::uint32_t k) {
    const std::int8_t* v = vecs_.data() + static_cast<std::size_t>(k) * dim_;
    for (std::uint32_t d = 0; d < dim_; ++d) hv[d] += v[d];
  };
  for (std::size_t j = 0; j < names_.size() && j < raw.size(); ++j) add(idx[j]);
  for (const auto& [a, b] : conj_) {
    const std::int8_t* va = vecs_.data() + static_cast<std::size_t>(idx[a]) * dim_;
    const std::int8_t* vb = vecs_.data() + static_cast<std::size_t>(idx[b]) * dim_;
    for (std::uint32_t d = 0; d < dim_; ++d) hv[d] += va[d] * vb[d];
  }
  add(static_cast<std::uint32_t>(keys_.size()) - 1);  // bias vector
  double s = w_.back();
  for (std::uint32_t d = 0; d < dim_; ++d) s += w_[d] * hv[d];
  infer_ns += std::chrono::duration<double, std::nano>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
  return s;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
