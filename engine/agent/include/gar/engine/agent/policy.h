#pragma once

// Policy: motif selection policies (paper §5.1). The policy maps the scored
// candidate set to one choice per step; OnApplication is the learning hook
// (empty for the handcrafted policies).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/goal.h"
#include "gar/engine/agent/jev_client.h"
#include "gar/engine/agent/motif.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

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
  // Rule-level selection (incremental greedy, policy-first): picks an
  // index into `rules`, or nullopt to decline. Default: argmax of the
  // static priority, ties to the lower index — the deterministic order.
  // The symbolic reachability constraint is applied BEFORE this call (the
  // caller only passes DG-reachable rules), so the hybrid policy shares
  // this default; JevPolicy keeps the deterministic fallback at rule
  // level (the Jev protocol stays motif-level).
  [[nodiscard]] virtual std::optional<std::size_t> SelectRule(
      const std::vector<ScoredRule>& rules) {
    if (rules.empty()) return std::nullopt;
    std::size_t best = 0;
    for (std::size_t i = 1; i < rules.size(); ++i) {
      if (rules[i].priority > rules[best].priority) best = i;
    }
    return best;
  }
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

  // Rule level: softmax over the static priority with the same tau/stream.
  std::optional<std::size_t> SelectRule(
      const std::vector<ScoredRule>& rules) override {
    if (rules.empty()) return std::nullopt;
    double mx = rules[0].priority;
    for (const auto& r : rules) mx = std::max(mx, r.priority);
    std::vector<double> w(rules.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < rules.size(); ++i) {
      w[i] = std::exp((rules[i].priority - mx) / tau_);
      sum += w[i];
    }
    std::uniform_real_distribution<double> u(0.0, sum);
    const double pick = u(rng_);
    double acc = 0.0;
    for (std::size_t i = 0; i < rules.size(); ++i) {
      acc += w[i];
      if (pick < acc) return i;
    }
    return rules.size() - 1;
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

struct JevConfig {
  std::string endpoint = "http://127.0.0.1:8080";
  std::uint32_t timeout_ms = 2000;
  std::size_t top_k = 8;
  bool sample = false;
  std::uint64_t seed = 42;
};

// JevPolicy: the LLM decision-model policy (paper §5.3). Per step:
// admissible hard-filter (same symbolic guarantee as the hybrid policy,
// Corollary 4) -> keep the top_k candidates by the handcrafted score ->
// hand them to the Jev decision model with a candidate dossier (rule
// id/name/description, d(p0), price, witnessed x/y, confidence, the
// shortest derivation path to the goal, recursive-family annotation) ->
// the returned probabilities ARE pi. "Rank, not invent": candidates and
// every logical check stay symbolic; the model only ranks.
//
// Calls are control overhead: wall-clock, NOT charged to the budget B
// (§5.3). Counters are public for the app to report (summary.yaml's
// control_overhead section).
//
// On any client failure (connect/timeout/parse/unmappable answer) it warns
// once on stderr and falls back to plain argmax-total over ALL candidates
// (exactly DeterministicPolicy's selection).
//
// NOTE: with BranchBoundExecutor the ranking comes from the scorer (the
// symbolic f), so a Jev policy makes no calls there (calls()==0); Jev
// ranks inside the greedy loop, which is the §5.1 policy point.
class JevPolicy final : public Policy {
 public:
  JevPolicy(JevConfig cfg, std::unique_ptr<JevClient> client,
            const Goal* goal, const graph::TripleGraph* g,
            const analysis::DependencyGraph* dg,
            const std::vector<GARule>* rules,
            const std::vector<std::string>* rule_ids, double budget)
      : cfg_(std::move(cfg)),
        client_(std::move(client)),
        goal_(goal),
        g_(g),
        dg_(dg),
        rules_(rules),
        rule_ids_(rule_ids),
        remaining_(budget),
        rng_(cfg_.seed) {}

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override;

  void OnApplication(const Motif& mu, bool success, double cost) override {
    (void)mu;
    (void)success;
    remaining_ -= cost;
  }

  [[nodiscard]] std::uint64_t calls() const { return calls_; }
  [[nodiscard]] double total_wall_ms() const { return wall_ms_; }
  [[nodiscard]] std::uint64_t fallbacks() const { return fallbacks_; }

 private:
  JevConfig cfg_;
  std::unique_ptr<JevClient> client_;
  const Goal* goal_;
  const graph::TripleGraph* g_;
  const analysis::DependencyGraph* dg_;
  const std::vector<GARule>* rules_;
  const std::vector<std::string>* rule_ids_;
  double remaining_;
  std::mt19937 rng_;

  std::uint64_t calls_ = 0;
  double wall_ms_ = 0.0;
  std::uint64_t fallbacks_ = 0;
  bool warned_ = false;

  // Lazily computed per-goal tables (d(.) for the dossier text).
  std::vector<std::uint32_t> distances_;
  std::uint32_t goal_pred_ = analysis::kInvalidPredicate;
  void EnsureGoalTables();
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
