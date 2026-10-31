#include "gar/engine/agent/executor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "apply_helpers.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"
#include "rule_support.h"

// GreedyExecutor — the INCREMENTAL forward engine (replaces the removed
// naive implementation that re-enumerated every DG-reachable rule per step
// and applied a single candidate).
//
// Two-stage policy-guided loop:
//   1. RULE level (policy-first): the policy picks one rule by its static
//      score (ScoredRule: rel_fam/confidence/price — DG + cost model only,
//      no embeddings). Rules that cannot reach the goal in the DG never
//      enter the candidate set (§5.3 hard filter).
//   2. MOTIF level: only the chosen rule is matched. First expansion of a
//      rule is a full (capped) enumeration; later steps reuse the
//      persistent candidate cache, re-filtered for head-existence and the
//      failure registry instead of re-enumerating.
//
// Delta matching: a derivation (slot s, src, dst) only affects initialized
// rules whose pattern references slot s (or its mirror slot). For each such
// pattern edge the endpoints are pinned to the new fact's endpoints and the
// enumeration yields exactly the embeddings containing that delta edge;
// results are merged into the cache with full-embedding dedup (a rule
// referencing the same slot several times re-finds the same embedding once
// per edge). Rules not referencing the slot are untouched.
//
// Candidate lifecycle per rule: `cands` holds active motifs (insertion
// order = deterministic enumeration order); `hold` holds motifs retired by
// a recorded failure — dynamic failures re-enter `cands` once the fact
// version moves past the failure (FailureRegistry semantics). `keys` is the
// dedup index over both lists.
//
// Goal pins: when the goal is (partially) grounded, rules whose head IS the
// goal predicate get the head endpoints pinned to the goal constants — at
// the initial enumeration and every delta. A delta pin that conflicts with
// a goal pin on the same pattern vertex voids that delta (the intersection
// is empty by construction). Constants are NOT propagated along multi-hop
// DG paths (that would be Magic Sets; not implemented and not faked).
//
// Truncation is explicit and per-enumeration-CALL, not per-rule-global:
// --max-candidates-per-rule bounds the rule's initial enumeration AND each
// delta enumeration (one call per matching pattern edge per derivation).
// The cache accumulates across deltas, so a rule's cached candidate count
// may exceed the cap. This is NOT the removed naive engine's "first cap
// embeddings of every per-step full enumeration" window: neither space is
// a subset of the other in general (delta windows are pinned to a single
// new fact, so they rarely hit the cap; the union over deltas tracks graph
// growth instead of a frozen prefix). "no_candidates" is the fixpoint of
// THAT space; whenever any enumeration reports capped (stats.capped_enums
// > 0) the engine makes no completeness claim. Under --forward-full-enum
// (open goals) that fixpoint becomes "full_enum_fixpoint" once at least
// one goal fact was derived.
//
// Debug self-check: GAR_VERIFY_INCREMENTAL=1 re-enumerates every
// delta-touched rule in full after each merge and aborts on any set
// mismatch (development validation only; O(full) per step).

namespace gar {
namespace engine {
namespace agent {

namespace {

// Per-rule persistent state: the prepared matching context and the
// candidate cache, initialized lazily on the rule's first selection.
struct RuleEntry {
  ScoredRule sr;              // static rule-level score
  RuleHead head;              // consequent resolution
  graph::PatternSpec spec;    // prepared once
  bool initialized = false;   // first full enumeration done
  bool capped = false;        // any enumeration hit the match cap
  std::vector<Motif> cands;   // active motifs (enumeration order)
  std::vector<Motif> hold;    // retired motifs (awaiting re-admission)
  std::unordered_set<std::uint64_t> keys;  // MotifHash over cands ∪ hold
  // Version at which the policy declined this rule's entire cache
  // (Policy::Select nullopt). The rule becomes eligible again only after
  // the fact set grows — mirrors the failure registry's re-admission
  // semantics and prevents an uncharged decline loop.
  std::uint64_t declined_version = UINT64_MAX;
};

bool VerifyIncremental() {
  static const bool on = std::getenv("GAR_VERIFY_INCREMENTAL") != nullptr;
  return on;
}

}  // namespace

AgentResult GreedyExecutor::Run(const Goal& goal, AgentState* state,
                                WitnessLog* witness, FailureRegistry* failures,
                                const AgentContext& ctx,
                                const ExecConfig& cfg, const ExecKit& kit) {
  AgentResult res;
  res.fwd_stats.emplace();
  auto& stats = *res.fwd_stats;
  const auto& rules = state->rules();
  const graph::TripleGraph& g = state->graph();
  const graph::PatternMatcher matcher(g);

  // A fully ground goal already present needs no chain.
  if (goal.args[0] && goal.args[1] &&
      state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    return res;
  }

  const bool full_enum =
      cfg.forward_full_enum && (!goal.args[0] || !goal.args[1]);
  const auto t0 = std::chrono::steady_clock::now();
  auto wall_exceeded = [&]() {
    return cfg.wall_limit_sec > 0.0 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         t0)
                   .count() >= cfg.wall_limit_sec;
  };
  auto ms_since = [](std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - a)
        .count();
  };

  ScoringContext sctx{&ctx.distances, ctx.table, witness, state};

  // ---- rule-level preparation (once per run) ----
  std::vector<RuleEntry> entries;
  // slot -> entries whose pattern references it (delta dispatch).
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> slot_users;
  {
    const auto p0 = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < rules.size(); ++i) {
      const GARule& r = rules[i];
      RuleHead head;
      if (!RuleSupported(r, &head)) continue;
      if (!RuleReachesGoal(r, ctx.distances, *ctx.table)) continue;
      RuleEntry e;
      e.sr.rule = i;
      kit.scorer->ScoreRule(r, sctx, &e.sr);
      e.head = head;
      e.spec = BuildSpec(r);
      for (const auto& edge : e.spec.edges) {
        slot_users[edge[2]].push_back(
            static_cast<std::uint32_t>(entries.size()));
      }
      entries.push_back(std::move(e));
    }
    stats.match_wall_ms += ms_since(p0);
  }

  // Goal pins for rules whose head IS the goal predicate (head constants
  // only — no multi-hop propagation).
  auto goal_pins = [&](const RuleEntry& e) {
    std::unordered_map<std::uint32_t, std::uint32_t> pins;
    if (e.head.slot == goal.slot) {
      if (goal.args[0]) pins[e.head.pu] = *goal.args[0];
      if (goal.args[1]) pins[e.head.pv] = *goal.args[1];
    }
    return pins;
  };

  // Insert one raw embedding into the cache: label filter, head-existence,
  // full-embedding dedup; retired motifs go to `hold` (dynamic failures
  // return on version growth). Returns true if the embedding was new.
  auto cache_insert = [&](RuleEntry& e, const std::uint32_t* emb) {
    const GARule& r = rules[e.sr.rule];
    const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
    if (!VertexLabelsOk(r, g, emb)) return false;
    if (state->ContainsEdge(e.head.slot, emb[e.head.pu], emb[e.head.pv])) {
      return false;
    }
    const std::uint64_t key = MotifHash(e.sr.rule, emb, n);
    if (!e.keys.insert(key).second) return false;  // duplicate
    Motif mu{e.sr.rule, std::vector<std::uint32_t>(emb, emb + n)};
    if (failures->IsRetired(key, state->version())) {
      e.hold.push_back(std::move(mu));
    } else {
      e.cands.push_back(std::move(mu));
    }
    return true;
  };

  // First expansion of a rule: one full (capped, possibly goal-pinned)
  // enumeration.
  auto initialize = [&](RuleEntry& e) {
    const auto m0 = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> embs;
    bool capped = false;
    const std::uint64_t n_emb = matcher.Enumerate(
        e.spec, cfg.match_cap_per_rule, &embs, &capped, goal_pins(e));
    e.initialized = true;
    e.capped = capped;
    ++stats.rules_initialized;
    stats.capped_enums += capped ? 1 : 0;
    stats.embeddings_generated += n_emb;
    const auto n = e.spec.num_vertices;
    for (std::uint64_t t = 0; t < n_emb; ++t) {
      cache_insert(e, embs.data() + t * n);
    }
    stats.match_wall_ms += ms_since(m0);
  };

  // Delta update after deriving d: only initialized rules whose pattern
  // references d.slot (or its mirror). Each matching pattern edge gets both
  // endpoints pinned to the new fact's endpoints; a pin conflicting with a
  // goal pin on the same vertex voids that edge's delta (empty by
  // construction). Every rule that received a delta enumeration is appended
  // to `touched` (for the GAR_VERIFY_INCREMENTAL equivalence check).
  auto delta_update = [&](const graph::DerivedEdge& d,
                          std::vector<std::uint32_t>* touched) {
    const std::uint32_t mirror = g.IsInverse(d.slot)
                                     ? g.BaseRelation(d.slot)
                                     : d.slot + g.num_relations();
    for (const std::uint32_t slot : {d.slot, mirror}) {
      const auto it = slot_users.find(slot);
      if (it == slot_users.end()) continue;
      const bool flipped = (slot == mirror);
      for (const std::uint32_t ei : it->second) {
        RuleEntry& e = entries[ei];
        if (!e.initialized) continue;  // first selection enumerates in full
        bool enumerated = false;
        for (const auto& edge : e.spec.edges) {
          if (edge[2] != slot) continue;
          const auto m0 = std::chrono::steady_clock::now();
          auto pins = goal_pins(e);
          const std::uint32_t ps = flipped ? d.dst : d.src;
          const std::uint32_t pd = flipped ? d.src : d.dst;
          auto pit = pins.find(edge[0]);
          if (pit != pins.end() && pit->second != ps) continue;
          auto qit = pins.find(edge[1]);
          if (qit != pins.end() && qit->second != pd) continue;
          pins[edge[0]] = ps;
          pins[edge[1]] = pd;
          std::vector<std::uint32_t> embs;
          bool capped = false;
          const std::uint64_t n_emb = matcher.Enumerate(
              e.spec, cfg.match_cap_per_rule, &embs, &capped, pins);
          enumerated = true;
          ++stats.delta_enums;
          stats.capped_enums += capped ? 1 : 0;
          stats.embeddings_generated += n_emb;
          e.capped = e.capped || capped;
          const auto n = e.spec.num_vertices;
          for (std::uint64_t t = 0; t < n_emb; ++t) {
            cache_insert(e, embs.data() + t * n);
          }
          stats.match_wall_ms += ms_since(m0);
        }
        if (enumerated &&
            std::find(touched->begin(), touched->end(), ei) ==
                touched->end()) {
          touched->push_back(ei);
        }
      }
    }
  };

  // GAR_VERIFY_INCREMENTAL: after a delta merge, the rule's key set must
  // equal a fresh full enumeration at the same fact version (same filters).
  auto verify_rule = [&](const RuleEntry& e) {
    std::vector<std::uint32_t> embs;
    bool capped = false;
    const std::uint64_t n_emb = matcher.Enumerate(
        e.spec, cfg.match_cap_per_rule, &embs, &capped, goal_pins(e));
    std::unordered_set<std::uint64_t> want;
    const auto n = e.spec.num_vertices;
    const GARule& r = rules[e.sr.rule];
    for (std::uint64_t t = 0; t < n_emb; ++t) {
      const std::uint32_t* emb = embs.data() + t * n;
      if (!VertexLabelsOk(r, g, emb)) continue;
      if (state->ContainsEdge(e.head.slot, emb[e.head.pu], emb[e.head.pv])) {
        continue;
      }
      want.insert(MotifHash(e.sr.rule, emb, n));
    }
    std::unordered_set<std::uint64_t> have;
    auto collect = [&](const std::vector<Motif>& ms) {
      for (const Motif& m : ms) {
        const std::uint32_t* emb = m.embedding.data();
        if (state->ContainsEdge(e.head.slot, emb[e.head.pu],
                                emb[e.head.pv])) {
          continue;  // stale key, dropped at the next refilter
        }
        have.insert(MotifHash(m.rule, emb,
                              static_cast<std::uint32_t>(
                                  m.embedding.size())));
      }
    };
    collect(e.cands);
    collect(e.hold);
    if (want != have) {
      std::fprintf(stderr,
                   "[verify-incremental] MISMATCH rule=%s want=%zu have=%zu\n",
                   r.name.substr(0, 80).c_str(), want.size(), have.size());
      std::abort();
    }
  };

  double spent = 0.0;
  res.termination = "budget";

  while (spent < static_cast<double>(cfg.budget)) {
    if (wall_exceeded()) {
      res.termination = "timeout";
      break;
    }
    // Active rules: uninitialized, holding active candidates, or holding a
    // retired motif that the CURRENT fact version re-admits (dynamic
    // failures re-enter on ANY growth of Gamma — a rule whose pattern gains
    // no new embeddings can still become applicable because its antecedent
    // did). A rule the policy declined at this version stays inactive until
    // the version moves (no uncharged decline loop; facts only accumulate,
    // so a still-declined/still-retired rule genuinely has nothing new).
    std::vector<ScoredRule> active;
    std::vector<std::uint32_t> active_idx;
    const std::uint64_t version = state->version();
    for (std::uint32_t ei = 0; ei < entries.size(); ++ei) {
      const RuleEntry& e = entries[ei];
      if (!e.initialized) {
        active.push_back(e.sr);
        active_idx.push_back(ei);
        continue;
      }
      if (!e.cands.empty() && version != e.declined_version) {
        active.push_back(e.sr);
        active_idx.push_back(ei);
        continue;
      }
      bool readmittable = false;
      for (const Motif& m : e.hold) {
        if (!failures->IsRetired(
                MotifHash(m.rule, m.embedding.data(),
                          static_cast<std::uint32_t>(m.embedding.size())),
                version)) {
          readmittable = true;
          break;
        }
      }
      if (readmittable) {
        active.push_back(e.sr);
        active_idx.push_back(ei);
      }
    }
    if (active.empty()) {
      res.termination = "no_candidates";
      break;
    }
    const auto sel = kit.policy->SelectRule(active);
    ++stats.rule_selections;
    if (!sel) {
      res.termination = "no_candidates";
      break;
    }
    RuleEntry& e = entries[active_idx[*sel]];

    if (!e.initialized) initialize(e);
    // Re-filter instead of re-enumerating: drop candidates whose conclusion
    // entered the graph since insertion; re-admit held motifs whose dynamic
    // failure aged out (version moved).
    {
      std::size_t w = 0;
      for (std::size_t k = 0; k < e.cands.size(); ++k) {
        const Motif& mu = e.cands[k];
        const std::uint32_t* emb = mu.embedding.data();
        const auto n = static_cast<std::uint32_t>(mu.embedding.size());
        if (state->ContainsEdge(e.head.slot, emb[e.head.pu],
                                emb[e.head.pv])) {
          e.keys.erase(MotifHash(mu.rule, emb, n));
          continue;
        }
        e.cands[w++] = e.cands[k];
        ++stats.candidates_reused;
      }
      e.cands.resize(w);
      std::size_t hw = 0;
      for (std::size_t k = 0; k < e.hold.size(); ++k) {
        const Motif& mu = e.hold[k];
        const auto n = static_cast<std::uint32_t>(mu.embedding.size());
        const std::uint64_t key =
            MotifHash(mu.rule, mu.embedding.data(), n);
        if (state->ContainsEdge(e.head.slot, mu.embedding.data()[e.head.pu],
                                mu.embedding.data()[e.head.pv])) {
          e.keys.erase(key);
          continue;
        }
        if (!failures->IsRetired(key, state->version())) {
          e.cands.push_back(mu);  // re-admitted
        } else {
          e.hold[hw++] = e.hold[k];
        }
      }
      e.hold.resize(hw);
    }
    if (e.cands.empty()) continue;  // uncharged: no application happened

    // Motif level: score only this rule's cached candidates.
    const auto s0 = std::chrono::steady_clock::now();
    std::vector<ScoredMotif> scored;
    kit.scorer->ScoreBatch(rules, e.cands, sctx, &scored);
    const auto pick = kit.policy->Select(scored);
    stats.score_wall_ms += ms_since(s0);
    if (!pick) {
      // Policy declined every candidate: keep the cache and its dedup keys
      // intact (clearing cands without keys would let a later delta
      // re-insert then get dedup-blocked, corrupting the cache). Mark the
      // rule declined at this fact version; it becomes eligible again only
      // after the graph grows (same re-admission discipline as the failure
      // registry), so a declining policy cannot spin uncharged.
      e.declined_version = state->version();
      continue;
    }

    const Motif& mu = scored[*pick].motif;
    const GARule& rule = rules[mu.rule];
    const std::uint32_t* emb = mu.embedding.data();
    const auto n = static_cast<std::uint32_t>(rule.pattern.num_vertices());
    const std::uint64_t mkey = MotifHash(mu.rule, emb, n);
    // Remove the applied motif from the cache (success: its conclusion is
    // now in the graph; failure: retired in the registry).
    auto erase_motif = [&](RuleEntry& re) {
      for (std::size_t k = 0; k < re.cands.size(); ++k) {
        if (MotifHash(re.cands[k].rule, re.cands[k].embedding.data(),
                      static_cast<std::uint32_t>(
                          re.cands[k].embedding.size())) == mkey) {
          re.cands.erase(re.cands.begin() + k);
          return;
        }
      }
    };

    const auto a0 = std::chrono::steady_clock::now();
    AgentStep step;
    step.motif = mu;
    step.cost = kit.cost_model->RuleLowerBound(rule);  // unit: exactly 1

    std::vector<Fact> premises;
    GroundPremises(rule, mu, g, *ctx.table, &premises);
    const std::size_t n_pattern = rule.pattern.num_edges();
    const bool ok =
        graph::EvalAntecedent(rule.antecedent, emb, state->attrs(), g);

    if (ok) {
      step.success = true;
      const graph::DerivedEdge d{emb[e.head.pu], emb[e.head.pv],
                                 e.head.slot, mu.rule,
                                 static_cast<std::uint32_t>(
                                     res.chain.size() + 1)};
      state->ApplyDerivation(d);
      step.derived = d;
      for (const auto& f : premises) {
        witness->WitnessFact(f, ctx.encoder);
      }
      witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst), ctx.encoder);
      if (GoalSatisfied(goal, d)) res.goal_derived = true;
      e.keys.erase(mkey);
      erase_motif(e);
      stats.apply_wall_ms += ms_since(a0);
      std::vector<std::uint32_t> touched;
      delta_update(d, &touched);
      if (VerifyIncremental()) {
        // Every rule this delta touched — not just the applied one — must
        // match a fresh full enumeration at the new fact version.
        for (const std::uint32_t ei : touched) verify_rule(entries[ei]);
      }
    } else {
      const FailureInfo fi =
          ClassifyFailure(rule, emb, state->attrs(), g, ctx);
      failures->RecordFailure(mkey, fi.dynamic, state->version());
      erase_motif(e);
      e.hold.push_back(mu);
      const std::size_t upto =
          fi.fail_idx < rule.antecedent.size() ? n_pattern + fi.fail_idx + 1
                                               : premises.size();
      for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
        witness->WitnessFact(premises[i], ctx.encoder);
      }
      stats.apply_wall_ms += ms_since(a0);
    }

    spent += step.cost;
    res.total_cost = spent;
    kit.policy->OnApplication(mu, step.success, step.cost);
    res.chain.push_back(std::move(step));

    if (!full_enum && res.goal_derived) {
      res.termination = "goal";
      break;
    }
  }
  if (full_enum && res.termination == "no_candidates" && res.goal_derived) {
    res.termination = "full_enum_fixpoint";
  }
  res.budget_exhausted = spent >= static_cast<double>(cfg.budget) &&
                         res.termination != "goal";
  res.derived = state->journal();
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
