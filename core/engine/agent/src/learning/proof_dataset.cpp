#include "gar/core/agent/learning/proof_dataset.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gar::core::engine::agent::learning {
namespace {

struct PairKey {
  Goal goal;
  FactKey fact;

  bool operator==(const PairKey& other) const {
    return goal.slot == other.goal.slot && goal.args == other.goal.args &&
           fact == other.fact;
  }
};

struct PairKeyHash {
  std::size_t operator()(const PairKey& key) const {
    std::uint64_t hash = key.goal.slot;
    for (const auto& arg : key.goal.args) {
      hash = (hash ^ arg.value_or(std::numeric_limits<std::uint32_t>::max())) *
             0x9E3779B97F4A7C15ull;
    }
    hash ^= key.fact.hi + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    hash ^= key.fact.lo + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return static_cast<std::size_t>(hash ^ (hash >> 31));
  }
};

struct RankedCandidate {
  RelevanceExample example;
  std::uint32_t distance = 0;
  std::size_t trace_index = 0;
  std::uint64_t action_id = 0;
};

std::string GoalIdentity(const Goal& goal) {
  return std::to_string(goal.slot) + ":" +
         (goal.args[0] ? std::to_string(*goal.args[0]) : "_") + ":" +
         (goal.args[1] ? std::to_string(*goal.args[1]) : "_");
}

bool GoalMatches(const Goal& goal, const Fact& fact) {
  if (fact.kind != FactKind::kEdge || fact.arity != 2 ||
      fact.pred != goal.slot) {
    return false;
  }
  return (!goal.args[0] || *goal.args[0] == fact.args[0]) &&
         (!goal.args[1] || *goal.args[1] == fact.args[1]);
}

bool Earlier(const RankedCandidate& left, const RankedCandidate& right) {
  if (left.trace_index != right.trace_index) {
    return left.trace_index < right.trace_index;
  }
  if (left.example.decision_id != right.example.decision_id) {
    return left.example.decision_id < right.example.decision_id;
  }
  return left.action_id < right.action_id;
}

bool SameConfiguration(const PolicyTrace& left, const PolicyTrace& right) {
  return left.graph_fingerprint == right.graph_fingerprint &&
         left.hdc.dim == right.hdc.dim && left.hdc.seed == right.hdc.seed &&
         left.scorer.alpha == right.scorer.alpha &&
         left.scorer.beta == right.scorer.beta &&
         left.scorer.gamma == right.scorer.gamma &&
         left.scorer.cost_weight == right.scorer.cost_weight &&
         left.scorer.nu2 == right.scorer.nu2 &&
         left.scorer.use_confidence == right.scorer.use_confidence;
}

std::uint64_t StableSeed(const Goal& goal) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const unsigned char c : GoalIdentity(goal)) {
    hash = (hash ^ c) * 0x100000001B3ull;
  }
  return hash;
}

// Shared emission tail for every label scheme: drop negatives that collide with
// a positive (goal, fact), emit positives in a stable order, then sample up to
// negatives_per_positive negatives per goal with a deterministic hash order.
void FinalizeDataset(
    const std::unordered_map<PairKey, RankedCandidate, PairKeyHash>& positives,
    std::unordered_map<PairKey, RankedCandidate, PairKeyHash>* negatives,
    const DatasetConfig& config, RelevanceDataset* dataset) {
  for (auto it = negatives->begin(); it != negatives->end();) {
    if (positives.count(it->first) != 0) {
      ++dataset->stats.conflicting_negatives;
      it = negatives->erase(it);
    } else {
      ++it;
    }
  }

  std::vector<const RankedCandidate*> ordered_positives;
  ordered_positives.reserve(positives.size());
  for (const auto& [key, candidate] : positives) {
    (void)key;
    ordered_positives.push_back(&candidate);
  }
  const auto less = [](const RankedCandidate* left,
                       const RankedCandidate* right) {
    const std::string left_goal = GoalIdentity(left->example.goal);
    const std::string right_goal = GoalIdentity(right->example.goal);
    if (left_goal != right_goal) return left_goal < right_goal;
    const FactKey left_fact = KeyOf(left->example.action_fact);
    const FactKey right_fact = KeyOf(right->example.action_fact);
    return left_fact.hi != right_fact.hi ? left_fact.hi < right_fact.hi
                                         : left_fact.lo < right_fact.lo;
  };
  std::sort(ordered_positives.begin(), ordered_positives.end(), less);
  for (const RankedCandidate* candidate : ordered_positives) {
    dataset->examples.push_back(candidate->example);
  }
  dataset->stats.positive_candidates = ordered_positives.size();

  std::unordered_map<std::string, std::uint64_t> positive_counts;
  for (const RankedCandidate* candidate : ordered_positives) {
    ++positive_counts[GoalIdentity(candidate->example.goal)];
  }
  std::unordered_map<std::string, Goal> goals;
  std::unordered_map<std::string, std::vector<const RankedCandidate*>> by_goal;
  for (const auto& [key, candidate] : *negatives) {
    const std::string identity = GoalIdentity(key.goal);
    goals.emplace(identity, key.goal);
    by_goal[identity].push_back(&candidate);
  }
  std::vector<std::string> goal_ids;
  goal_ids.reserve(by_goal.size());
  for (const auto& [identity, candidates] : by_goal) {
    (void)candidates;
    goal_ids.push_back(identity);
  }
  std::sort(goal_ids.begin(), goal_ids.end());
  for (const std::string& identity : goal_ids) {
    auto& candidates = by_goal[identity];
    const Goal& goal = goals.at(identity);
    const auto sample_order = [&](const RankedCandidate* candidate) {
      const FactKey fact = KeyOf(candidate->example.action_fact);
      std::uint64_t state = config.seed ^ StableSeed(goal) ^ fact.hi ^
                            (fact.lo + 0x9E3779B97F4A7C15ull);
      return gar::core::hdc::SplitMix64Next(state);
    };
    std::sort(candidates.begin(), candidates.end(),
              [&](const RankedCandidate* left, const RankedCandidate* right) {
                const std::uint64_t left_order = sample_order(left);
                const std::uint64_t right_order = sample_order(right);
                if (left_order != right_order) return left_order < right_order;
                return less(left, right);
              });
    const std::size_t positive_count = positive_counts[identity];
    const std::size_t sample_count = std::min(
        candidates.size(), positive_count * config.negatives_per_positive);
    for (std::size_t i = 0; i < sample_count; ++i) {
      dataset->examples.push_back(candidates[i]->example);
    }
    dataset->stats.negative_candidates += sample_count;
  }
}

}  // namespace

// Shared proof-DAG labeling. binary_targets=false regresses the graded proximity
// gamma^distance; binary_targets=true classifies on-proof (1) vs off-proof (0).
// The binary form treats off-proof actions as waste to suppress, rather than
// scoring how near a fact sits to the goal in the final proof.
static RelevanceDataset BuildProofLabeledDataset(
    const std::vector<PolicyTrace>& traces, const DatasetConfig& config,
    bool binary_targets) {
  if (!(config.label_gamma > 0.0 && config.label_gamma <= 1.0) ||
      !std::isfinite(config.label_gamma)) {
    throw std::invalid_argument("label_gamma must be finite and in (0,1]");
  }
  RelevanceDataset dataset;
  std::unordered_map<PairKey, RankedCandidate, PairKeyHash> positives;
  std::unordered_map<PairKey, RankedCandidate, PairKeyHash> negatives;

  for (std::size_t trace_index = 0; trace_index < traces.size();
       ++trace_index) {
    const PolicyTrace& trace = traces[trace_index];
    if (!trace.goal_derived) {
      ++dataset.stats.discarded_traces;
      continue;
    }
    if (!traces.empty() && !SameConfiguration(trace, traces.front())) {
      throw std::invalid_argument(
          "all policy traces must share graph, HDC, and scorer configs");
    }
    ++dataset.stats.successful_traces;

    std::unordered_map<std::uint64_t, const CandidateSnapshot*> by_action;
    for (const CandidateSnapshot& candidate : trace.candidates) {
      const auto [it, inserted] =
          by_action.emplace(candidate.action_id, &candidate);
      if (!inserted && ((candidate.selected && !it->second->selected) ||
                        (candidate.selected == it->second->selected &&
                         candidate.decision_id < it->second->decision_id))) {
        it->second = &candidate;
      }
    }
    std::unordered_map<FactKey, std::size_t, FactKeyHash> derivation_by_fact;
    for (std::size_t i = 0; i < trace.derivations.size(); ++i) {
      if (!derivation_by_fact.emplace(KeyOf(trace.derivations[i].consequent), i)
               .second) {
        throw std::invalid_argument(
            "a policy trace derives the same fact more than once");
      }
    }

    std::vector<int> distances(trace.derivations.size(), -1);
    std::queue<std::size_t> pending;
    for (std::size_t i = 0; i < trace.derivations.size(); ++i) {
      if (GoalMatches(trace.goal, trace.derivations[i].consequent)) {
        distances[i] = 0;
        pending.push(i);
        break;
      }
    }
    if (pending.empty()) {
      throw std::invalid_argument(
          "a successful policy trace has no goal derivation");
    }
    while (!pending.empty()) {
      const std::size_t index = pending.front();
      pending.pop();
      for (const Fact& premise : trace.derivations[index].premises) {
        const auto it = derivation_by_fact.find(KeyOf(premise));
        if (it == derivation_by_fact.end() || distances[it->second] >= 0)
          continue;
        distances[it->second] = distances[index] + 1;
        pending.push(it->second);
      }
    }

    std::unordered_map<FactKey, bool, FactKeyHash> proof_facts;
    for (std::size_t i = 0; i < trace.derivations.size(); ++i) {
      if (distances[i] < 0) continue;
      const DerivationRecord& derivation = trace.derivations[i];
      proof_facts.emplace(KeyOf(derivation.consequent), true);
      const auto snapshot = by_action.find(derivation.action_id);
      if (snapshot == by_action.end()) {
        throw std::invalid_argument(
            "a proof derivation has no candidate snapshot");
      }
      const PairKey key{trace.goal, KeyOf(derivation.consequent)};
      RankedCandidate candidate{
          RelevanceExample{derivation.consequent, trace.goal,
                           snapshot->second->handcrafted_score,
                           binary_targets
                               ? 1.0
                               : std::pow(config.label_gamma, distances[i]),
                           snapshot->second->decision_id},
          static_cast<std::uint32_t>(distances[i]), trace_index,
          derivation.action_id};
      const auto [it, inserted] = positives.emplace(key, candidate);
      if (!inserted && (candidate.distance < it->second.distance ||
                        (candidate.distance == it->second.distance &&
                         Earlier(candidate, it->second)))) {
        it->second = std::move(candidate);
      }
    }

    for (const CandidateSnapshot& snapshot : trace.candidates) {
      const FactKey fact = KeyOf(snapshot.consequent);
      if (proof_facts.count(fact) != 0) continue;
      RankedCandidate candidate{
          RelevanceExample{snapshot.consequent, trace.goal,
                           snapshot.handcrafted_score, 0.0,
                           snapshot.decision_id},
          0, trace_index, snapshot.action_id};
      const PairKey key{trace.goal, fact};
      const auto [it, inserted] = negatives.emplace(key, candidate);
      if (!inserted && Earlier(candidate, it->second))
        it->second = std::move(candidate);
    }
  }

  FinalizeDataset(positives, &negatives, config, &dataset);
  return dataset;
}

RelevanceDataset BuildRelevanceDataset(const std::vector<PolicyTrace>& traces,
                                       const DatasetConfig& config) {
  return BuildProofLabeledDataset(traces, config, /*binary_targets=*/false);
}

RelevanceDataset BuildBinaryProofDataset(const std::vector<PolicyTrace>& traces,
                                         const DatasetConfig& config) {
  return BuildProofLabeledDataset(traces, config, /*binary_targets=*/true);
}

}  // namespace gar::core::engine::agent::learning
