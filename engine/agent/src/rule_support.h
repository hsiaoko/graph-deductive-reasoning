// src-local: the shared rule-support filter and per-rule matching
// preparation used by the incremental greedy executor and the bnb
// all-candidates helper. Extracted verbatim from the (removed) naive
// per-step generator so both engines enumerate under exactly the same
// chase convention.
#pragma once

#include <cstdint>

#include "gar/analysis/dependency_graph.h"
#include "gar/analysis/predicate_table.h"
#include "gar/engine/agent/motif.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

namespace gar {
namespace engine {
namespace agent {

// Skips exactly the rule shapes the chase marks unsupported
// (cpu_chase_engine.cpp): no/short pattern, unlabeled pattern edges, ml
// predicates (no oracle in this engine), unresolved edge-predicate labels,
// non-edge or unresolved consequent.
inline bool RuleSupported(const GARule& r, RuleHead* head_out) {
  const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
  if (n < 2 || r.pattern.num_edges() < 1 || !r.pattern.has_edge_labels()) {
    return false;
  }
  *head_out = ResolveRuleHead(r);
  if (!head_out->valid) return false;
  for (const auto& pred : r.antecedent) {
    if (pred.kind == gar::PredKind::kMl ||
        (pred.kind == gar::PredKind::kEdge &&
         pred.label == gar::kWildcardLabel)) {
      return false;
    }
  }
  return true;
}

// Hard symbolic filter (§5.3): the consequent predicate must reach the
// goal in the dependency graph.
inline bool RuleReachesGoal(const GARule& r,
                            const std::vector<std::uint32_t>& distances,
                            const analysis::PredicateTable& table) {
  const std::uint32_t p0 =
      table.Find(analysis::PredicateIdOf(*r.consequent, r));
  return p0 != analysis::kInvalidPredicate &&
         distances[p0] != analysis::kUnreachable;
}

inline graph::PatternSpec BuildSpec(const GARule& r) {
  graph::PatternSpec spec;
  const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
  spec.num_vertices = n;
  for (std::uint32_t s = 0; s < n; ++s) {
    for (gar::EdgeIndex e = r.pattern.offsets[s];
         e < r.pattern.offsets[s + 1]; ++e) {
      spec.edges.push_back({s, r.pattern.indices[e], r.pattern.edge_labels[e]});
    }
  }
  return spec;
}

// Vertex-label post-filter, the chase's convention.
inline bool VertexLabelsOk(const GARule& r, const graph::TripleGraph& g,
                           const std::uint32_t* emb) {
  if (!g.has_vertex_labels()) return true;
  const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
  for (std::uint32_t u = 0; u < n; ++u) {
    if (u < r.node_labels.size() && r.node_labels[u] != gar::kWildcardLabel &&
        g.VertexLabel(emb[u]) != r.node_labels[u]) {
      return false;
    }
  }
  return true;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
