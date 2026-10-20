#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gar/graph/fact.h"
#include "gar/graph/triple_graph.h"
#include "gar/graph/vertex_attrs.h"
#include "gar/rule.h"

// ChaseEngine: CPU deduction (logical closure / chase) of a GAR rule set over
// a knowledge graph.
//
//   G_{i+1} = G_i ∪ { p₀(emb) | rule Q[x̄](X → p₀), emb ∈ Q(G_i), X(emb) }
//
// iterated to a fixpoint. Matching reuses graph::PatternMatcher (homomorphism
// semantics, edge-label constraints, vertex-label post-filter — the same
// counting convention as the miners). X predicates are evaluated per
// embedding: const (x.A⊕c), var (x.A⊕y.B), exists (x.A) against the vertex
// attribute table, edge (l(x,y)) against the current (augmented) graph; ml
// predicates are unsupported and cause the rule to be skipped. Derived facts
// are merged into the TripleGraph (base + inverse slot) after each round, so
// later rounds match on the augmented graph.
//
// This is the naive chase: every round re-enumerates every rule. Rules within
// a round are evaluated in parallel (oneTBB when available); per-rule
// derived-fact candidates are merged in rule order, keeping dedup credit
// deterministic. Termination is guaranteed (heads introduce no fresh vertices,
// so at most |V|²·|R| edges exist); caps bound the practical cost and flag a
// non-fixpoint stop.
//
// The engine never sees MatrixGraph types: graphs arrive as
// graph::TripleGraph and vertex attributes as graph::VertexAttrs (plain CPU
// columns); loading from MatrixGraph formats lives in the bridge module.
namespace gar {
namespace reason {

struct ChaseConfig {
  std::uint64_t max_matches_per_rule = 1'000'000;  // embedding cap per rule
  std::uint64_t max_derivations = 10'000'000;      // total new-facts cap
  std::uint32_t max_rounds = 100;
  bool keep_matches = true;  // retain final-round embeddings for output
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
  std::vector<graph::DerivedEdge> derived;  // all new facts, in derivation order
  std::uint32_t rounds = 0;
  bool fixpoint = false;             // false when stopped by a cap
};

class ChaseEngine {
 public:
  // `g` is mutated in place: derived edges are merged into its indexes.
  ChaseEngine(graph::TripleGraph& g, const graph::VertexAttrs* attrs)
      : g_(g), attrs_(attrs) {}

  ChaseResult Run(const std::vector<GARule>& rules, const ChaseConfig& cfg);

 private:
  graph::TripleGraph& g_;
  const graph::VertexAttrs* attrs_;  // may be nullptr (only edge/exists predicates)
};

}  // namespace reason
}  // namespace gar
