#include "gar/core/agent/learning/learned_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "gar/core/agent/motif.h"

namespace gar::core::engine::agent::learning {

namespace {
bool ProfileEnabled() {
  static const bool enabled = std::getenv("GAR_DERIVE_PROFILE") != nullptr;
  return enabled;
}
}  // namespace

LearnedPolicy::~LearnedPolicy() {
  if (ProfileEnabled()) {
    std::fprintf(stderr,
                 "[learned-profile] unique_facts=%zu encode=%.1fms project=%.1fms "
                 "cosine=%.1fms predict=%.1fms\n",
                 action_projections_.size(), encode_ns_.load() / 1.0e6,
                 project_ns_.load() / 1.0e6, cosine_ns_.load() / 1.0e6,
                 predict_ns_.load() / 1.0e6);
  }
}

double LearnedPolicy::Predict(const ScoredMotif& candidate) {
  const GARule& rule = (*rules_)[candidate.motif.rule];
  const RuleHead head = ResolveRuleHead(rule);
  if (!head.valid)
    throw std::invalid_argument("learned policy candidate has no rule head");
  const Fact consequent =
      MakeEdgeFact(head.slot, candidate.motif.embedding[head.pu],
                   candidate.motif.embedding[head.pv]);
  const FactKey key = KeyOf(consequent);
  auto [it, inserted] = action_projections_.try_emplace(key);
  if (inserted) {
    const bool prof = ProfileEnabled();
    const auto t0 = std::chrono::steady_clock::now();
    const auto action_vector = encoder_.Encode(consequent);
    const auto t1 = std::chrono::steady_clock::now();
    it->second = model_->ProjectAction(action_vector);
    const auto t2 = std::chrono::steady_clock::now();
    // Encode() and EncodeGoal() both return L2-normalized vectors, so the
    // cosine is just the dot product. The previous per-fact Norm() calls
    // recomputed unit norms (2 extra 128-dim dots + 2 sqrt) for nothing.
    action_cosines_[key] = action_vector.Dot(goal_vector_);
    const auto t3 = std::chrono::steady_clock::now();
    if (prof) {
      encode_ns_.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      project_ns_.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
      cosine_ns_.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    }
  }
  const auto p0 = std::chrono::steady_clock::now();
  const double result = model_->PredictProjected(
      it->second, goal_projection_, candidate.total, action_cosines_[key]);
  if (ProfileEnabled()) {
    predict_ns_.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - p0)
            .count());
  }
  return result;
}

double LearnedPolicy::Score(const ScoredMotif& candidate) {
  return handcrafted_weight_ * candidate.total +
         relevance_weight_ * Predict(candidate);
}

double LearnedPolicy::SelectionPriorityForCandidate(
    std::uint64_t action_id, const ScoredMotif& candidate) {
  if (!competitive_gate_) return Score(candidate);
  const double f = candidate.total;
  // Predict is a sigmoid in (0,1), so hw*f + rel is a strict upper bound on the
  // blended score. If even that ceiling is below the live heap max, the model
  // cannot promote this candidate to argmax now -- skip its encode/project.
  const double ceiling = handcrafted_weight_ * f + relevance_weight_;
  auto it = live_priority_.find(action_id);
  if (it != live_priority_.end())
    live_priorities_.erase(live_priorities_.find(it->second));
  double priority;
  if (!live_priorities_.empty() && ceiling < *live_priorities_.rbegin()) {
    priority = f;
  } else {
    priority = Score(candidate);
  }
  live_priority_[action_id] = priority;
  live_priorities_.insert(priority);
  return priority;
}

void LearnedPolicy::ReleaseSelectionPriority(std::uint64_t action_id) {
  const auto it = live_priority_.find(action_id);
  if (it == live_priority_.end()) return;
  live_priorities_.erase(live_priorities_.find(it->second));
  live_priority_.erase(it);
}

std::optional<std::size_t> LearnedPolicy::Select(
    const std::vector<ScoredMotif>& candidates) {
  if (candidates.empty()) return std::nullopt;
  std::size_t best = 0;
  double best_score = Score(candidates[0]);
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    const double score = Score(candidates[i]);
    if (score > best_score) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

std::optional<std::size_t> LearnedPolicy::SelectScores(
    const std::vector<double>& totals, const std::vector<char>& admissible) {
  (void)totals;
  (void)admissible;
  throw std::logic_error("LearnedPolicy requires scored candidate motifs");
}

std::vector<std::size_t> LearnedPolicy::SelectBatch(
    const std::vector<BatchCandidate>& candidates, std::size_t max_actions) {
  std::vector<std::pair<double, std::size_t>> predictions;
  predictions.reserve(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    predictions.emplace_back(candidates[i].priority, i);
  }
  std::stable_sort(predictions.begin(), predictions.end(),
                   [&](const auto& left, const auto& right) {
                     if (left.first != right.first)
                       return left.first > right.first;
                     return candidates[left.second].action_id <
                            candidates[right.second].action_id;
                   });
  std::vector<std::size_t> indices;
  indices.reserve(std::min(max_actions, predictions.size()));
  for (const auto& [prediction, index] : predictions) {
    (void)prediction;
    indices.push_back(index);
  }
  indices.resize(std::min(max_actions, indices.size()));
  return indices;
}

}  // namespace gar::core::engine::agent::learning
