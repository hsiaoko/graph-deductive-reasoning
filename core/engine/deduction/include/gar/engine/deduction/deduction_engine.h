#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gar/graph/fact.h"
#include "gar/rule.h"

// DeductionEngine: full-closure deduction over a knowledge graph (the chase).
// Two engines share the same semantics and output contract: CpuChaseEngine
// (oneTBB) and GpuDeduceEngine (MatrixGraph task::GARDeduce). Levelized: this
// module depends on gar::core::graph and gar::core only (the GPU engine additionally
// on MatrixGraph, the sanctioned contact point).
namespace gar::core::engine {
namespace chase {

struct ChaseConfig {
  std::uint64_t max_matches_per_rule = 1'000'000;  // embedding cap per rule
  std::uint64_t max_derivations = 10'000'000;      // total new-facts cap
  std::uint32_t max_rounds = 100;
  bool keep_matches = true;  // retain final-round embeddings for output
  // CPU engine: oneTBB max_allowed_parallelism (0 = all cores). The GPU
  // engine ignores it (its host-side work is tiny).
  std::uint32_t threads = 0;
};

struct RuleChaseStats {
  std::uint64_t embeddings = 0;   // topology matches, final round
  std::uint64_t x_satisfied = 0;  // matches passing X, final round
  std::uint64_t derived = 0;      // new facts first derived by this rule
  bool match_capped = false;      // embedding cap hit (counts are lower bounds)
  bool skipped = false;           // unsupported rule shape (see note)
  std::string note;
};

// Final-round matches of one rule (kept when ChaseConfig::keep_matches).
struct RuleMatches {
  std::uint32_t n = 0;                // pattern vertices per embedding
  std::vector<std::uint32_t> embs;    // flat, n per row
  std::vector<std::uint8_t> x_ok;     // X satisfied
  std::vector<std::uint8_t> head_hit; // head edge present in the final graph
  bool capped = false;
};

struct ChaseResult {
  std::vector<RuleChaseStats> rules;
  std::vector<RuleMatches> matches;
  std::vector<gar::core::graph::DerivedEdge> derived;  // all new facts, in derivation order
  // Materialized work over ALL rounds. Per-rule embeddings/x_satisfied above
  // remain final-round values; these totals make batch exploration comparable
  // with goal-directed executors without redefining branches as derivations.
  std::uint64_t embeddings_total = 0;
  std::uint64_t x_satisfied_total = 0;
  std::uint64_t derivation_candidates_total = 0;  // before round dedup
  std::uint32_t rounds = 0;
  bool fixpoint = false;             // false when stopped by a cap
};

class DeductionEngine {
 public:
  virtual ~DeductionEngine() = default;
  virtual ChaseResult Run(const std::vector<GARule>& rules,
                          const ChaseConfig& cfg) = 0;
};

}  // namespace chase
}  // namespace gar::core::engine
