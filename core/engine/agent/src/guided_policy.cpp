#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "gar/core/agent/policy.h"
#include "gar/core/analysis/predicate_table.h"

namespace gar::core::engine::agent {

void PaperGuide::Prepare() {
  if (initialized_) return;
  initialized_ = true;
  std::unordered_set<std::string> valid_rules;
  std::vector<std::string> ordered_ids;
  std::string state = "goal: " + g_->RelationName(goal_->slot) + "(" +
      (goal_->args[0] ? g_->EntityName(*goal_->args[0]) : "_") + "," +
      (goal_->args[1] ? g_->EntityName(*goal_->args[1]) : "_") + ")\n" +
      "graph vertices: " + std::to_string(g_->num_entities()) +
      " edges: " + std::to_string(g_->num_edges()) + "\nrules:\n";
  const std::uint32_t goal_pred =
      dg_->table().Find(g_->RelationName(goal_->slot));
  const auto distances = goal_pred == gar::core::analysis::kInvalidPredicate
      ? std::vector<std::uint32_t>{}
      : dg_->DistancesTo(goal_pred);
  for (std::size_t i = 0; i < rules_->size(); ++i) {
    if (ordered_ids.size() >= max_options_) break;
    const GARule& rule = (*rules_)[i];
    if (!rule.consequent || distances.empty()) continue;
    const auto pred = dg_->table().Find(
        gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
    if (pred == gar::core::analysis::kInvalidPredicate ||
        distances[pred] == gar::core::analysis::kUnreachable) {
      continue;
    }
    const std::string id = rule_ids_ != nullptr && i < rule_ids_->size()
        ? (*rule_ids_)[i]
        : "r" + std::to_string(i);
    ordered_ids.push_back(id);
    valid_rules.insert(id);
    valid_rules.insert(rule.name);
    state += "- option " + std::to_string(ordered_ids.size() - 1) + ": " + id +
             ": " + rule.name + "\nconfidence=" +
        std::to_string(rule.quality.confidence) + "\n";
    if (!rule.description.empty()) state += rule.description + "\n";
    state += "distance=" + std::to_string(distances[pred]) + "\n";
    state += "path: " + dg_->RenderPath(pred, goal_pred, rule_ids_) + "\n";
  }
  const std::string instructions =
      "Compile domain preferences into restricted action-priority rules. "
      "Use only registered features: rule.confidence, rule.support, "
      "rule.body_edges, goal.distance, score.total, score.price, "
      "score.residual_cost, evidence.witnessed, evidence.total, "
      "evidence.ratio, binding.N.id, binding.N.label, binding.N.kind. "
      "Conditions are [feature, comparison, feature_or_literal]. "
      "Each rule field must be the integer index of one supplied option, not a "
      "semantic alias or a gar id. Weights must be in [-1,1]. Return compact "
      "JSON with at most 16 priority rules and no explanations. Do not invent "
      "facts, features, or option indices.";

  const auto started = std::chrono::steady_clock::now();
  try {
    const std::string response = client_->Guide(state, instructions);
    raw_response_ = response;
    ++calls_;
    auto root = nlohmann::json::parse(response);
    if (root.is_object() && root.contains("rules") &&
        root["rules"].is_array()) {
      for (auto& entry : root["rules"]) {
        if (!entry.is_object() || !entry.contains("rule")) continue;
        std::size_t index = ordered_ids.size();
        if (entry["rule"].is_number_integer()) {
          const auto value = entry["rule"].get<long long>();
          if (value >= 0) index = static_cast<std::size_t>(value);
        } else if (entry["rule"].is_string()) {
          const auto text = entry["rule"].get<std::string>();
          if (!text.empty() && std::all_of(text.begin(), text.end(),
                                           [](unsigned char c) {
                                             return std::isdigit(c);
                                           })) {
            index = std::stoul(text);
          }
        }
        if (index < ordered_ids.size()) entry["rule"] = ordered_ids[index];
        if (entry.contains("when") && entry["when"].is_array()) {
          for (auto& condition : entry["when"]) {
            if (condition.is_array() && condition.size() == 3 &&
                condition[1].is_string() && condition[1] == "==") {
              condition[1] = "=";
            }
          }
        }
      }
    }
    guide_ = CompileGuide(root.dump(), catalog_, valid_rules);
    active_ = !guide_.empty();
    if (!active_ && guide_.rejected() > 0) ++fallbacks_;
  } catch (const std::exception& e) {
    ++fallbacks_;
    guide_ = CompiledGuide{};
    active_ = false;
    if (!warned_) {
      std::fprintf(stderr, "[paper-guide] fallback: %s\n", e.what());
      warned_ = true;
    }
  }
  wall_ms_ += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
}

void GuidedPolicy::EnsureDistances() {
  if (!distances_.empty()) return;
  const auto goal_pred = dg_->table().Find(g_->RelationName(goal_->slot));
  if (goal_pred != gar::core::analysis::kInvalidPredicate) {
    distances_ = dg_->DistancesTo(goal_pred);
  }
}

GuideContext GuidedPolicy::Context(const ScoredMotif& candidate) {
  EnsureDistances();
  const GARule& rule = (*rules_)[candidate.motif.rule];
  std::uint32_t distance = gar::core::analysis::kUnreachable;
  if (rule.consequent && !distances_.empty()) {
    const auto pred = dg_->table().Find(
        gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
    if (pred != gar::core::analysis::kInvalidPredicate) distance = distances_[pred];
  }
  return GuideContext{candidate, rule, *g_, distance};
}

double GuidedPolicy::Priority(const ScoredMotif& candidate) {
  const std::size_t index = candidate.motif.rule;
  const std::string id = rule_ids_ != nullptr && index < rule_ids_->size()
      ? (*rule_ids_)[index]
      : "r" + std::to_string(index);
  const auto context = Context(candidate);
  return guide_->guide().Priority(id, context);
}

bool GuidedPolicy::UseGuideThisBatch() {
  if (!UseGuide()) return false;
  if (lambda_ >= 1.0) return true;
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < lambda_;
}

double GuidedPolicy::SelectionPriority(std::uint64_t action_id, double total) {
  return base_->SelectionPriority(action_id, total);
}

double GuidedPolicy::SelectionPriorityForCandidate(
    std::uint64_t action_id, const ScoredMotif& candidate) {
  return base_->SelectionPriorityForCandidate(action_id, candidate);
}

void GuidedPolicy::ReleaseSelectionPriority(std::uint64_t action_id) {
  base_->ReleaseSelectionPriority(action_id);
}

std::optional<std::size_t> GuidedPolicy::Select(
    const std::vector<ScoredMotif>& cands) {
  if (!UseGuideThisBatch()) return base_->Select(cands);
  std::optional<std::size_t> best;
  double best_priority = 0.0;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    const double priority = Priority(cands[i]);
    if (!best || priority > best_priority ||
        (priority == best_priority && cands[i].total > cands[*best].total)) {
      best = i;
      best_priority = priority;
    }
  }
  return best;
}

std::optional<std::size_t> GuidedPolicy::SelectScores(
    const std::vector<double>& totals, const std::vector<char>& admissible) {
  if (UseGuide()) {
    throw std::logic_error("guided selection requires full action scores");
  }
  return base_->SelectScores(totals, admissible);
}

std::vector<std::size_t> GuidedPolicy::SelectBatch(
    const std::vector<BatchCandidate>& cands, std::size_t max_actions) {
  if (!UseGuideThisBatch()) return base_->SelectBatch(cands, max_actions);
  struct Ranked {
    double guide;
    double score;
    std::size_t index;
  };
  std::vector<Ranked> ranked;
  ranked.reserve(cands.size());
  for (std::size_t i = 0; i < cands.size(); ++i) {
    ranked.push_back(Ranked{Priority(*cands[i].score), cands[i].score->total, i});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a,
                                                    const auto& b) {
    if (a.guide != b.guide) return a.guide > b.guide;
    return a.score > b.score;
  });
  std::vector<std::size_t> selected;
  selected.reserve(std::min(max_actions, ranked.size()));
  for (const auto& item : ranked) selected.push_back(item.index);
  selected.resize(std::min(max_actions, selected.size()));
  return selected;
}

}  // namespace gar::core::engine::agent
