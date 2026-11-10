#pragma once

#include "gar/engine/deduction/deduction_engine.h"
#include "gar/graph/triple_graph.h"
#include "gar/graph/vertex_attrs.h"

// CpuChaseEngine: CPU deduction (logical closure / chase) of a GAR rule set
// over a knowledge graph.
//
//   G_{i+1} = G_i ∪ { p₀(emb) | rule Q[x̄](X → p₀), emb ∈ Q(G_i), X(emb) }
//
// iterated to a fixpoint. Matching reuses gar::core::graph::PatternMatcher (homomorphism
// semantics, edge-label constraints, vertex-label post-filter — the same
// counting convention as the miners). X predicates are evaluated per
// embedding: const (x.A⊕c), var (x.A⊕y.B), exists (x.A) against the vertex
// attribute table, edge (l(x,y)) against the current (augmented) graph; ml
// predicates are unsupported and cause the rule to be skipped. Derived facts
// are merged into the TripleGraph (base + inverse slot) after each round, so
// later rounds match on the augmented graph.
//
// This is the naive chase: every round re-enumerates every rule. Rules within
// a round are evaluated in parallel (oneTBB when available; ChaseConfig::
// threads caps the parallelism, 0 = all cores); per-rule derived-fact
// candidates are merged in rule order, keeping dedup credit deterministic.
// Termination is guaranteed (heads introduce no fresh vertices, so at most
// |V|²·|R| edges exist); caps bound the practical cost and flag a
// non-fixpoint stop.
//
// The engine never sees MatrixGraph types: graphs arrive as
// gar::core::graph::TripleGraph and vertex attributes as gar::core::graph::VertexAttrs (plain CPU
// columns); loading from MatrixGraph formats lives in the bridge module.
namespace gar::core::engine {
namespace chase {

class CpuChaseEngine final : public DeductionEngine {
 public:
  // `g` is mutated in place: derived edges are merged into its indexes.
  CpuChaseEngine(gar::core::graph::TripleGraph& g, const gar::core::graph::VertexAttrs* attrs)
      : g_(g), attrs_(attrs) {}

  ChaseResult Run(const std::vector<GARule>& rules,
                  const ChaseConfig& cfg) override;

 private:
  gar::core::graph::TripleGraph& g_;
  const gar::core::graph::VertexAttrs* attrs_;  // may be nullptr (only edge/exists predicates)
};

}  // namespace chase
}  // namespace gar::core::engine
