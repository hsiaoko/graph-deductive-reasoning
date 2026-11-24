#pragma once

// Policy: ordered action-batch selection (paper §5.1). The single-action path
// remains available as batch size 1 and as the optimized executor fast path.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/agent/goal.h"
#include "gar/core/agent/guide.h"
#include "gar/core/agent/jev_client.h"
#include "gar/core/agent/motif.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule.h"

namespace gar::core::engine {
namespace agent {

struct BatchCandidate {
  const ScoredMotif* score;
  std::uint64_t action_id = 0;
  // Cached selection priority (Score plus policy perturbation) maintained by
  // ScoreReady. Policies that rank by this priority can skip re-scoring.
  double priority = 0.0;
};

class Policy {
 public:
  virtual ~Policy() = default;
  // Picks an index into `cands`, or nullopt to decline all (the executor
  // then terminates with "no_candidates").
  [[nodiscard]] virtual std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) = 0;
  // Returns up to max_actions distinct ordered indices. All draws observe the
  // same batch-start scores and non-owning score views.
  [[nodiscard]] virtual std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands, std::size_t max_actions) {
    std::vector<std::size_t> remaining(cands.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
    std::vector<std::size_t> selected;
    while (!remaining.empty() && selected.size() < max_actions) {
      std::vector<ScoredMotif> current;
      current.reserve(remaining.size());
      for (const std::size_t index : remaining) {
        current.push_back(*cands[index].score);
      }
      const auto pick = Select(current);
      if (!pick) break;
      selected.push_back(remaining[*pick]);
      remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(*pick));
    }
    return selected;
  }
  // Score-only selection avoids copying every embedding when the executor
  // maintains current scores incrementally. Policies that need full dossiers
  // (Jev) keep the default and use Select above instead.
  virtual bool SupportsScoreView() const { return false; }
  virtual bool PrefersHighestScore() const { return false; }
  virtual double SelectionPriority(std::uint64_t action_id, double total) {
    (void)action_id;
    return total;
  }
  virtual double SelectionPriorityForCandidate(
      std::uint64_t action_id, const ScoredMotif& candidate) {
    return SelectionPriority(action_id, candidate.total);
  }
  virtual double GuidePriority(const ScoredMotif& candidate) {
    (void)candidate;
    return 0.0;
  }
  virtual void ReleaseSelectionPriority(std::uint64_t action_id) {
    (void)action_id;
  }
  virtual void OnSelection(std::uint64_t action_id) { (void)action_id; }
  virtual std::optional<std::uint64_t> ForcedAction() { return std::nullopt; }
  virtual bool RequiresAdmissible() const { return false; }
  [[nodiscard]] virtual std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) {
    (void)totals;
    (void)admissible;
    return std::nullopt;
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

  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands,
      std::size_t max_actions) override {
    std::vector<std::size_t> indices(cands.size());
    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a,
                                                         std::size_t b) {
      return cands[a].score->total > cands[b].score->total;
    });
    indices.resize(std::min(max_actions, indices.size()));
    return indices;
  }
};

struct ReplayAction {
  std::uint64_t action_id = 0;
  Motif motif;
};

class ReplayPolicy final : public Policy {
 public:
  explicit ReplayPolicy(std::vector<ReplayAction> actions)
      : actions_(std::move(actions)) {}

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }
  std::optional<std::uint64_t> ForcedAction() override {
    if (next_ >= actions_.size()) return std::nullopt;
    return actions_[next_].action_id;
  }
  void OnSelection(std::uint64_t action_id) override;
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override;

 private:
  std::vector<ReplayAction> actions_;
  std::size_t next_ = 0;
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

  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands,
      std::size_t max_actions) override;

 private:
  std::mt19937 rng_;
  std::uniform_real_distribution<double> unit_{0.0, 1.0};
  std::unordered_map<std::uint64_t, double> gumbel_;
  double tau_;
};

class UniformRandomPolicy final : public Policy {
 public:
  explicit UniformRandomPolicy(std::uint64_t seed) : rng_(seed) {}

  bool SupportsScoreView() const override { return true; }

  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override {
    (void)admissible;
    if (totals.empty()) return std::nullopt;
    return std::uniform_int_distribution<std::size_t>(0, totals.size() - 1)(rng_);
  }

  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& candidates) override {
    if (candidates.empty()) return std::nullopt;
    return std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(rng_);
  }

  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& candidates,
      std::size_t max_actions) override {
    std::vector<std::size_t> indices(candidates.size());
    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng_);
    indices.resize(std::min(max_actions, indices.size()));
    return indices;
  }

 private:
  std::mt19937 rng_;
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

  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands,
      std::size_t max_actions) override {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      if (cands[i].score->admissible) indices.push_back(i);
    }
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a,
                                                         std::size_t b) {
      return cands[a].score->total > cands[b].score->total;
    });
    indices.resize(std::min(max_actions, indices.size()));
    return indices;
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
// NOTE: the agent has a single executor (AdaDeduceExecutor / Algorithm 6);
// Jev ranks candidates inside its ready-set selection, which is the §5.1
// policy point.
class JevPolicy final : public Policy {
 public:
  JevPolicy(JevConfig cfg, std::unique_ptr<JevClient> client,
            const Goal* goal, const gar::core::graph::Graph* g,
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
  const gar::core::graph::Graph* g_;
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

// LlmStarGuide issues at most one decision-model request per goal. It assigns
// a preference in [0,1] to each eligible rule star whose consequent can reach
// the goal; motif selection remains with the injected local policy and every
// logical check stays symbolic.
class LlmStarGuide {
 public:
  LlmStarGuide(std::unique_ptr<LlmClient> client, const Goal* goal,
               const gar::core::graph::Graph* g,
               const gar::core::analysis::DependencyGraph* dg,
               const std::vector<GARule>* rules,
               const std::vector<std::string>* rule_ids,
               std::size_t max_options = 255,
               double min_estimated_cost = 0.0)
      : client_(std::move(client)),
        goal_(goal),
        g_(g),
        dg_(dg),
        rules_(rules),
        rule_ids_(rule_ids),
        max_options_(max_options),
        min_estimated_cost_(min_estimated_cost) {}

  const std::vector<double>& Preferences(
      const std::vector<std::uint32_t>& eligible);
  [[nodiscard]] std::uint64_t calls() const { return calls_; }
  [[nodiscard]] double total_wall_ms() const { return wall_ms_; }
  [[nodiscard]] std::uint64_t fallbacks() const { return fallbacks_; }
  [[nodiscard]] std::uint64_t reachable() const { return reachable_; }
  [[nodiscard]] std::uint64_t options() const { return options_; }
  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] double estimated_cost() const { return estimated_cost_; }

 private:
  std::unique_ptr<LlmClient> client_;
  const Goal* goal_;
  const gar::core::graph::Graph* g_;
  const gar::core::analysis::DependencyGraph* dg_;
  const std::vector<GARule>* rules_;
  const std::vector<std::string>* rule_ids_;
  std::size_t max_options_;
  double min_estimated_cost_;
  std::vector<double> preferences_;
  bool initialized_ = false;
  std::uint64_t calls_ = 0;
  double wall_ms_ = 0.0;
  std::uint64_t fallbacks_ = 0;
  std::uint64_t reachable_ = 0;
  std::uint64_t options_ = 0;
  bool active_ = false;
  double estimated_cost_ = 0.0;
  bool warned_ = false;
};

// PaperGuide compiles at most one action-level guide per goal. Failures produce
// an empty guide and retain the local policy.
class PaperGuide {
 public:
  PaperGuide(std::unique_ptr<LlmClient> client, const Goal* goal,
             const gar::core::graph::Graph* g,
             const gar::core::analysis::DependencyGraph* dg,
             const std::vector<GARule>* rules,
             const std::vector<std::string>* rule_ids,
             std::size_t max_options = 64)
      : client_(std::move(client)), goal_(goal), g_(g), dg_(dg),
        rules_(rules), rule_ids_(rule_ids), max_options_(max_options) {}

  void Prepare();
  [[nodiscard]] const CompiledGuide& guide() const { return guide_; }
  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] std::uint64_t calls() const { return calls_; }
  [[nodiscard]] double total_wall_ms() const { return wall_ms_; }
  [[nodiscard]] std::uint64_t fallbacks() const { return fallbacks_; }
  [[nodiscard]] std::size_t rejected() const { return guide_.rejected(); }
  [[nodiscard]] const std::string& raw_response() const { return raw_response_; }

 private:
  std::unique_ptr<LlmClient> client_;
  const Goal* goal_;
  const gar::core::graph::Graph* g_;
  const gar::core::analysis::DependencyGraph* dg_;
  const std::vector<GARule>* rules_;
  const std::vector<std::string>* rule_ids_;
  std::size_t max_options_;
  GuideFeatureCatalog catalog_;
  CompiledGuide guide_;
  std::string raw_response_;
  bool initialized_ = false;
  bool active_ = false;
  std::uint64_t calls_ = 0;
  double wall_ms_ = 0.0;
  std::uint64_t fallbacks_ = 0;
  bool warned_ = false;
};

class GuidedPolicy final : public Policy {
 public:
  GuidedPolicy(std::unique_ptr<Policy> base, const PaperGuide* guide,
               const Goal* goal, const gar::core::graph::Graph* g,
               const gar::core::analysis::DependencyGraph* dg,
               const std::vector<GARule>* rules,
               const std::vector<std::string>* rule_ids,
               double lambda, std::uint64_t seed)
      : base_(std::move(base)), guide_(guide), goal_(goal), g_(g), dg_(dg),
        rules_(rules), rule_ids_(rule_ids), lambda_(lambda), rng_(seed) {}

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }
  double GuidePriority(const ScoredMotif& candidate) override {
    return UseGuide() ? Priority(candidate) : 0.0;
  }
  bool RequiresAdmissible() const override {
    return base_->RequiresAdmissible();
  }
  double SelectionPriority(std::uint64_t action_id, double total) override;
  double SelectionPriorityForCandidate(
      std::uint64_t action_id, const ScoredMotif& candidate) override;
  void ReleaseSelectionPriority(std::uint64_t action_id) override;
  std::optional<std::uint64_t> ForcedAction() override {
    return base_->ForcedAction();
  }
  void OnSelection(std::uint64_t action_id) override {
    base_->OnSelection(action_id);
  }
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override;
  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override;
  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands,
      std::size_t max_actions) override;
  void OnApplication(const Motif& mu, bool success, double cost) override {
    base_->OnApplication(mu, success, cost);
  }

 private:
  bool UseGuide() const {
    return guide_ != nullptr && guide_->active() && lambda_ > 0.0;
  }
  bool UseGuideThisBatch();
  double Priority(const ScoredMotif& candidate);
  GuideContext Context(const ScoredMotif& candidate);
  void EnsureDistances();

  std::unique_ptr<Policy> base_;
  const PaperGuide* guide_;
  const Goal* goal_;
  const gar::core::graph::Graph* g_;
  const gar::core::analysis::DependencyGraph* dg_;
  const std::vector<GARule>* rules_;
  const std::vector<std::string>* rule_ids_;
  double lambda_;
  std::mt19937 rng_;
  std::vector<std::uint32_t> distances_;
};

// Uses the fallback policy when the symbolic cost gate is closed, and the
// injected guided policy only after an active initial guide.
class AdaptivePolicy final : public Policy {
 public:
  AdaptivePolicy(std::unique_ptr<Policy> fallback,
                 std::unique_ptr<Policy> guided);
  void SetGuide(const LlmStarGuide* guide) { guide_ = guide; }

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }
  bool RequiresAdmissible() const override;
  double SelectionPriority(std::uint64_t action_id, double total) override;
  double SelectionPriorityForCandidate(
      std::uint64_t action_id, const ScoredMotif& candidate) override;
  void ReleaseSelectionPriority(std::uint64_t action_id) override;
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& cands) override;
  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override;
  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& cands,
      std::size_t max_actions) override;
  void OnApplication(const Motif& mu, bool success, double cost) override;

 private:
  Policy& Current();
  const Policy& Current() const;
  std::unique_ptr<Policy> fallback_;
  std::unique_ptr<Policy> guided_;
  const LlmStarGuide* guide_ = nullptr;
};

}  // namespace agent
}  // namespace gar::core::engine
