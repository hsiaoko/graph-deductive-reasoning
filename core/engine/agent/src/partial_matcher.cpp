#include "partial_matcher.h"

#include <queue>

#include "gar/engine/agent/executor.h"  // AgentContext
#include "rule_support.h"

namespace gar::core::engine {
namespace agent {
namespace deduce {

namespace {

// Vertices covered by at least one edge of `edges` stay connected after
// dropping edge `drop`? BFS over the remaining edges.
bool ConnectedWithout(const gar::core::graph::PatternSpec& spec, std::size_t drop,
                      std::vector<char>* covered) {
  covered->assign(spec.num_vertices, 0);
  std::vector<std::vector<std::uint32_t>> adj(spec.num_vertices);
  std::uint32_t start = UINT32_MAX;
  for (std::size_t i = 0; i < spec.edges.size(); ++i) {
    if (i == drop) continue;
    const auto& e = spec.edges[i];
    adj[e[0]].push_back(e[1]);
    adj[e[1]].push_back(e[0]);
    (*covered)[e[0]] = 1;
    (*covered)[e[1]] = 1;
    start = e[0];
  }
  if (start == UINT32_MAX) return false;  // no edges left at all
  std::queue<std::uint32_t> q;
  q.push(start);
  std::vector<char> seen(spec.num_vertices, 0);
  seen[start] = 1;
  std::uint32_t n_seen = 0;
  while (!q.empty()) {
    const std::uint32_t v = q.front();
    q.pop();
    ++n_seen;
    for (std::uint32_t w : adj[v]) {
      if (!seen[w]) {
        seen[w] = 1;
        q.push(w);
      }
    }
  }
  std::uint32_t n_covered = 0;
  for (char c : *covered) n_covered += c;
  return n_seen == n_covered;
}

// Whether slot's relation (incl. inverse slots, named with the rule's
// dictionary exactly as the DG builder interns them) is derivable — some
// rule's head produces it (DG in-degree > 0).
bool SlotDerivable(std::uint32_t slot, const GARule& rule,
                   const AgentContext& ctx) {
  const std::uint32_t id =
      ctx.table->Find(gar::core::analysis::LabelNameOf(slot, rule));
  return id != gar::core::analysis::kInvalidPredicate && ctx.dg->InDegree(id) > 0;
}

}  // namespace

bool BuildRulePlan(const GARule& rule, std::uint32_t rule_idx,
                   const AgentContext& ctx, const gar::core::graph::TripleGraph& g,
                   RulePlan* out) {
  RuleHead head;
  if (!RuleSupported(rule, &head)) return false;
  if (!RuleReachesGoal(rule, ctx.distances, *ctx.table)) return false;
  out->rule = rule_idx;
  out->head = head;
  out->spec = BuildSpec(rule);
  const auto n = out->spec.num_vertices;
  for (std::size_t i = 0; i < out->spec.edges.size(); ++i) {
    const auto& e = out->spec.edges[i];
    if (!SlotDerivable(e[2], rule, ctx)) continue;  // static: never later
    DeferrableEdge de;
    de.spec_edge = i;
    de.slot = e[2];
    de.pu = e[0];
    de.pv = e[1];
    std::vector<char> covered;
    if (!ConnectedWithout(out->spec, i, &covered)) continue;
    de.both_bound = covered[de.pu] && covered[de.pv];
    // Remap: sub-spec vertices = covered vertices in ascending id order.
    de.to_orig.clear();
    std::vector<std::uint32_t> remap(n, UINT32_MAX);
    for (std::uint32_t v = 0; v < n; ++v) {
      if (covered[v]) {
        remap[v] = static_cast<std::uint32_t>(de.to_orig.size());
        de.to_orig.push_back(v);
      }
    }
    de.sub_spec.num_vertices =
        static_cast<std::uint32_t>(de.to_orig.size());
    for (std::size_t j = 0; j < out->spec.edges.size(); ++j) {
      if (j == i) continue;
      const auto& f = out->spec.edges[j];
      de.sub_spec.edges.push_back({remap[f[0]], remap[f[1]], f[2]});
    }
    out->defers.push_back(std::move(de));
  }
  return true;
}

std::vector<std::uint32_t> ExpandPartial(const DeferrableEdge& de,
                                         const std::uint32_t* sub_emb,
                                         std::uint32_t n_orig) {
  std::vector<std::uint32_t> out(n_orig, UINT32_MAX);
  for (std::uint32_t sv = 0; sv < de.to_orig.size(); ++sv) {
    out[de.to_orig[sv]] = sub_emb[sv];
  }
  return out;
}

}  // namespace deduce
}  // namespace agent
}  // namespace gar::core::engine
