#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gar/graph/pattern_matcher.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

// PatternMiner: mines GAR rules whose body is an arbitrary small connected
// topology (chains, stars, triangles, ...), generalizing ChainMiner:
//
//   body:  a connected pattern of k <= max_body_edges labeled edges
//   head:  rh(xu, xv) for a base relation rh and a pivot pair (u,v)
//
// * support    = |{(xu,xv) : body ∧ head}|      (distinct pivot pairs)
// * confidence = support / |{(xu,xv) : body}|
//
// exactly the ChainMiner convention; in fact on chain patterns of length
// <= 2 the counts are computed with the same nested-loop join (and the same
// caps) as ChainMiner, so both miners produce identical rule sets there.
//
// Mining proceeds level-wise (k = 1..max_body_edges). Patterns are extended
// by growth (a new edge to a fresh vertex) or closing (a new edge between
// two existing, non-adjacent vertices), pruned by a slot co-occurrence
// index, deduplicated by a canonical form over all vertex permutations, and
// kept alive when some ordered vertex pair projects to >= min_support
// distinct pairs (an Apriori-safe criterion). Rules are emitted on pivot
// pairs at maximal pattern distance — for chains these are exactly the two
// endpoint orders ChainMiner uses.
namespace gar::tools::miner {

struct PatternMinerConfig {
  std::uint32_t max_body_edges = 3;    // body edges k = 1..max_body_edges
  std::uint32_t min_body_edges = 1;    // emit no rules below this body size
                                       // (2 = no single-edge r(x,y)->s(x,y)
                                       // relation-implication rules; level 1
                                       // is still evaluated as parent patterns)
  std::uint64_t min_support = 50;      // on support (body ∧ head pairs)
  double min_confidence = 0.05;
  std::size_t pair_cap = 4'000'000;    // per pivot-pair pair-set cap
  std::uint64_t match_cap = 2'000'000;     // per-pattern embedding cap
  std::uint64_t join_attempt_cap = 50'000'000;  // chain join work cap
  std::string dataset_root;            // dataset root path, recorded as provenance
};

struct PatternMinerStats {
  std::uint64_t patterns_evaluated = 0;
  std::uint64_t patterns_capped = 0;
  std::uint64_t rules_emitted = 0;
};

class PatternMiner {
 public:
  PatternMiner(const gar::core::graph::TripleGraph& graph, PatternMinerConfig config);

  // Runs the mining pass and returns the rules (quality fields filled).
  std::vector<GARule> Mine();

  [[nodiscard]] const PatternMinerStats& stats() const { return stats_; }

 private:
  // A connected pattern in canonical numbering (edges sorted).
  struct Pattern {
    std::uint32_t n = 0;
    std::vector<std::array<std::uint32_t, 3>> edges;  // (src, dst, slot)
  };

  const gar::core::graph::TripleGraph& g_;
  PatternMinerConfig cfg_;
  PatternMinerStats stats_;
};

}  // namespace gar::tools::miner
