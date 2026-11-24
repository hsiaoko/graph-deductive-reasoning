#pragma once

#include <cstdint>
#include <vector>

#include "gar/core/agent/learning/policy_trace.h"

namespace gar::core::engine::agent::learning {

struct RelevanceExample {
  Fact action_fact;
  Goal goal;
  double handcrafted_score = 0.0;
  double target = 0.0;
  std::uint64_t decision_id = 0;
};

struct DatasetConfig {
  double label_gamma = 0.5;
  std::size_t negatives_per_positive = 4;
  std::uint64_t seed = 42;
};

struct DatasetStats {
  std::uint64_t successful_traces = 0;
  std::uint64_t discarded_traces = 0;
  std::uint64_t positive_candidates = 0;
  std::uint64_t negative_candidates = 0;
  std::uint64_t conflicting_negatives = 0;
};

struct RelevanceDataset {
  std::vector<RelevanceExample> examples;
  DatasetStats stats;
};

// Labels positives by proof-DAG proximity to the goal: target = gamma^distance.
RelevanceDataset BuildRelevanceDataset(const std::vector<PolicyTrace>& traces,
                                       const DatasetConfig& config);

// Binary proof-usefulness labels: target = 1 for on-proof candidates, 0 for
// off-proof ones. Unlike the graded gamma^distance regression, this frames
// off-proof actions as waste to suppress rather than scoring proof proximity.
RelevanceDataset BuildBinaryProofDataset(const std::vector<PolicyTrace>& traces,
                                         const DatasetConfig& config);

}  // namespace gar::core::engine::agent::learning
