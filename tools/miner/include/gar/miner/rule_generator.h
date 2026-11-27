#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "gar/core/graph/graph.h"
#include "gar/core/rule.h"

namespace gar::tools::miner {

struct RelationMetadata {
  std::uint32_t slot = 0;
  std::uint64_t support = 0;
  std::unordered_set<std::uint32_t> sources;
  std::unordered_set<std::uint32_t> targets;
};

struct CandidateRule {
  std::uint32_t p = 0;
  std::uint32_t q = 0;
  std::uint32_t r = 0;
  std::uint64_t candidate_pair_count = 0;
  std::uint64_t support = 0;
  double confidence = 0.0;
  std::uint64_t novel_count = 0;
  GARule rule;
};

struct RuleGeneratorConfig {
  std::size_t count = 100;
  std::uint64_t min_support = 1;
  std::string dataset_root;
};

struct RuleGeneratorStats {
  std::uint64_t body_pairs_evaluated = 0;
  std::uint64_t candidates_scored = 0;
  std::uint64_t rules_emitted = 0;
};

// Generates disconnected two-edge rules with exact pivot-pair statistics:
// p(x0,x1) AND q(x2,x3) -> r(x0,x3). Head relations are always base slots.
class RuleGenerator {
 public:
  RuleGenerator(const gar::core::graph::Graph& graph,
                RuleGeneratorConfig config);

  std::vector<CandidateRule> Generate();

  [[nodiscard]] const RuleGeneratorStats& stats() const { return stats_; }

 private:
  RelationMetadata ExtractMetadata(std::uint32_t slot) const;
  std::vector<std::uint64_t> CountHeadSupport(
      const RelationMetadata& p, const RelationMetadata& q,
      const std::vector<char>& eligible_heads) const;
  GARule BuildRule(const CandidateRule& candidate) const;

  const gar::core::graph::Graph& g_;
  RuleGeneratorConfig cfg_;
  std::vector<RelationMetadata> metadata_;
  RuleGeneratorStats stats_;
};

}  // namespace gar::tools::miner
