#pragma once

#include "gar/engine/deduction/deduction_engine.h"
#include "gar/graph/triple_graph.h"
#include "gar/graph/vertex_attrs.h"

// GpuDeduceEngine: full logical closure (chase) of a GAR rule set over a KG,
// with the per-rule match + X-evaluation + head emission running on the GPU
// (MatrixGraph's task::GARDeduce). The round driver, fact dedup and graph
// merging stay on the host and are SHARED with the CPU chase
// (gar::graph::MergeRoundFacts), so both engines produce the same derived
// fact set round by round (row order inside a round may differ: GPU
// emission is atomicAdd-ordered).
//
// Semantics mirrored from the CPU chase (cpu_chase_engine.cpp):
//   * homomorphism matching (no injectivity); vertex labels enforced at
//     candidate time == the CPU post-filter for connected patterns;
//   * inverse relation slots folded to base relations with swapped
//     endpoints (pattern edges and edge predicates); heads are emitted
//     unfolded, recorded with the rule's original slot id;
//   * X predicates const/var/exists/edge per embedding; ml predicates and
//     unresolvable attribute references behave exactly like the CPU
//     (rule skipped, resp. x always false with embeddings still counted);
//   * attribute values compared as doubles (int64 columns converted);
//   * same caps: ChaseConfig::max_matches_per_rule (per rule, truncation
//     flagged), max_rounds, max_derivations. ChaseConfig::threads is ignored
//     (the host-side work is tiny).
//
// The header stays free of MatrixGraph types; the implementation (.cpp) is
// the sanctioned MatrixGraph contact point inside this module.
namespace gar {
namespace engine {
namespace deduction {

class GpuDeduceEngine final : public DeductionEngine {
 public:
  // `g` is mutated in place: derived edges are merged into its indexes.
  // `attrs` may be nullptr (treated as zero attribute columns).
  GpuDeduceEngine(graph::TripleGraph& g, const graph::VertexAttrs* attrs)
      : g_(g), attrs_(attrs) {}

  ChaseResult Run(const std::vector<GARule>& rules,
                  const ChaseConfig& cfg) override;

 private:
  graph::TripleGraph& g_;
  const graph::VertexAttrs* attrs_;
};

}  // namespace deduction
}  // namespace engine
}  // namespace gar
