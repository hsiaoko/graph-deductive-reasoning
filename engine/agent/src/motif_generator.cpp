#include "gar/engine/agent/motif_generator.h"

#include "gar/analysis/predicate_table.h"
#include "gar/graph/pattern_matcher.h"

namespace gar {
namespace engine {
namespace agent {

void NaiveMotifGenerator::Candidates(
    const AgentState& state, const std::vector<std::uint32_t>& distances,
    const analysis::PredicateTable& table, std::uint32_t match_cap_per_rule,
    const FailureRegistry& failures, std::vector<Motif>* out) const {
  out->clear();
  const auto& rules = state.rules();
  const graph::TripleGraph& g = state.graph();
  const graph::PatternMatcher matcher(g);
  const bool typed = g.has_vertex_labels();

  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    // Skip exactly the rule shapes the chase marks unsupported
    // (cpu_chase_engine.cpp): no/short pattern, unlabeled pattern edges,
    // ml predicates (no oracle in this engine), unresolved edge-predicate
    // labels, non-edge or unresolved consequent.
    const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
    if (n < 2 || r.pattern.num_edges() < 1 || !r.pattern.has_edge_labels()) {
      continue;
    }
    const RuleHead head = ResolveRuleHead(r);
    if (!head.valid) continue;
    bool skip = false;
    for (const auto& pred : r.antecedent) {
      if (pred.kind == gar::PredKind::kMl ||
          (pred.kind == gar::PredKind::kEdge &&
           pred.label == gar::kWildcardLabel)) {
        skip = true;
        break;
      }
    }
    if (skip) continue;

    // Hard symbolic filter (§5.3): the consequent predicate must reach the
    // goal in the dependency graph.
    const std::uint32_t p0 = table.Find(
        analysis::PredicateIdOf(*r.consequent, r));
    if (p0 == analysis::kInvalidPredicate ||
        distances[p0] == analysis::kUnreachable) {
      continue;
    }

    graph::PatternSpec spec;
    spec.num_vertices = n;
    for (std::uint32_t s = 0; s < n; ++s) {
      for (gar::EdgeIndex e = r.pattern.offsets[s];
           e < r.pattern.offsets[s + 1]; ++e) {
        spec.edges.push_back({s, r.pattern.indices[e],
                              r.pattern.edge_labels[e]});
      }
    }
    std::vector<std::uint32_t> embs;
    bool capped = false;
    const std::uint64_t n_emb =
        matcher.Enumerate(spec, match_cap_per_rule, &embs, &capped);

    for (std::uint64_t t = 0; t < n_emb; ++t) {
      const std::uint32_t* emb = embs.data() + t * n;
      // Vertex-label post-filter, the chase's convention.
      bool label_ok = true;
      for (std::uint32_t u = 0; u < n; ++u) {
        if (typed && u < r.node_labels.size() &&
            r.node_labels[u] != gar::kWildcardLabel &&
            g.VertexLabel(emb[u]) != r.node_labels[u]) {
          label_ok = false;
          break;
        }
      }
      if (!label_ok) continue;
      // Grounded conclusion already in Gamma: derives nothing new.
      if (state.ContainsEdge(head.slot, emb[head.pu], emb[head.pv])) {
        continue;
      }
      // Retired by a recorded failure.
      if (failures.IsRetired(MotifHash(i, emb, n), state.version())) {
        continue;
      }
      out->push_back(Motif{i, std::vector<std::uint32_t>(emb, emb + n)});
    }
  }
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
