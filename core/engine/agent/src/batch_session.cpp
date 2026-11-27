#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "deduce_session.h"

namespace gar::core::engine::agent {

std::vector<deduce::ActionRecord> DeduceSession::SelectReadyBatch() {
  auto& fx = *res_.stats;
  std::vector<deduce::ActionRecord*> candidates;
  candidates.reserve(ws_.ready().size());
  for (auto& rec : ws_.ready()) {
    const bool head_known =
        state_->ContainsEdge(rec.head.slot, rec.motif.embedding[rec.head.pu],
                             rec.motif.embedding[rec.head.pv]);
    rec.score.admissible = rec.score.admissible && !head_known;
    if (kit_.policy->RequiresAdmissible() && !rec.score.admissible) continue;
    candidates.push_back(&rec);
  }
  if (candidates.empty()) return {};

  if (!IncrementalScores()) {
    const auto started = std::chrono::steady_clock::now();
    for (auto* rec : candidates) {
      kit_.scorer->Score(state_->rules()[rec->motif.rule], rec->motif, sctx_,
                         &rec->score);
    }
    fx.score_wall_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    fx.score_calls += candidates.size();
    fx.scored_candidates += candidates.size();
  }

  std::vector<BatchCandidate> views;
  views.reserve(candidates.size());
  for (const auto* rec : candidates) {
    views.push_back(BatchCandidate{&rec->score, rec->id, rec->selection_priority});
  }
  const auto picks = kit_.policy->SelectBatch(views, cfg_.batch_size);
  std::unordered_set<std::size_t> unique;
  for (const std::size_t pick : picks) {
    if (pick >= candidates.size() || !unique.insert(pick).second) {
      throw std::runtime_error("policy returned an invalid batch index");
    }
  }

  std::vector<deduce::ActionRecord> selected;
  selected.reserve(picks.size());
  for (const std::size_t pick : picks) selected.push_back(*candidates[pick]);
  if (cfg_.fair_interval > 0 && selects_since_fair_ >= cfg_.fair_interval) {
    const auto* oldest = *std::min_element(
        candidates.begin(), candidates.end(),
        [](const auto* a, const auto* b) { return a->seq < b->seq; });
    const auto present =
        std::find_if(selected.begin(), selected.end(),
                     [&](const auto& rec) { return rec.id == oldest->id; });
    if (present == selected.end()) {
      if (selected.size() >= cfg_.batch_size && !selected.empty()) {
        selected.pop_back();
      }
      selected.push_back(*oldest);
    }
    selects_since_fair_ = 0;
    ++fx.fair_select_events;
    Trace("fair_select\taction=" + std::to_string(oldest->id));
  }

  ++fx.select_events;
  for (const auto& rec : selected) {
    kit_.policy->OnSelection(rec.id);
    WriteDecisionTrace(rec);
    Trace("select\taction=" + std::to_string(rec.id));
  }
  return selected;
}

void DeduceSession::PrepareStaticBatch(
    const std::vector<deduce::ActionRecord>& batch) {
  for (const auto& rec : batch) {
    const GARule& rule = state_->rules()[rec.motif.rule];
    for (const auto& pred : rule.antecedent) {
      if (IsDynamicPredicate(pred, rule, ctx_)) continue;
      const auto key =
          deduce::MakePreconditionKey(pred, rec.motif.embedding.data());
      if (H_.Lookup(key, true, state_->version())) continue;
      const auto state = deduce::ClassifyPredicate(
          pred, rule, rec.motif.embedding.data(), state_->attrs(), g_, ctx_,
          state_->ml_models());
      H_.Record(key, state == deduce::PrecondState::kStaticTrue, true,
                state_->version());
    }
  }
}

bool DeduceSession::ProcessAction(const deduce::ActionRecord& rec) {
  if (state_->ContainsEdge(rec.head.slot, rec.motif.embedding[rec.head.pu],
                           rec.motif.embedding[rec.head.pv])) {
    Trace("skip-known\taction=" + std::to_string(rec.id));
    return false;
  }
  const auto evaluation_started = std::chrono::steady_clock::now();
  const int verdict = Evaluate(rec);
  res_.stats->evaluation_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - evaluation_started)
          .count();
  kit_.policy->OnApplication(
      rec.motif, verdict == 2,
      kit_.cost_model->RuleLowerBound(state_->rules()[rec.motif.rule]));
  if (verdict == 2) {
    const auto derivation_started = std::chrono::steady_clock::now();
    const auto derived = Derive(rec);
    res_.stats->derivation_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - derivation_started)
            .count();
    if (res_.goal_derived && !cfg_.enumerate_open) return true;
    const auto delta_started = std::chrono::steady_clock::now();
    pending_derivations_.push_back(derived);
    if (pending_derivations_.size() >= 256) FlushDerivations();
    res_.stats->delta_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - delta_started)
            .count();
  } else if (verdict == 0 || verdict == 3) {
    ++res_.stats->prune_events;
    Trace(verdict == 3 ? "prune-cached\taction=" + std::to_string(rec.id)
                       : "prune\taction=" + std::to_string(rec.id));
  }
  return false;
}

}  // namespace gar::core::engine::agent
