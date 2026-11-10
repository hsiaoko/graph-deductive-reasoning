#include <chrono>
#include <cstdio>

#include "gar/engine/agent/policy.h"

#include "gar/analysis/predicate_table.h"
#include "gar/engine/agent/value_model.h"

#include <stdexcept>

namespace gar::core::engine {
namespace agent {

namespace {

std::string FormatDouble(double v, int digits = 3) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
  return buf;
}

}  // namespace

void JevPolicy::EnsureGoalTables() {
  if (!distances_.empty()) return;
  goal_pred_ =
      dg_->table().Find(g_->RelationName(goal_->slot));
  if (goal_pred_ != gar::core::analysis::kInvalidPredicate) {
    distances_ = dg_->DistancesTo(goal_pred_);
  }
}

std::optional<std::size_t> JevPolicy::Select(
    const std::vector<ScoredMotif>& cands) {
  auto fallback = [&](const std::string& reason) {
    ++fallbacks_;
    if (!warned_) {
      std::fprintf(stderr, "[jev] fallback: %s\n", reason.c_str());
      warned_ = true;
    }
    return DeterministicPolicy{}.Select(cands);
  };

  // admissible hard filter (the symbolic guarantee, Corollary 4).
  std::vector<std::size_t> admissible;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (cands[i].admissible) admissible.push_back(i);
  }
  if (admissible.empty()) return std::nullopt;

  // top_k by the handcrafted score (ties: lower index — deterministic).
  std::sort(admissible.begin(), admissible.end(),
            [&](std::size_t x, std::size_t y) {
              if (cands[x].total != cands[y].total) {
                return cands[x].total > cands[y].total;
              }
              return x < y;
            });
  const std::size_t k = std::min(cfg_.top_k, admissible.size());
  admissible.resize(k);

  EnsureGoalTables();
  const auto& table = dg_->table();
  const auto sccs = dg_->StronglyConnectedComponents();

  // ---- state text: goal, remaining budget, one dossier per candidate ----
  std::string state = "goal: " + g_->RelationName(goal_->slot) + "(";
  state += goal_->args[0] ? g_->EntityName(*goal_->args[0]) : "_";
  state += ",";
  state += goal_->args[1] ? g_->EntityName(*goal_->args[1]) : "_";
  state += ")\nbudget remaining: " + FormatDouble(remaining_, 0) + "\n";
  state += "candidates (top " + std::to_string(k) + " of " +
           std::to_string(cands.size()) + ", admissible only):\n";

  std::vector<std::string> options, criteria;
  for (std::size_t i = 0; i < k; ++i) {
    const ScoredMotif& sm = cands[admissible[i]];
    const GARule& rule = (*rules_)[sm.motif.rule];
    const std::string rule_id = rule_ids_ && sm.motif.rule < rule_ids_->size()
                                    ? (*rule_ids_)[sm.motif.rule]
                                    : "r" + std::to_string(sm.motif.rule);
    const std::uint32_t p0 = table.Find(
        gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
    const std::uint32_t dp0 =
        (p0 != gar::core::analysis::kInvalidPredicate && !distances_.empty())
            ? distances_[p0]
            : gar::core::analysis::kUnreachable;
    const std::string d_str = dp0 == gar::core::analysis::kUnreachable
                                  ? "inf"
                                  : std::to_string(dp0);
    const std::string path =
        (p0 != gar::core::analysis::kInvalidPredicate &&
         goal_pred_ != gar::core::analysis::kInvalidPredicate)
            ? dg_->RenderPath(p0, goal_pred_, rule_ids_)
            : "";

    const int fam =
        p0 != gar::core::analysis::kInvalidPredicate ? dg_->RecursiveFamilyOf(p0) : -1;

    state += "- option " + std::to_string(i) + ": rule " + rule_id + " (" +
             rule.name + ")\n";
    if (!rule.description.empty()) {
      state += "    description: " + rule.description + "\n";
    }
    state += "    d(p0)=" + d_str + " price=" + FormatDouble(sm.price) +
             " confidence=" + FormatDouble(rule.quality.confidence) +
             " witnessed: " + std::to_string(sm.premises_witnessed) + "/" +
             std::to_string(sm.premises_total) + "\n";
    state += "    path: " + (path.empty() ? "n/a" : path) + "\n";
    if (fam >= 0) {
      state += "    in recursive family #" + std::to_string(fam) + " (" +
               std::to_string(sccs[fam].members.size()) + " predicates)\n";
    }

    options.push_back(std::to_string(i));
    criteria.push_back("rule " + rule_id + " conf=" +
                       FormatDouble(rule.quality.confidence) + " d=" + d_str +
                       " price=" + FormatDouble(sm.price) +
                       " witnessed: " +
                       std::to_string(sm.premises_witnessed) + "/" +
                       std::to_string(sm.premises_total) +
                       " rel=" + FormatDouble(sm.rel) +
                       " score=" + FormatDouble(sm.total) +
                       " recursive_family=" + std::to_string(fam));
  }

  const std::string instructions =
      "Rank, do not invent: pick the candidate motif most likely to derive a "
      "new fact that serves the goal. Candidates and all logical checks are "
      "already settled symbolically.";

  const auto t0 = std::chrono::steady_clock::now();
  JevChoiceResult res;
  try {
    res = client_->Choice(state, instructions, options, criteria);
    wall_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    ++calls_;
  } catch (const std::exception& e) {
    return fallback(e.what());
  }

  // The returned probabilities are pi; validate alignment.
  if (res.probabilities.size() != options.size()) {
    return fallback("probabilities/options size mismatch");
  }
  double sum = 0.0, best = -1.0;
  std::size_t best_i = 0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    sum += res.probabilities[i];
    if (res.probabilities[i] > best) {
      best = res.probabilities[i];
      best_i = i;
    }
  }
  if (sum <= 0.0) return fallback("degenerate (all-zero) probabilities");

  if (!cfg_.sample) {
    return admissible[best_i];  // argmax pi, ties -> lower index
  }
  std::uniform_real_distribution<double> u(0.0, sum);
  const double pick = u(rng_);
  double acc = 0.0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    acc += res.probabilities[i];
    if (pick < acc) return admissible[i];
  }
  return admissible.back();
}

}  // namespace agent
}  // namespace gar::core::engine


namespace gar::core::engine {
namespace agent {

double LearnedQPolicy::SelectionPriorityWithFeatures(
    std::uint64_t action_id, double total,
    const std::array<double, 11>& features) {
  (void)action_id;
  (void)total;
  return -model_->PredictRemaining(features);
}

std::optional<std::size_t> LearnedQPolicy::Select(
    const std::vector<ScoredMotif>& cands) {
  (void)cands;
  throw std::logic_error("LearnedQPolicy requires AdaDeduce feature selection");
}

std::optional<std::size_t> LearnedQPolicy::SelectScores(
    const std::vector<double>& totals, const std::vector<char>& admissible) {
  (void)totals;
  (void)admissible;
  throw std::logic_error("LearnedQPolicy requires AdaDeduce feature selection");
}

}  // namespace agent
}  // namespace gar::core::engine
