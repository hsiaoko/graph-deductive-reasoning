#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "gar/core/agent/fact_encoder.h"
#include "gar/core/agent/learning/relevance_model.h"
#include "gar/core/agent/policy.h"

namespace gar::core::engine::agent::learning {

class LearnedPolicy final : public Policy {
 public:
  LearnedPolicy(const RelevanceModel* model, FactEncoder encoder, Goal goal,
                const std::vector<GARule>* rules,
                double handcrafted_weight = 0.0, double relevance_weight = 1.0,
                bool competitive_gate = false)
      : model_(model),
        encoder_(std::move(encoder)),
        goal_(goal),
        rules_(rules),
        handcrafted_weight_(handcrafted_weight),
        relevance_weight_(relevance_weight),
        competitive_gate_(competitive_gate),
        goal_vector_(encoder_.EncodeGoal(goal_)),
        goal_projection_(model_->ProjectGoal(goal_vector_)) {}

  ~LearnedPolicy() override;

  bool SupportsScoreView() const override { return true; }
  bool PrefersHighestScore() const override { return true; }

  double SelectionPriorityForCandidate(std::uint64_t action_id,
                                       const ScoredMotif& candidate) override;
  void ReleaseSelectionPriority(std::uint64_t action_id) override;
  std::optional<std::size_t> Select(
      const std::vector<ScoredMotif>& candidates) override;
  std::optional<std::size_t> SelectScores(
      const std::vector<double>& totals,
      const std::vector<char>& admissible) override;
  std::vector<std::size_t> SelectBatch(
      const std::vector<BatchCandidate>& candidates,
      std::size_t max_actions) override;

 private:
  double Predict(const ScoredMotif& candidate);
  double Score(const ScoredMotif& candidate);

  const RelevanceModel* model_;
  FactEncoder encoder_;
  Goal goal_;
  const std::vector<GARule>* rules_;
  double handcrafted_weight_;
  double relevance_weight_;
  // Competitive gate: at enqueue, skip the per-fact HDC encode/project for any
  // candidate whose blended-score ceiling (hw*f + rel, Predict is a sigmoid)
  // cannot beat the current live heap max. Such a candidate can never be the
  // argmax now, so it keeps the cheap f priority; the model only encodes
  // competitive facts. This is the bulk of the learned wall overhead. The gate
  // is a heuristic: a cold candidate is not re-scored when the heap max later
  // drops, so ranking is approximate (measured against derived count).
  bool competitive_gate_;
  std::unordered_map<std::uint64_t, double> live_priority_;
  std::multiset<double> live_priorities_;
  gar::core::hdc::DenseVector goal_vector_;
  RelevanceModel::RelevanceProjection goal_projection_;
  std::unordered_map<FactKey, RelevanceModel::RelevanceProjection, FactKeyHash>
      action_projections_;
  std::unordered_map<FactKey, double, FactKeyHash> action_cosines_;
  // GAR_DERIVE_PROFILE-gated per-fact cost breakdown (encode vs project vs
  // cosine vs MLP), printed on destruction; isolates the encoding overhead.
  mutable std::atomic<std::uint64_t> encode_ns_{0};
  mutable std::atomic<std::uint64_t> project_ns_{0};
  mutable std::atomic<std::uint64_t> cosine_ns_{0};
  mutable std::atomic<std::uint64_t> predict_ns_{0};
};

}  // namespace gar::core::engine::agent::learning
