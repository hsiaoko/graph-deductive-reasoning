// src-local: the shared rule-support filter and per-rule matching
// preparation used by the incremental greedy executor and the bnb
// all-candidates helper. Extracted verbatim from the (removed) naive
// per-step generator so both engines enumerate under exactly the same
// chase convention.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/analysis/predicate_table.h"
#include "gar/core/agent/motif.h"
#include "gar/core/graph/pattern_matcher.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule.h"

namespace gar::core::engine {
namespace agent {

// Rejects malformed patterns, unresolved edge-predicate labels, and non-edge
// consequents. ML predicates require a registered model before execution.
inline bool RuleSupported(const GARule& r, RuleHead* head_out) {
  const auto n = static_cast<std::uint32_t>(r.num_vertices());
  if (n < 2 || r.num_edges() < 1 || !r.has_edge_labels()) {
    return false;
  }
  *head_out = ResolveRuleHead(r);
  if (!head_out->valid) return false;
  for (const auto& pred : r.antecedent) {
    if (pred.kind == gar::PredKind::kEdge &&
        pred.label == gar::kWildcardLabel) {
      return false;
    }
  }
  return true;
}

// Hard symbolic filter (§5.3): the consequent predicate must reach the
// goal in the dependency graph.
inline bool RuleReachesGoal(const GARule& r,
                            const std::vector<std::uint32_t>& distances,
                            const gar::core::analysis::PredicateTable& table) {
  const std::uint32_t p0 =
      table.Find(gar::core::analysis::PredicateIdOf(*r.consequent, r));
  return p0 != gar::core::analysis::kInvalidPredicate &&
         distances[p0] != gar::core::analysis::kUnreachable;
}

inline gar::core::graph::PatternSpec BuildSpec(const GARule& r) {
  gar::core::graph::PatternSpec spec;
  const auto n = static_cast<std::uint32_t>(r.num_vertices());
  spec.num_vertices = n;
  for (std::size_t e = 0; e < r.edge_src.size(); ++e) {
    spec.edges.push_back(
        {static_cast<std::uint32_t>(r.edge_src[e]),
         static_cast<std::uint32_t>(r.edge_dst[e]),
         static_cast<gar::LabelID>(r.edge_labels[e])});
  }
  return spec;
}

// Vertex-label post-filter, the chase's convention.
inline bool VertexLabelsOk(const GARule& r, const gar::core::graph::Graph& g,
                           const std::uint32_t* emb) {
  if (!g.has_vertex_labels()) return true;
  const auto n = static_cast<std::uint32_t>(r.num_vertices());
  for (std::uint32_t u = 0; u < n; ++u) {
    if (emb[u] == UINT32_MAX) continue;
    if (u < r.node_labels.size() && r.node_labels[u] != gar::kWildcardLabel &&
        g.VertexLabel(emb[u]) != r.node_labels[u]) {
      return false;
    }
  }
  return true;
}

inline double EstimateStarCost(const GARule& rule,
                               const gar::core::graph::Graph& graph) {
  if (rule.num_edges() == 0) return 0.0;
  double log_estimate = 0.0;
  const double log_vertices =
      std::log(static_cast<double>(std::max(1u, graph.num_entities())));
  for (std::size_t i = 0; i < rule.num_edges(); ++i) {
    const double cardinality = static_cast<double>(std::max<std::size_t>(
        1, graph.RelationPairSet(rule.edge_labels[i]).size()));
    const double contribution = i == 0
        ? std::log(cardinality)
        : std::max(0.0, std::log(cardinality) - log_vertices);
    log_estimate += contribution;
  }
  return std::exp(std::min(log_estimate, 50.0));
}

}  // namespace agent
}  // namespace gar::core::engine
