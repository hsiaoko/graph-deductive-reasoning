// Fixpoint executor internals: rule plans with deferrable (dynamic) pattern
// edges. src-local; not a public API.
//
// A pattern edge is DEFERRABLE iff its slot's base relation is derivable
// (DG in-degree > 0 — the fact may appear in Γ later) AND removing it keeps
// the remaining pattern connected over the vertices it still touches. The
// PatternMatcher needs every enumerated vertex covered by an edge, so the
// sub-spec remaps to only the covered vertices; `to_orig` maps back.
// Partial embeddings live in the ORIGINAL vertex space with unbound
// vertices marked UINT32_MAX — they never leave the fixpoint executor's
// internals (never handed to Motif/Scorer/Policy/EvalAntecedent).
#pragma once

#include <cstdint>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/executor.h"  // AgentContext
#include "gar/engine/agent/motif.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/rule.h"

namespace gar {
namespace engine {
namespace agent {
namespace fixpoint {

struct DeferrableEdge {
  std::size_t spec_edge = 0;  // index into RulePlan::spec.edges
  std::uint32_t slot = 0;     // pattern slot (may be an inverse slot)
  std::uint32_t pu = 0, pv = 0;  // ORIGINAL vertex ids of the endpoints
  bool both_bound = false;    // both endpoints covered by sub_spec edges
  graph::PatternSpec sub_spec;             // spec minus this edge (remapped)
  std::vector<std::uint32_t> to_orig;    // sub vertex id -> original id
};

struct RulePlan {
  std::uint32_t rule = 0;
  RuleHead head;
  graph::PatternSpec spec;             // full pattern (original vertices)
  std::vector<DeferrableEdge> defers;  // deferrable pattern edges
};

// Builds the plan; returns false for rules the chase convention skips
// (RuleSupported) or that cannot reach the goal in the DG.
bool BuildRulePlan(const GARule& rule, std::uint32_t rule_idx,
                   const AgentContext& ctx, const graph::TripleGraph& g,
                   RulePlan* out);

// Expand a sub-spec embedding (sub vertex space) into the original vertex
// space; vertices unbound by the sub-spec become UINT32_MAX.
std::vector<std::uint32_t> ExpandPartial(const DeferrableEdge& de,
                                         const std::uint32_t* sub_emb,
                                         std::uint32_t n_orig);

}  // namespace fixpoint
}  // namespace agent
}  // namespace engine
}  // namespace gar
