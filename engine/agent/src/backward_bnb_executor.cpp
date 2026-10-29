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
  bool shape_truncated = false;  // candidates dropped outside the
                                 // certified space: undetermined endpoints
                                 // or non-semi-joinable open sets
  bool cap_truncated = false;    // enumeration/pair caps hit
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

  // Min proof per target. Stamp = per-predicate derived counts summed over
  // the body predicates of rules that could derive the target — the memo is
  // invalid only when the target's OWN candidate space could have changed
  // (global version invalidation would invalidate on every derivation
  // anywhere, churning the whole memo map into uselessness).
  auto target_stamp = [&](const Target& t) -> std::uint64_t {
    std::uint64_t s = 0;
    const auto rit = head_index.find(g.RelationName(t.slot));
    if (rit == head_index.end()) return 0;
    std::uint64_t rule_i = 0;
    for (const std::uint32_t ri : rit->second) {
      if (std::getenv("GAR_DEBUG_BWB") && ((rule_i++ & 7) == 0)) {
        std::fprintf(stderr, "[bwbnb]   rule %u/%zu ri=%u\n", rule_i,
                     rit->second.size(), ri);
      }
      const GARule& r = rules[ri];
      for (std::uint32_t e = 0; e < r.pattern.num_edges(); ++e) {
        gar::LabelID l = r.pattern.edge_labels[e];
        const std::uint32_t b =
            l >= nrel ? l - nrel : static_cast<std::uint32_t>(l);
        s += state->PerSlotDerivedCount(b);
      }
      for (const auto& pred : r.antecedent) {
        if (pred.kind != gar::PredKind::kEdge) continue;
        const std::uint32_t b =
            pred.label >= nrel ? pred.label - nrel : pred.label;
        s += state->PerSlotDerivedCount(b);
      }
    }
    return s;
  };

  // ---- instance-level lower bound (instance mode) ----
  // one_level_derives(t): any head rule fires RIGHT NOW under the pins
  // (pinned enumeration + label filter + EvalAntecedent on current Gamma).
  // Gamma is monotone, so "no motif fires now" implies any proof of t needs
  // >= 2 applications (one to create the match, one to derive t). Memoized
  // per target at the current fact-set version; recomputed on growth (a
  // stale "no" would overestimate -> unsafe for pruning).
  std::unordered_map<std::size_t, std::pair<std::uint64_t, bool>> onelevel_cache;
  std::uint64_t probe_calls = 0;
  auto one_level_derives = [&](const Target& t) -> bool {
    const std::size_t tk = TargetHash{}(t);
    const auto it = onelevel_cache.find(tk);
    if (it != onelevel_cache.end() && it->second.first == target_stamp(t)) {
      return it->second.second;
    }
    ++probe_calls;
    bool fires = false;
    const auto rit = head_index.find(g.RelationName(t.slot));
    if (rit != head_index.end()) {
      for (const std::uint32_t ri : rit->second) {
        const GARule& r = rules[ri];
        const RuleHead head = ResolveRuleHead(r);
        std::unordered_map<std::uint32_t, std::uint32_t> pins;
        pins[head.pu] = t.src;
        pins[head.pv] = t.dst;
        const auto spec = build_spec(r);
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            spec, cfg.match_cap_per_rule, &embs, &capped, pins);
        const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
        for (std::uint64_t k = 0; k < n_emb && !fires; ++k) {
          const std::uint32_t* emb = embs.data() + k * n;
          if (!label_ok(r, emb)) continue;
          if (graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
            fires = true;
          }
        }
        if (fires) break;
      }
    }
    onelevel_cache[tk] = {target_stamp(t), fires};
    return fires;
  };
  // Free probes for coordination candidates (same no-charge convention as
  // BackwardExecutor: probes are reads, only applications charge).
  // present_values: free-endpoint values of slot edges already in Gamma
  // under the pin (base + derived; base or inverse slot via OutNeighbors).
  auto present_values = [&](gar::LabelID slot, bool pin_src,
                            std::uint32_t pinned) {
    const std::uint32_t rel =
        pin_src ? slot : slot + static_cast<std::uint32_t>(g.num_relations());
    const auto [ptr, cnt] = g.OutNeighbors(pinned, rel);
    return std::vector<std::uint32_t>(ptr, ptr + cnt);
  };
  // collect_candidate_values: free-endpoint values that rules deriving
  // `slot` could produce RIGHT NOW with the pin (1-step match horizon).
  auto collect_candidate_values = [&](gar::LabelID slot, bool pin_src,
                                      std::uint32_t pinned_val)
      -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> vals;
    std::unordered_set<std::uint32_t> seen;
    const auto it = head_index.find(g.RelationName(slot));
    if (it == head_index.end()) return vals;
    for (const std::uint32_t ri : it->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      pins[pin_src ? head.pu : head.pv] = pinned_val;
      const auto spec = build_spec(r);
      std::vector<std::uint32_t> embs;
      bool capped = false;
      const std::uint64_t n_emb = matcher.Enumerate(
          spec, cfg.match_cap_per_rule, &embs, &capped, pins);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      const std::uint32_t free_v = pin_src ? head.pv : head.pu;
      for (std::uint64_t k = 0; k < n_emb; ++k) {
        const std::uint32_t* emb = embs.data() + k * n;
        if (!label_ok(r, emb)) continue;
        if (!graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
          continue;
        }
        if (seen.insert(emb[free_v]).second) vals.push_back(emb[free_v]);
      }
    }
    return vals;
  };
  auto merge_candidates = [](std::vector<std::uint32_t> present,
                             const std::vector<std::uint32_t>& derivable) {
    std::unordered_set<std::uint32_t> seen(present.begin(), present.end());
    for (const std::uint32_t v : derivable) {
      if (seen.insert(v).second) present.push_back(v);
    }
    return present;
  };

  // Probe-space stamp for a predicate: facts of the predicate itself plus
  // of every body predicate that could contribute a derivation of it.
  auto probe_stamp = [&](std::uint32_t slot) -> std::uint64_t {
    std::uint64_t out = state->PerSlotDerivedCount(slot);
    const auto rit = head_index.find(g.RelationName(slot));
    if (rit == head_index.end()) return out;
    std::uint64_t rule_i = 0;
    for (const std::uint32_t ri : rit->second) {
      if (std::getenv("GAR_DEBUG_BWB") && ((rule_i++ & 7) == 0)) {
        std::fprintf(stderr, "[bwbnb]   rule %u/%zu ri=%u\n", rule_i,
                     rit->second.size(), ri);
      }
      const GARule& r = rules[ri];
      for (std::uint32_t e = 0; e < r.pattern.num_edges(); ++e) {
        gar::LabelID l = r.pattern.edge_labels[e];
        out += state->PerSlotDerivedCount(
            l >= nrel ? l - nrel : static_cast<std::uint32_t>(l));
      }
      for (const auto& pred : r.antecedent) {
        if (pred.kind != gar::PredKind::kEdge) continue;
        out += state->PerSlotDerivedCount(
            pred.label >= nrel ? pred.label - nrel : pred.label);
      }
    }
    return out;
  };
  // vals cache: (slot, pin_src, pin) -> (stamp, present ∪ 1-level-derivable)
  std::unordered_map<std::uint64_t,
                     std::pair<std::uint64_t, std::vector<std::uint32_t>>>
      vals_cache;
  auto side_vals = [&](std::uint32_t slot, bool pin_src, std::uint32_t pin)
      -> const std::vector<std::uint32_t>& {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(slot) << 33) ^
        (static_cast<std::uint64_t>(pin_src ? 1 : 0) << 32) ^ pin;
    const std::uint64_t stamp = probe_stamp(slot);
    const auto it = vals_cache.find(key);
    if (it != vals_cache.end() && it->second.first == stamp) {
      return it->second.second;
    }
    auto merged = merge_candidates(present_values(slot, pin_src, pin),
                                   collect_candidate_values(slot, pin_src,
                                                            pin));
    return vals_cache.emplace(key, std::make_pair(stamp, std::move(merged)))
        .first->second.second;
  };
  // 1-level-derivable dst set of (slot, src): for the semi-join membership
  // test (present checked separately by the caller).
  std::unordered_map<std::uint64_t,
                     std::pair<std::uint64_t, std::vector<std::uint32_t>>>
      reach_cache;
  auto one_level_dsts = [&](std::uint32_t slot, std::uint32_t src)
      -> const std::vector<std::uint32_t>& {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(slot) << 32) ^ src;
    const std::uint64_t stamp = probe_stamp(slot);
    const auto it = reach_cache.find(key);
    if (it != reach_cache.end() && it->second.first == stamp) {
      return it->second.second;
    }
    return reach_cache
        .emplace(key, std::make_pair(
                          stamp, collect_candidate_values(slot, true, src)))
        .first->second.second;
  };

  std::uint64_t lb_entry_prunes = 0, est_prunes = 0, inf_prunes = 0;
  // Instance-level lb for an open (non-present) target:
  //   INF            no rule can ever derive it (no head rules / predicate
  //                  unreachable from anchors) — safe prune;
  //   max(pred,2)    no motif fires now (proof needs >= 2 applications,
  //                  Gamma-monotonicity argument in the header comment);
  //   max(pred,1)    a motif fires now (tight: >= 1);
  // predicate mode: the data-blind derivation depth only.
  auto lb_inst = [&](const Target& t) -> std::uint64_t {
    const std::uint64_t plb = lb_of(t.slot);
    if (plb == kInf) return kInf;
    if (!cfg.backward_bnb_instance_lb) return plb;
    if (head_index.find(g.RelationName(t.slot)) == head_index.end()) {
      return kInf;
    }
    if (one_level_derives(t)) return std::max<std::uint64_t>(plb, 1);
    return std::max<std::uint64_t>(plb, 2);
  };

  // ---- proof-set search (fact-set accounting; sharing exact) ----
  // search(t, cap, depth) returns the minimum-cardinality PROOF of t: the
  // set of facts to derive (each application derives one unique new fact,
  // so set size == application count). Base facts (present in Gamma at Run
  // start) have the empty proof. Premise subproofs are UNIONED — a shared
  // subproof is counted once globally, so the total is the true unique-
  // application count of the DAG proof. Pruning uses only admissible keys
  // (max of premise lower bounds; the union is at least as large as any
  // member proof); sums are ordering heuristics only.
  using FactSet = std::unordered_set<std::uint64_t>;
  using FactSetPtr = std::shared_ptr<const FactSet>;
  const FactSetPtr kEmptyProof = std::make_shared<const FactSet>();
  auto fact_key = [](std::uint32_t slot, std::uint32_t s, std::uint32_t d) {
    return gar::graph::PackPair(slot, gar::graph::PackPair(s, d));
  };
  std::unordered_map<std::size_t, std::pair<std::uint64_t, FactSetPtr>>
      proof_memo;  // (stamp, proof)
  // Failure memo: (stamp, cap) — a target unprovable within cap stays so
  // while its candidate space is unchanged; re-admit on space growth or a
  // LARGER cap.
  std::unordered_map<std::size_t, std::pair<std::uint64_t, std::uint64_t>>
      failed_cap;

  const graph::TripleGraph g0 = state->graph();  // base snapshot (holdout
  // already applied by the caller): proofs are costed against g0
  auto g0_has = [&](std::uint32_t slot, std::uint32_t s, std::uint32_t d) {
    return g0.RelationPairSet(slot).count(gar::graph::PackPair(s, d)) != 0;
  };

  std::function<FactSetPtr(const Target&, std::uint64_t, std::uint32_t)>
      search = [&](const Target& t, std::uint64_t cap,
                   std::uint32_t depth) -> FactSetPtr {
    const std::size_t tk = TargetHash{}(t);
    {
      const auto it = proof_memo.find(tk);
      if (it != proof_memo.end() && it->second.first == target_stamp(t)) {
        return it->second.second->size() <= cap ? it->second.second
                                                : nullptr;
      }
    }
    if (g0_has(t.slot, t.src, t.dst)) return kEmptyProof;
    if (cap == 0 || depth > cfg.max_subgoal_depth) return nullptr;
    {
      const auto it = failed_cap.find(tk);
      if (it != failed_cap.end() &&
          it->second.first == target_stamp(t) && it->second.second >= cap) {
        return nullptr;
      }
    }
    const std::uint64_t my_lb = lb_inst(t);
    if (my_lb == kInf || my_lb > cap) {
      ++lb_entry_prunes;
      return nullptr;
    }
    if (!active.insert(tk).second) return nullptr;  // cycle
    struct Guard {
      std::unordered_set<std::size_t>& a;
      std::size_t k;
      ~Guard() { a.erase(k); }
    } guard{active, tk};

    ++expansions;
    const std::string& pred_name = g.RelationName(t.slot);
    const auto rit = head_index.find(pred_name);
    if (rit == head_index.end()) {
      failed_cap[tk] = {target_stamp(t), cap};
      return nullptr;
    }

    // Enumerate pinned candidate motifs of every head rule.
    struct Cand {
      std::uint32_t rule;
      std::vector<std::uint32_t> emb;
      std::uint64_t est;  // 1 + sum of missing-premise lbs (ORDERING only)
      std::uint64_t maxlb = 0;  // max(1, max premise lb): admissible PRUNE key
      std::vector<std::pair<Target, std::uint64_t>> missing;  // (target, lb)
    };
    std::vector<Cand> cands;
    std::unordered_set<std::uint64_t> seen;
    std::uint64_t rule_i = 0;
    for (const std::uint32_t ri : rit->second) {
      if (std::getenv("GAR_DEBUG_BWB") && ((rule_i++ & 7) == 0)) {
        std::fprintf(stderr, "[bwbnb]   rule %u/%zu ri=%u\n", rule_i,
                     rit->second.size(), ri);
      }
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
      // missing premises = pattern edges + edge antecedents absent from g0
      // (the BASE fact set — a non-base edge needs a proof even when it is
      // already derived; its memoized proof is unioned by the parent).
      // Inverse pattern edges map to base (d,s).
      auto add_missing_of = [&](Cand& c, gar::LabelID slot, std::uint32_t s,
                                std::uint32_t d) {
        if (slot >= nrel) {
          slot -= nrel;
          std::swap(s, d);
        }
        if (g0_has(slot, s, d)) return;
        const std::uint64_t l = lb_inst(Target{slot, s, d});
        c.missing.emplace_back(Target{slot, s, d}, l);
        c.est += l;
        c.maxlb = std::max(c.maxlb, l);
      };
      auto add_cand = [&](const std::uint32_t* emb) {
        if (!label_ok(r, emb)) return;
        const std::uint64_t key = MotifHash(ri, emb, n);
        if (!seen.insert(key).second) return;
        Cand c;
        c.rule = ri;
        c.emb.assign(emb, emb + n);
        c.est = 1;
        c.maxlb = 0;
        for (std::uint32_t e = 0; e < m_edges; ++e) {
          add_missing_of(c, r.pattern.edge_labels[e], emb[src_of[e]],
                         emb[r.pattern.indices[e]]);
        }
        for (const auto& pred : r.antecedent) {
          if (pred.kind != gar::PredKind::kEdge) continue;
          add_missing_of(c, pred.label, emb[pred.src], emb[pred.dst]);
        }
        c.maxlb = std::max<std::uint64_t>(c.maxlb, 1);
        std::sort(c.missing.begin(), c.missing.end(),
                  [](const auto& a, const auto& b) {
                    return a.second < b.second;
                  });
        cands.push_back(std::move(c));
      };
      // pass A: full pinned match (all pattern edges present in Gamma)
      {
        const auto spec = build_spec(r);
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            spec, cfg.match_cap_per_rule, &embs, &capped, pins);
        if (capped) cap_truncated = true;
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
          shape_truncated = true;
          continue;
        }
        // the opened edge's endpoints must be determined
        if (remap.count(src_of[oe]) == 0 && !pins.count(src_of[oe])) {
          shape_truncated = true;
          continue;
        }
        if (remap.count(r.pattern.indices[oe]) == 0 &&
            !pins.count(r.pattern.indices[oe])) {
          shape_truncated = true;
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
        // An unanchored sub-pattern (no pin survives the opening) enumerates
        // the whole graph — excluded from the certified space.
        if (sub_pins.empty()) {
          shape_truncated = true;
          continue;
        }
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            sub, cfg.match_cap_per_rule, &embs, &capped, sub_pins);
        if (capped) cap_truncated = true;
        const auto sn = static_cast<std::uint32_t>(new2old.size());
        for (std::uint64_t k = 0; k < n_emb; ++k) {
          const std::uint32_t* se = embs.data() + k * sn;
          std::vector<std::uint32_t> emb(n, 0);
          for (std::uint32_t uu = 0; uu < sn; ++uu) emb[new2old[uu]] = se[uu];
          for (const auto& [vv, ww] : pins) emb[vv] = ww;
          add_cand(emb.data());
        }
      }
      // pass C: two-open coordination (the disjoint-free shape): each opened
      // edge has exactly one goal-pinned endpoint and the free endpoints are
      // different variables; candidate values are present-or-1-level
      // derivable per side, semi-joined through the remaining edges
      // (present in Gamma or 1-level derivable). Each composing pair yields
      // a fully grounded embedding — an ordinary Cand whose opened/middle
      // edges are missing premises searched recursively (DAG cost).
      for (gar::EdgeIndex e1 = 0; e1 < m_edges; ++e1) {
        for (gar::EdgeIndex e2 = e1 + 1; e2 < m_edges; ++e2) {
          const std::uint32_t s1 = src_of[e1], d1 = r.pattern.indices[e1];
          const std::uint32_t s2 = src_of[e2], d2 = r.pattern.indices[e2];
          const bool p1s = pins.count(s1) != 0, p1d = pins.count(d1) != 0;
          const bool p2s = pins.count(s2) != 0, p2d = pins.count(d2) != 0;
          if (p1s == p1d || p2s == p2d) continue;  // need exactly one each
          const std::uint32_t a_free = p1s ? d1 : s1;
          const std::uint32_t b_free = p2s ? d2 : s2;
          if (a_free == b_free) continue;         // shared var: pass B shape
          // semi-joinable + all vertices determined: every remaining edge's
          // endpoints in {pins, a_free, b_free}, and no other vertex exists
          bool sj = true;
          std::vector<char> determined(n, 0);
          for (const auto& [vv, ww] : pins) determined[vv] = 1;
          determined[a_free] = 1;
          determined[b_free] = 1;
          for (std::uint32_t uu = 0; uu < n; ++uu) {
            if (!determined[uu]) {
              sj = false;
              break;
            }
          }
          for (std::uint32_t e = 0; e < m_edges && sj; ++e) {
            if (e == e1 || e == e2) continue;
            if (!determined[src_of[e]] || !determined[r.pattern.indices[e]]) {
              sj = false;
            }
          }
          if (!sj) {
            shape_truncated = true;
            continue;
          }
          // base-orientation pins for the two opened edges
          const gar::LabelID l1 = r.pattern.edge_labels[e1];
          const gar::LabelID l2 = r.pattern.edge_labels[e2];
          const std::uint32_t slot1 =
              l1 >= nrel ? l1 - nrel : static_cast<std::uint32_t>(l1);
          const std::uint32_t slot2 =
              l2 >= nrel ? l2 - nrel : static_cast<std::uint32_t>(l2);
          const bool inv1 = l1 >= nrel;
          const bool inv2 = l2 >= nrel;
          const std::uint32_t pin1 = pins.at(p1s ? s1 : d1);
          const std::uint32_t pin2 = pins.at(p2s ? s2 : d2);
          // pattern-src pinned sits on the base src unless inverse
          const bool pin1_src = p1s != inv1;
          const bool pin2_src = p2s != inv2;
          const std::vector<std::uint32_t>& vals1 =
              side_vals(slot1, pin1_src, pin1);
          const std::vector<std::uint32_t>& vals2 =
              side_vals(slot2, pin2_src, pin2);
          if (vals1.empty() || vals2.empty()) continue;
          std::size_t pairs_added = 0;
          bool overflow = false;
          for (const std::uint32_t v1 : vals1) {
            if (overflow) break;
            for (const std::uint32_t v2 : vals2) {
              if (pairs_added >= 64) {
                overflow = true;
                cap_truncated = true;
                break;
              }
              // semi-join: every remaining edge present or 1-level derivable
              std::vector<std::uint32_t> emb(n, 0);
              for (const auto& [vv, ww] : pins) emb[vv] = ww;
              emb[a_free] = v1;
              emb[b_free] = v2;
              bool edges_ok = true;
              for (std::uint32_t e = 0; e < m_edges && edges_ok; ++e) {
                if (e == e1 || e == e2) continue;
                gar::LabelID l = r.pattern.edge_labels[e];
                std::uint32_t es = emb[src_of[e]], ed = emb[r.pattern.indices[e]];
                if (l >= nrel) {
                  l -= nrel;
                  std::swap(es, ed);
                }
                if (!g0_has(l, es, ed) && !state->ContainsEdge(l, es, ed)) {
                  const auto& reach = one_level_dsts(l, es);
                  if (std::find(reach.begin(), reach.end(), ed) ==
                      reach.end()) {
                    edges_ok = false;
                  }
                }
              }
              if (!edges_ok) continue;
              ++pairs_added;
              add_cand(emb.data());
            }
          }
        }
      }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) {
                if (a.est != b.est) return a.est < b.est;
                return a.rule < b.rule;
              });

    FactSetPtr best = nullptr;
    for (const Cand& c : cands) {
      const std::uint64_t w =
          std::min(cap, best ? best->size() - 1 : cap);
      if (c.maxlb > w) {  // admissible prune (union >= each member proof)
        ++est_prunes;
        if (c.maxlb == kInf) ++inf_prunes;
        continue;
      }
      // AND: search each missing premise (cheapest-lb first), each with a
      // sub-cap leaving room for the remaining premises' lower bounds.
      FactSet plan;
      bool ok = true;
      std::vector<AgentStep> plan_steps;
      for (std::size_t i = 0; i < c.missing.size(); ++i) {
        std::uint64_t later = 0;  // suffix-MAX of still-unproven lbs
        for (std::size_t j = i + 1; j < c.missing.size(); ++j) {
          if (g0_has(c.missing[j].first.slot, c.missing[j].first.src,
                     c.missing[j].first.dst)) {
            continue;
          }
          later = std::max(later, c.missing[j].second);
        }
        const std::uint64_t room =
            (w > plan.size() + later) ? (w - plan.size() - later) : 0;
        if (c.missing[i].second > room) {
          ok = false;
          break;
        }
        const FactSetPtr sub =
            search(c.missing[i].first, room, depth + 1);
        if (!sub) {
          ok = false;
          break;
        }
        plan.insert(sub->begin(), sub->end());
        const auto pit = best_plan.find(TargetHash{}(c.missing[i].first));
        if (pit != best_plan.end()) {
          plan_steps.insert(plan_steps.end(), pit->second.begin(),
                            pit->second.end());
        }
      }
      if (!ok) continue;
      // premises proven; apply the motif (derives the head fact into Gamma,
      // enriching later candidate spaces)
      const auto d = apply(c.rule, c.emb.data(),
                           static_cast<std::uint32_t>(c.emb.size()));
      if (!d) continue;  // static antecedent failure: candidate invalid
      plan.insert(fact_key(d->slot, d->src, d->dst));
      if (!best || plan.size() < best->size()) {
        AgentStep step;
        step.motif = Motif{c.rule, c.emb};
        step.cost = 1;
        step.success = true;
        step.derived = *d;
        plan_steps.push_back(std::move(step));
        best = std::make_shared<const FactSet>(std::move(plan));
        best_plan[tk] = std::move(plan_steps);
      }
    }
    if (!best) {
      failed_cap[tk] = {target_stamp(t), cap};
      return nullptr;
    }
    proof_memo[tk] = {target_stamp(t), best};
    return best->size() <= cap ? best : nullptr;
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
  const FactSetPtr best = search(top, cfg.budget, 0);

  res.expansions = expansions;
  if (best && best->size() <= cfg.budget) {
    res.goal_derived = true;
    res.termination = "goal";
    res.total_cost = static_cast<double>(best->size());
    res.certified_optimal = !shape_truncated && !cap_truncated;
    if (shape_truncated) res.termination = "goal_space_truncated";
    else if (cap_truncated) res.termination = "goal_cap_truncated";
    res.budget_exhausted = best->size() >= cfg.budget;
    const auto it = best_plan.find(TargetHash{}(top));
    if (it != best_plan.end()) res.chain = it->second;
    if (res.chain.empty()) {
      AgentStep only;  // single-fact proof (direct base match via pass A)
    }
  } else {
    res.goal_derived = false;
    res.termination =
        expansions > 0 ? "no_chain_within_budget" : "no_candidates";
    res.certified_no_chain = !shape_truncated && !cap_truncated;
    if (shape_truncated) res.termination = "no_chain_space_truncated";
    else if (cap_truncated) res.termination = "no_chain_cap_truncated";
    res.budget_exhausted = true;
  }
  res.derived = state->journal();
  std::fprintf(stderr,
               "[bwbnb] done: goal=%s cost=%.0f expansions=%llu "
               "derived=%zu space_truncated=%d(1=shape,2=cap) lb_prunes=%llu "
               "est_prunes=%llu inf_prunes=%llu probes=%llu\n",
               res.goal_derived ? "true" : "false",
               res.goal_derived && best ? static_cast<double>(best->size())
                                        : -1.0,
               static_cast<unsigned long long>(expansions),
               res.derived.size(),
               static_cast<int>(shape_truncated) +
                   2 * static_cast<int>(cap_truncated),
               static_cast<unsigned long long>(lb_entry_prunes),
               static_cast<unsigned long long>(est_prunes),
               static_cast<unsigned long long>(inf_prunes),
               static_cast<unsigned long long>(probe_calls));
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
