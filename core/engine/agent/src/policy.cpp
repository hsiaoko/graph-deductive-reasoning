#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "gar/core/agent/policy.h"

#include "gar/core/analysis/predicate_table.h"
#include "rule_support.h"

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

std::vector<std::size_t> RandomizedPolicy::SelectBatch(
    const std::vector<BatchCandidate>& cands, std::size_t max_actions) {
  std::vector<std::size_t> remaining(cands.size());
  for (std::size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
  std::vector<std::size_t> selected;
  while (!remaining.empty() && selected.size() < max_actions) {
    double maximum = cands[remaining.front()].score->total;
    for (const std::size_t index : remaining) {
      maximum = std::max(maximum, cands[index].score->total);
    }
    std::vector<double> weights;
    weights.reserve(remaining.size());
    double sum = 0.0;
    for (const std::size_t index : remaining) {
      const double weight =
          std::exp((cands[index].score->total - maximum) / tau_);
      weights.push_back(weight);
      sum += weight;
    }
    std::uniform_real_distribution<double> distribution(0.0, sum);
    const double draw = distribution(rng_);
    double cumulative = 0.0;
    std::size_t position = remaining.size() - 1;
    for (std::size_t i = 0; i < remaining.size(); ++i) {
      cumulative += weights[i];
      if (draw < cumulative) {
        position = i;
        break;
      }
    }
    selected.push_back(remaining[position]);
    remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return selected;
}

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

const std::vector<double>& LlmStarGuide::Preferences(
    const std::vector<std::uint32_t>& eligible) {
  if (initialized_) return preferences_;
  initialized_ = true;
  preferences_.assign(rules_->size(), 0.0);
  auto fallback = [&](const std::string& reason) {
    ++fallbacks_;
    active_ = false;
    std::fill(preferences_.begin(), preferences_.end(), 0.0);
    if (!warned_) {
      std::fprintf(stderr, "[guide] fallback: %s\n", reason.c_str());
      warned_ = true;
    }
  };

  std::vector<std::string> options;
  std::vector<std::string> criteria;
  std::vector<std::uint32_t> rule_indices;
  const std::uint32_t goal_pred =
      dg_->table().Find(g_->RelationName(goal_->slot));
  const auto distances = goal_pred != gar::core::analysis::kInvalidPredicate
                             ? dg_->DistancesTo(goal_pred)
                             : std::vector<std::uint32_t>{};
  for (const std::uint32_t rule_index : eligible) {
    if (rule_index >= rules_->size()) continue;
    const GARule& rule = (*rules_)[rule_index];
    const std::string id = rule_ids_ && rule_index < rule_ids_->size()
                               ? (*rule_ids_)[rule_index]
                               : "r" + std::to_string(rule_index);
    const std::uint32_t p0 = dg_->table().Find(
        gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
    const std::uint32_t distance =
        p0 < distances.size() ? distances[p0]
                              : gar::core::analysis::kUnreachable;
    if (distance == gar::core::analysis::kUnreachable) continue;
    ++reachable_;
    estimated_cost_ += EstimateStarCost(rule, *g_);
    if (options.size() >= max_options_) continue;
    std::string criterion = "rule " + id + ": " + rule.name +
                            "\nconfidence=" +
                            FormatDouble(rule.quality.confidence) +
                            " d=" + std::to_string(distance) +
                            " guide_score: " +
                            FormatDouble(rule.quality.confidence, 6);
    if (!rule.description.empty()) {
      criterion += "\ndescription: " + rule.description;
    }
    if (p0 < distances.size() &&
        goal_pred != gar::core::analysis::kInvalidPredicate) {
      criterion += "\npath: " + dg_->RenderPath(p0, goal_pred, rule_ids_);
    }
    options.push_back(std::to_string(rule_index));
    criteria.push_back(std::move(criterion));
    rule_indices.push_back(rule_index);
  }
  options_ = options.size();
  if (options.empty()) return preferences_;
  active_ = estimated_cost_ >= min_estimated_cost_;
  if (!active_) return preferences_;

  const std::string state =
      "goal: " + g_->RelationName(goal_->slot) + "(" +
      (goal_->args[0] ? g_->EntityName(*goal_->args[0]) : "_") + "," +
      (goal_->args[1] ? g_->EntityName(*goal_->args[1]) : "_") + ")\n" +
      "graph vertices: " + std::to_string(g_->num_entities()) +
      " edges: " + std::to_string(g_->num_edges()) + "\n" +
      "This is the only guide request for this goal.";
  const std::string instructions =
      "Rank the rule stars by how promising their consequents and dependency "
      "paths are for the goal. Return a preference for every option. Do not "
      "invent facts or rules; exact deduction is performed separately.";

  LlmChoiceResult result;
  const auto started = std::chrono::steady_clock::now();
  try {
    result = client_->Choice(state, instructions, options, criteria);
    wall_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    ++calls_;
  } catch (const std::exception& e) {
    wall_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    ++calls_;
    fallback(e.what());
    return preferences_;
  }
  if (result.probabilities.size() != options.size()) {
    fallback("probabilities/options size mismatch");
    return preferences_;
  }
  double best = 0.0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const double p = result.probabilities[i];
    if (!std::isfinite(p) || p < 0.0) {
      fallback("invalid rule preference");
      return preferences_;
    }
    preferences_[rule_indices[i]] = p;
    best = std::max(best, p);
  }
  if (best == 0.0) {
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (result.choice == options[i]) {
        preferences_[rule_indices[i]] = 1.0;
        best = 1.0;
        break;
      }
    }
  }
  if (best == 0.0) {
    fallback("degenerate (all-zero) rule preferences");
    return preferences_;
  }
  for (const std::uint32_t rule_index : rule_indices) {
    preferences_[rule_index] /= best;
  }
  return preferences_;
}

AdaptivePolicy::AdaptivePolicy(std::unique_ptr<Policy> fallback,
                               std::unique_ptr<Policy> guided)
    : fallback_(std::move(fallback)), guided_(std::move(guided)) {}

Policy& AdaptivePolicy::Current() {
  return guide_ != nullptr && guide_->active() ? *guided_ : *fallback_;
}

const Policy& AdaptivePolicy::Current() const {
  return guide_ != nullptr && guide_->active() ? *guided_ : *fallback_;
}

bool AdaptivePolicy::RequiresAdmissible() const {
  return Current().RequiresAdmissible();
}

double AdaptivePolicy::SelectionPriority(std::uint64_t action_id, double total) {
  return Current().SelectionPriority(action_id, total);
}

double AdaptivePolicy::SelectionPriorityForCandidate(
    std::uint64_t action_id, const ScoredMotif& candidate) {
  return Current().SelectionPriorityForCandidate(action_id, candidate);
}

void AdaptivePolicy::ReleaseSelectionPriority(std::uint64_t action_id) {
  Current().ReleaseSelectionPriority(action_id);
}

std::optional<std::size_t> AdaptivePolicy::Select(
    const std::vector<ScoredMotif>& cands) {
  return Current().Select(cands);
}

std::optional<std::size_t> AdaptivePolicy::SelectScores(
    const std::vector<double>& totals, const std::vector<char>& admissible) {
  return Current().SelectScores(totals, admissible);
}

std::vector<std::size_t> AdaptivePolicy::SelectBatch(
    const std::vector<BatchCandidate>& cands, std::size_t max_actions) {
  return Current().SelectBatch(cands, max_actions);
}

void AdaptivePolicy::OnApplication(const Motif& mu, bool success, double cost) {
  Current().OnApplication(mu, success, cost);
}

}  // namespace agent
}  // namespace gar::core::engine


namespace gar::core::engine {
namespace agent {

void ReplayPolicy::OnSelection(std::uint64_t action_id) {
  if (next_ >= actions_.size()) return;
  if (actions_[next_].action_id != action_id) {
    throw std::runtime_error("replay action is not selected at decision " +
                             std::to_string(next_));
  }
  ++next_;
}

std::optional<std::size_t> ReplayPolicy::Select(
    const std::vector<ScoredMotif>& cands) {
  (void)cands;
  throw std::logic_error("ReplayPolicy requires incremental action IDs");
}

}  // namespace agent
}  // namespace gar::core::engine
