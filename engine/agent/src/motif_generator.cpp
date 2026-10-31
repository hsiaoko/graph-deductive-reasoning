#include "gar/engine/agent/motif_generator.h"

#include "rule_support.h"

namespace gar {
namespace engine {
namespace agent {

void EnumerateAllCandidates(
    const AgentState& state, const std::vector<std::uint32_t>& distances,
    const analysis::PredicateTable& table, std::uint32_t match_cap_per_rule,
    const FailureRegistry& failures, std::vector<Motif>* out) {
  out->clear();
  const auto& rules = state.rules();
  const graph::TripleGraph& g = state.graph();
  const graph::PatternMatcher matcher(g);

  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    RuleHead head;
    if (!RuleSupported(r, &head)) continue;
    if (!RuleReachesGoal(r, distances, table)) continue;

    const graph::PatternSpec spec = BuildSpec(r);
    std::vector<std::uint32_t> embs;
    bool capped = false;
    const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
    const std::uint64_t n_emb =
        matcher.Enumerate(spec, match_cap_per_rule, &embs, &capped);

    for (std::uint64_t t = 0; t < n_emb; ++t) {
      const std::uint32_t* emb = embs.data() + t * n;
      if (!VertexLabelsOk(r, g, emb)) continue;
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
