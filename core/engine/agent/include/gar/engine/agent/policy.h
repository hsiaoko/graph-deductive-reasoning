#pragma once

// Policy: motif selection policies (paper §5.1). The policy maps the scored
// candidate set to one choice per step; OnApplication is the learning hook
// (empty for the handcrafted policies).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/goal.h"
#include "gar/engine/agent/jev_client.h"
#include "gar/engine/agent/motif.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

namespace gar::core::engine {
namespace agent {

class ValueModel;

class Policy {
 public:
  virtual ~Policy() = default;
  // Picks an index into `cands`, or nullopt to decline all (the executor
  // then terminates with "no_candidates").
  [[nodiscard]] virtual std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) = 0;
  // Score-only selection avoids copying every embedding when the executor
  // maintains current scores incrementally. Policies that need full dossiers
  // (Jev) keep the default and use Select above instead.
  virtual bool SupportsScoreView() const { return false; }
  virtual bool PrefersHighestScore() const { return false; }
  virtual double SelectionPriority(std::uint64_t action_id, double total) {
    (void)action_id;
    return total;
  }
  virtual double SelectionPriorityWithFeatures(
      std::uint64_t action_id, double total,
      const std::array<double, 11>& features) {
    (void)features;
    return SelectionPriority(action_id, total);
  }
  virtual void ReleaseSelectionPriority(std::uint64_t action_id) {
    (void)action_id;
  }
  virtual bool RequiresAdmissible() const { return false; }
  [[nodiscard]] virtual std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) {
    (void)totals;
    (void)admissible;
    return std::nullopt;
  }
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
  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }

  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override {
    (void)admissible;
    if (totals.empty()) return std::nullopt;
    std::size_t best = 0;
    for (std::size_t i = 1; i < totals.size(); ++i) {
      if (totals[i] > totals[best]) best = i;
    }
    return best;
  }

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    std::vector<double> totals;
    std::vector<char> admissible;
    for (const auto& c : cands) {
      totals.push_back(c.total);
      admissible.push_back(c.admissible);
    }
    return SelectScores(totals, admissible);
  }
};

// Randomized: softmax over total/tau (§5.1); low tau approaches the
// deterministic policy. The mt19937 stream is seeded at construction, so a
// fixed seed reproduces a run exactly.
class RandomizedPolicy final : public Policy {
 public:
  explicit RandomizedPolicy(std::uint64_t seed, double tau = 1.0)
      : rng_(seed), tau_(tau) {}

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }

  // Gumbel-max is softmax sampling without rescanning the ready set. Keep
  // one independent perturbation per live action while its score is updated.
  double SelectionPriority(std::uint64_t action_id, double total) override {
    auto [it, inserted] = gumbel_.try_emplace(action_id);
    if (inserted) {
      const double u = 1.0 - unit_(rng_);
      it->second = -std::log(-std::log(u));
    }
    return total / tau_ + it->second;
  }

  void ReleaseSelectionPriority(std::uint64_t action_id) override {
    gumbel_.erase(action_id);
  }

  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override {
    (void)admissible;
    if (totals.empty()) return std::nullopt;
    const double mx = *std::max_element(totals.begin(), totals.end());
    std::vector<double> w(totals.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < totals.size(); ++i) {
      w[i] = std::exp((totals[i] - mx) / tau_);
      sum += w[i];
    }
    std::uniform_real_distribution<double> u(0.0, sum);
    const double pick = u(rng_);
    double acc = 0.0;
    for (std::size_t i = 0; i < totals.size(); ++i) {
      acc += w[i];
      if (pick < acc) return i;
    }
    return totals.size() - 1;
  }

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    std::vector<double> totals;
    std::vector<char> admissible;
    for (const auto& c : cands) {
      totals.push_back(c.total);
      admissible.push_back(c.admissible);
    }
    return SelectScores(totals, admissible);
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
  std::uniform_real_distribution<double> unit_{0.0, 1.0};
  std::unordered_map<std::uint64_t, double> gumbel_;
  double tau_;
};

// Hybrid neuro-symbolic: hard-filter to `admissible` motifs first (the
// symbolic guarantee — d(p0) < inf and the conclusion is new; deduction
// stays complete, Corollary 4), then argmax of the score inside.
class HybridNeuroSymbolicPolicy final : public Policy {
 public:
  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }
  bool RequiresAdmissible() const override { return true; }

  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < totals.size(); ++i) {
      if (!admissible[i]) continue;
      if (!best || totals[i] > totals[*best]) best = i;
    }
    return best;
  }

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override {
    std::vector<double> totals;
    std::vector<char> admissible;
    for (const auto& c : cands) {
      totals.push_back(c.total);
      admissible.push_back(c.admissible);
    }
    return SelectScores(totals, admissible);
  }
};

// Learned action-conditioned cost-to-go ordering for AdaDeduce. The model is
// ordering-only; symbolic admissibility and exact predicate evaluation stay
// in the executor. Non-owning pointer: ReasoningAgent owns the model.
class LearnedQPolicy final : public Policy {
 public:
  explicit LearnedQPolicy(const ValueModel* model) : model_(model) {}

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }
  bool RequiresAdmissible() const override { return true; }
  double SelectionPriorityWithFeatures(
      std::uint64_t action_id, double total,
      const std::array<double, 11>& features) override;
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override;
  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override;

 private:
  const ValueModel* model_;
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
// NOTE: the agent has a single executor (AdaDeduceExecutor / Algorithm 6);
// Jev ranks candidates inside its ready-set selection, which is the §5.1
// policy point.
class JevPolicy final : public Policy {
 public:
  JevPolicy(JevConfig cfg, std::unique_ptr<JevClient> client,
            const Goal* goal, const gar::core::graph::TripleGraph* g,
            const gar::core::analysis::DependencyGraph* dg,
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
  const gar::core::graph::TripleGraph* g_;
  const gar::core::analysis::DependencyGraph* dg_;
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
  std::uint32_t goal_pred_ = gar::core::analysis::kInvalidPredicate;
  void EnsureGoalTables();
};

}  // namespace agent
}  // namespace gar::core::engine
