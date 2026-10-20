#pragma once

// Policy: motif selection policies (paper §5.1). The policy maps the scored
// candidate set to one choice per step; OnApplication is the learning hook
// (empty for the handcrafted policies).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

#include "gar/engine/agent/motif.h"

namespace gar {
namespace engine {
namespace agent {

class Policy {
 public:
  virtual ~Policy() = default;
  // Picks an index into `cands`, or nullopt to decline all (the executor
  // then terminates with "no_candidates").
  [[nodiscard]] virtual std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) = 0;
  virtual void OnApplication(const Motif& mu, bool success, double cost) {
    (void)mu;
    (void)success;
    (void)cost;
  }
};

// Deterministic: argmax of the score, greedy with respect to f (§5.1);
// ties break to the lower index (candidate order is deterministic).
class DeterministicPolicy final : public Policy {
 public:
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    if (cands.empty()) return std::nullopt;
    std::size_t best = 0;
    for (std::size_t i = 1; i < cands.size(); ++i) {
      if (cands[i].total > cands[best].total) best = i;
    }
    return best;
  }
};

// Randomized: softmax over total/tau (§5.1); low tau approaches the
// deterministic policy. The mt19937 stream is seeded at construction, so a
// fixed seed reproduces a run exactly.
class RandomizedPolicy final : public Policy {
 public:
  explicit RandomizedPolicy(std::uint64_t seed, double tau = 1.0)
      : rng_(seed), tau_(tau) {}

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    if (cands.empty()) return std::nullopt;
    double mx = cands[0].total;
    for (const auto& c : cands) mx = std::max(mx, c.total);
    std::vector<double> w(cands.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      w[i] = std::exp((cands[i].total - mx) / tau_);
      sum += w[i];
    }
    std::uniform_real_distribution<double> u(0.0, sum);
    const double pick = u(rng_);
    double acc = 0.0;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      acc += w[i];
      if (pick < acc) return i;
    }
    return cands.size() - 1;
  }

 private:
  std::mt19937 rng_;
  double tau_;
};

// Hybrid neuro-symbolic: hard-filter to `admissible` motifs first (the
// symbolic guarantee — d(p0) < inf and the conclusion is new; deduction
// stays complete, Corollary 4), then argmax of the score inside.
class HybridNeuroSymbolicPolicy final : public Policy {
 public:
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      if (!cands[i].admissible) continue;
      if (!best || cands[i].total > cands[*best].total) best = i;
    }
    return best;
  }
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
