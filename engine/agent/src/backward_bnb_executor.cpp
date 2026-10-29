// BackwardBnBExecutor (Phase 2): optimal-chain search over the backward
// AND-OR structure — DFBnB (depth-first branch-and-bound) on grounded
// targets.
//
//   OR node  = grounded target (slot, src, dst); its children are the
//              pinned-enumerated motifs of rules deriving that predicate.
//   AND node = a motif application; its children are the premise targets
//              (pattern edges + edge antecedents) missing from the current
//              fact set, each recursively searched.
//
// Cost: one unit per application (same accounting as the other executors).
// Shared Gamma: derived facts persist globally, so a fact derived earlier is
// FREE when reused by another plan — the optimality certificate is therefore
// over DAG proofs (a shared subproof pays once), within the SEEN candidate
// space (pinned enumeration, capped per rule per visit), matching the
// forward bnb wording ("所见候选空间内最优").
//
// Bounds: the per-predicate derivation depth (min applications to derive
// the predicate from base-anchored predicates, computed inline below) is an
// admissible lower bound on the additional cost of any open target.
// Pruning `paid + 1 + sum(premise lbs) >= incumbent` uses the TREE-style
// sum; under DAG sharing un-derived premises cannot share subproofs (sharing
// only zeroes already-derived facts, whose lb contribution is 0), so the
// sum stays admissible here. Open-sided coordination shapes (disjoint-free
// two-open pairs etc.) are OUT of the certified space in this version
// (skipped as candidates); goals needing them report budget/no-chain
// honestly.
//
// Default off: constructed only for --direction backward --executor bnb.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "apply_helpers.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"

namespace gar {
namespace engine {
namespace agent {

namespace {

constexpr std::uint64_t kInf = std::numeric_limits<std::uint64_t>::max() / 4;

struct Target {
  std::uint32_t slot;
  std::uint32_t src, dst;
  bool operator==(const Target& o) const {
    return slot == o.slot && src == o.src && dst == o.dst;
  }
};
struct TargetHash {
  std::size_t operator()(const Target& t) const {
    return gar::graph::PackPair(t.slot, gar::graph::PackPair(t.src, t.dst));
  }
};

}  // namespace

AgentResult BackwardBnBExecutor::Run(const Goal& goal, AgentState* state,
                                     WitnessLog* witness,
                                     FailureRegistry* failures,
                                     const AgentContext& ctx,
                                     const ExecConfig& cfg,
                                     const ExecKit& kit) {
  AgentResult res;
  const auto& rules = state->rules();
  const graph::TripleGraph& g = state->graph();

  // Head index: predicate name -> rules deriving it (same skip conditions
  // as BackwardExecutor).
  std::unordered_map<std::string, std::vector<std::uint32_t>> head_index;
  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    if (r.pattern.num_vertices() < 2 || r.pattern.num_edges() < 1 ||
        !r.pattern.has_edge_labels()) {
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
    head_index[analysis::PredicateIdOf(*r.consequent, r)].push_back(i);
  }

  // Per-predicate derivation depth (admissible lower bound on applications
  // to derive a predicate): Dijkstra over the rule hypergraph, anchored at
  // predicates with base instances.
  const std::uint32_t nrel = static_cast<std::uint32_t>(g.num_relations());
  std::vector<std::uint64_t> lb(nrel, kInf);
  {
    std::unordered_set<std::string> anchored;
    for (std::uint32_t r = 0; r < nrel; ++r) {
      if (!g.RelationPairs(r).empty()) anchored.insert(g.RelationName(r));
    }
    // iterate to fixpoint (predicate count is small)
    std::unordered_map<std::string, std::uint32_t> rid;
    for (std::uint32_t r = 0; r < nrel; ++r) rid[g.RelationName(r)] = r;
    for (std::uint32_t r = 0; r < nrel; ++r) {
      if (anchored.count(g.RelationName(r))) lb[r] = 0;
    }
    for (bool changed = true; changed;) {
      changed = false;
      for (const GARule& r : rules) {
        const RuleHead head = ResolveRuleHead(r);
        if (!head.valid) continue;
        const auto hit = rid.find(g.RelationName(head.slot));
        if (hit == rid.end()) continue;
        std::uint64_t mx = 0;
        bool inf = false;
        for (std::uint32_t e = 0; e < r.pattern.num_edges(); ++e) {
          gar::LabelID l = r.pattern.edge_labels[e];
          const std::uint32_t b =
              l >= nrel ? l - nrel : static_cast<std::uint32_t>(l);
          const auto bit = rid.find(g.RelationName(b));
          if (bit == rid.end()) continue;
          if (lb[bit->second] == kInf) {
            inf = true;
            break;
          }
          mx = std::max(mx, lb[bit->second]);
        }
        if (inf) continue;
        if (mx + 1 < lb[hit->second]) {
          lb[hit->second] = mx + 1;
          changed = true;
        }
      }
    }
  }
  auto lb_of = [&](gar::LabelID slot) -> std::uint64_t {
    const std::uint32_t b =
        slot >= nrel ? slot - nrel : static_cast<std::uint32_t>(slot);
    return lb[b];
  };

  const graph::PatternMatcher matcher(g);
  auto build_spec = [](const GARule& r) {
    graph::PatternSpec spec;
    spec.num_vertices = static_cast<std::uint32_t>(r.pattern.num_vertices());
    for (std::uint32_t s = 0; s < spec.num_vertices; ++s) {
      for (gar::EdgeIndex e = r.pattern.offsets[s];
           e < r.pattern.offsets[s + 1]; ++e) {
        spec.edges.push_back(
            {s, r.pattern.indices[e], r.pattern.edge_labels[e]});
      }
    }
    return spec;
  };
  auto label_ok = [&](const GARule& r, const std::uint32_t* emb) {
    if (!g.has_vertex_labels()) return true;
    for (std::uint32_t u = 0; u < r.pattern.num_vertices(); ++u) {
      if (u < r.node_labels.size() &&
          r.node_labels[u] != gar::kWildcardLabel &&
          g.VertexLabel(emb[u]) != r.node_labels[u]) {
        return false;
      }
    }
    return true;
  };

  std::uint64_t expansions = 0;
  bool space_truncated = false;  // any candidate dropped outside the
                                 // certified space (open-ended coordination
                                 // shapes, per-rule enumeration cap)
  std::unordered_set<std::size_t> active;  // cycle guard (target hashes)
  // target hash -> fact-set version at which it proved unsolvable; re-admit
  // on Gamma growth (same dynamic-failure discipline as BackwardExecutor).
  std::unordered_map<std::size_t, std::uint64_t> failed_at;
  // best plan per target hash (for the final chain reconstruction)
  std::unordered_map<std::size_t, std::vector<AgentStep>> best_plan;

  auto apply = [&](std::uint32_t ri, const std::uint32_t* emb, std::uint32_t n)
      -> std::optional<graph::DerivedEdge> {
    const GARule& r = rules[ri];
    const std::uint64_t mkey = MotifHash(ri, emb, n);
    if (failures->IsRetired(mkey, state->version())) return std::nullopt;
    if (!graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
      const FailureInfo fi = ClassifyFailure(r, emb, state->attrs(), g, ctx);
      failures->RecordFailure(mkey, fi.dynamic, state->version());
      return std::nullopt;
    }
    const RuleHead head = ResolveRuleHead(r);
    const graph::DerivedEdge d{emb[head.pu], emb[head.pv], head.slot, ri, 0};
    state->ApplyDerivation(d);
    AgentStep step;
    step.motif = Motif{ri, std::vector<std::uint32_t>(emb, emb + n)};
    std::vector<Fact> premises;
    GroundPremises(r, step.motif, g, *ctx.table, &premises);
    for (const auto& f : premises) witness->WitnessFact(f, ctx.encoder);
    witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst), ctx.encoder);
    return d;
  };

  // search(t, cap): min additional cost to make t hold, <= cap; else kInf.
  // Derives facts on success (shared Gamma). Tree/DAG note: present
  // premises cost 0, so repeated subproofs pay once (DAG semantics).
  std::function<std::uint64_t(const Target&, std::uint64_t, std::uint32_t)>
      search = [&](const Target& t, std::uint64_t cap,
                   std::uint32_t depth) -> std::uint64_t {
    if (state->ContainsEdge(t.slot, t.src, t.dst)) return 0;
    if (cap == 0 || depth > cfg.max_subgoal_depth) return kInf;
    const std::size_t tk = TargetHash{}(t);
    if (!active.insert(tk).second) return kInf;  // cycle
    struct Guard {
      std::unordered_set<std::size_t>& a;
      std::size_t k;
      ~Guard() { a.erase(k); }
    } guard{active, tk};
    {
      const auto it = failed_at.find(tk);
      if (it != failed_at.end() && it->second == state->version()) {
        return kInf;
      }
    }
    const std::uint64_t my_lb = lb_of(t.slot);
    if (my_lb == kInf || my_lb > cap) return kInf;

    ++expansions;
    const std::string& pred_name = g.RelationName(t.slot);
    const auto rit = head_index.find(pred_name);
    if (rit == head_index.end()) {
      failed_at[tk] = state->version();
      return kInf;
    }

    // Enumerate pinned candidate motifs of every head rule.
    struct Cand {
      std::uint32_t rule;
      std::vector<std::uint32_t> emb;
      std::uint64_t est;  // 1 + sum of missing-premise lower bounds
      std::vector<std::pair<Target, std::uint64_t>> missing;  // (target, lb)
    };
    std::vector<Cand> cands;
    std::unordered_set<std::uint64_t> seen;
    for (const std::uint32_t ri : rit->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      pins[head.pu] = t.src;
      pins[head.pv] = t.dst;
      const auto m_edges = r.pattern.num_edges();
      // pattern CSR: edges are grouped by src vertex
      std::vector<std::uint32_t> src_of(m_edges);
      for (std::uint32_t sv = 0; sv < n; ++sv) {
        for (gar::EdgeIndex ee = r.pattern.offsets[sv];
             ee < r.pattern.offsets[sv + 1]; ++ee) {
          src_of[ee] = sv;
        }
      }
      // missing premises = pattern edges + edge antecedents absent from the
      // current fact set, GROUNDED (both endpoints determined by the
      // embedding or the pins). Inverse pattern edges map to base (d,s).
      auto add_missing_of = [&](Cand& c, gar::LabelID slot, std::uint32_t s,
                                std::uint32_t d) {
        if (slot >= nrel) {
          slot -= nrel;
          std::swap(s, d);
        }
        if (state->ContainsEdge(slot, s, d)) return;
        const std::uint64_t l = lb_of(slot);
        c.missing.emplace_back(Target{slot, s, d}, l);
        c.est += l;
      };
      auto add_cand = [&](const std::uint32_t* emb) {
        if (!label_ok(r, emb)) return;
        const std::uint64_t key = MotifHash(ri, emb, n);
        if (!seen.insert(key).second) return;
        Cand c;
        c.rule = ri;
        c.emb.assign(emb, emb + n);
        c.est = 1;
        for (std::uint32_t e = 0; e < m_edges; ++e) {
          add_missing_of(c, r.pattern.edge_labels[e], emb[src_of[e]],
                         emb[r.pattern.indices[e]]);
        }
        for (const auto& pred : r.antecedent) {
          if (pred.kind != gar::PredKind::kEdge) continue;
          add_missing_of(c, pred.label, emb[pred.src], emb[pred.dst]);
        }
        std::sort(c.missing.begin(), c.missing.end(),
                  [](const auto& a, const auto& b) {
                    return a.second < b.second;
                  });
        cands.push_back(std::move(c));
      };
      // pass A: full pinned match (all pattern edges present)
      {
        const auto spec = build_spec(r);
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            spec, cfg.match_cap_per_rule, &embs, &capped, pins);
        if (capped) space_truncated = true;
        for (std::uint64_t k = 0; k < n_emb; ++k) {
          add_cand(embs.data() + k * n);
        }
      }
      // pass B: single opened edge — enumerate the sub-pattern without it
      // (only vertices still covered by remaining edges or pinned are
      // determined; an undetermined endpoint of the opened edge makes the
      // candidate open-ended, out of this certificate's scope — skipped).
      for (gar::EdgeIndex oe = 0; oe < m_edges; ++oe) {
        if (m_edges == 1) {
          // single-edge rule: both head endpoints pinned determine the
          // premise target directly
          std::vector<std::uint32_t> emb(n, 0);
          for (const auto& [vv, ww] : pins) emb[vv] = ww;
          add_cand(emb.data());
          break;
        }
        // remaining vertices: covered by other edges or pinned
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        std::vector<std::uint32_t> new2old;
        bool ok_open = true;
        {
          std::vector<char> has(n, 0);
          for (std::uint32_t e = 0; e < m_edges; ++e) {
            if (e == oe) continue;
            has[src_of[e]] = 1;
            has[r.pattern.indices[e]] = 1;
          }
          for (std::uint32_t uu = 0; uu < n; ++uu) {
            if (has[uu]) {
              remap[uu] = static_cast<std::uint32_t>(new2old.size());
              new2old.push_back(uu);
            } else if (!pins.count(uu)) {
              ok_open = false;  // isolated unpinned vertex: undetermined
              break;
            }
          }
        }
        if (!ok_open) {
          space_truncated = true;
          continue;
        }
        // the opened edge's endpoints must be determined
        if (remap.count(src_of[oe]) == 0 && !pins.count(src_of[oe])) {
          space_truncated = true;
          continue;
        }
        if (remap.count(r.pattern.indices[oe]) == 0 &&
            !pins.count(r.pattern.indices[oe])) {
          space_truncated = true;
          continue;
        }
        graph::PatternSpec sub;
        sub.num_vertices = static_cast<std::uint32_t>(new2old.size());
        for (std::uint32_t e = 0; e < m_edges; ++e) {
          if (e == oe) continue;
          sub.edges.push_back({remap[src_of[e]], remap[r.pattern.indices[e]],
                               r.pattern.edge_labels[e]});
        }
        std::unordered_map<std::uint32_t, std::uint32_t> sub_pins;
        for (const auto& [vv, ww] : pins) {
          if (remap.count(vv)) sub_pins[remap[vv]] = ww;
        }
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            sub, cfg.match_cap_per_rule, &embs, &capped, sub_pins);
        if (capped) space_truncated = true;
        const auto sn = static_cast<std::uint32_t>(new2old.size());
        for (std::uint64_t k = 0; k < n_emb; ++k) {
          const std::uint32_t* se = embs.data() + k * sn;
          std::vector<std::uint32_t> emb(n, 0);
          for (std::uint32_t uu = 0; uu < sn; ++uu) emb[new2old[uu]] = se[uu];
          for (const auto& [vv, ww] : pins) emb[vv] = ww;
          add_cand(emb.data());
        }
      }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) {
                if (a.est != b.est) return a.est < b.est;
                return a.rule < b.rule;
              });

    std::uint64_t best = kInf;
    for (const Cand& c : cands) {
      const std::uint64_t w = std::min(cap, best == kInf ? cap : best - 1);
      if (c.est > w) continue;  // cannot beat the incumbent
      // AND: search each missing premise (cheapest-lb first), each with a
      // sub-cap leaving room for the remaining premises' lower bounds.
      std::uint64_t plan = 1;
      bool ok = true;
      std::vector<AgentStep> plan_steps;
      for (std::size_t i = 0; i < c.missing.size(); ++i) {
        std::uint64_t later = 0;
        for (std::size_t j = i + 1; j < c.missing.size(); ++j) {
          later += c.missing[j].second;
        }
        const std::uint64_t room =
            (w > plan + later) ? (w - plan - later) : 0;
        if (c.missing[i].second > room) {
          ok = false;
          break;
        }
        const std::uint64_t pc =
            search(c.missing[i].first, room, depth + 1);
        if (pc == kInf) {
          ok = false;
          break;
        }
        plan += pc;
        const auto pit = best_plan.find(TargetHash{}(c.missing[i].first));
        if (pit != best_plan.end()) {
          plan_steps.insert(plan_steps.end(), pit->second.begin(),
                            pit->second.end());
        }
      }
      if (!ok) continue;
      // premises hold now; apply the motif (its cost is the leading 1)
      const auto d = apply(c.rule, c.emb.data(),
                           static_cast<std::uint32_t>(c.emb.size()));
      if (!d) continue;
      if (plan < best) {
        best = plan;
        AgentStep step;
        step.motif = Motif{c.rule, c.emb};
        step.cost = 1;
        step.success = true;
        step.derived = *d;
        plan_steps.push_back(std::move(step));
        best_plan[tk] = std::move(plan_steps);
      }
    }
    if (best == kInf) failed_at[tk] = state->version();
    return best;
  };

  // Upfront check, same as the other executors.
  if (goal.args[0] && goal.args[1] &&
      state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    res.certified_optimal = true;
    return res;
  }
  if (!goal.args[0] || !goal.args[1]) {
    res.termination = "open_goal_unsupported";
    res.goal_derived = false;
    return res;
  }
  const Target top{goal.slot, *goal.args[0], *goal.args[1]};
  const std::uint64_t best =
      search(top, cfg.budget, 0);
  res.expansions = expansions;
  if (best != kInf && best <= cfg.budget) {
    res.goal_derived = true;
    res.termination = "goal";
    res.total_cost = static_cast<double>(best);
    res.certified_optimal = !space_truncated;  // within the seen space;
    if (space_truncated) res.termination = "goal_space_truncated";
    res.budget_exhausted = best >= cfg.budget;
    const auto it = best_plan.find(TargetHash{}(top));
    if (it != best_plan.end()) res.chain = it->second;
  } else {
    res.goal_derived = false;
    res.termination =
        expansions > 0 ? "no_chain_within_budget" : "no_candidates";
    res.certified_no_chain = !space_truncated;  // seen space exhausted
    if (space_truncated) res.termination = "no_chain_space_truncated";
    res.budget_exhausted = true;
  }
  res.derived = state->journal();
  std::fprintf(stderr,
               "[bwbnb] done: goal=%s cost=%.0f expansions=%llu "
               "derived=%zu space_truncated=%d\n",
               res.goal_derived ? "true" : "false",
               res.goal_derived ? static_cast<double>(best) : -1.0,
               static_cast<unsigned long long>(expansions),
               res.derived.size(), static_cast<int>(space_truncated));
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
