#pragma once

#include "gar/core/chase/chase_engine.h"
#include "gar/core/graph/graph.h"
#include "gar/core/graph/attributes.h"
#include "gar/core/graph/predicate_evaluator.h"

// CpuChaseEngine: CPU deduction (logical closure / chase) of a GAR rule set
// over a knowledge graph.
//
//   G_{i+1} = G_i ∪ { p₀(emb) | rule Q[x̄](X → p₀), emb ∈ Q(G_i), X(emb) }
//
// iterated to a fixpoint. Matching reuses gar::core::graph::PatternMatcher (homomorphism
// semantics, edge-label constraints, vertex-label post-filter — the same
// counting convention as the miners). X predicates are evaluated per
// embedding: const (x.A⊕c), var (x.A⊕y.B), exists (x.A) against the vertex
// attribute table, edge (l(x,y)) against the current (augmented) graph; ML
// predicates use the injected model registry. Derived facts
// are merged into the Graph (base + inverse slot) after each round, so
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
// Graphs arrive as gar::core::graph::Graph (MatrixGraph GARGraphArrays view +
// dynamic fact indexes) and attributes as MatrixGraph AttributesIO::Storage.
namespace gar::core::engine {
namespace chase {

class CpuChaseEngine final : public ChaseEngine {
 public:
  // `g` is mutated in place: derived edges are merged into its indexes.
  CpuChaseEngine(gar::core::graph::Graph& g,
                 const gar::core::graph::VertexAttributes* attrs,
                 const gar::core::graph::MlModelRegistry* ml_models = nullptr)
      : g_(g), attrs_(attrs), ml_models_(ml_models) {}

  ChaseResult Run(const std::vector<GARule>& rules,
                  const ChaseConfig& cfg) override;

 private:
  gar::core::graph::Graph& g_;
  const gar::core::graph::VertexAttributes* attrs_;  // may be nullptr
  const gar::core::graph::MlModelRegistry* ml_models_;
};

}  // namespace chase
}  // namespace gar::core::engine
