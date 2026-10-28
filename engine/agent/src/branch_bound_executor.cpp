#include "gar/engine/agent/branch_bound_executor.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

#include <sys/resource.h>

#include "apply_helpers.h"
#include "gar/graph/predicate_evaluator.h"
#include "gar/engine/agent/value_model.h"

// BranchBoundExecutor: HybNS (paper Figure 6), line by line. See
// branch_bound_executor.h for the branch-state design (TripleGraph clone
// per branch) and the deferred best-first evaluation.

namespace gar {
namespace engine {
namespace agent {

namespace {

// Open-set order (Figure 6 line 3): smallest priority = c + min price;
// ties by utility (higher first), then serial (push order) — fully
// deterministic.
struct BranchCmp {
  bool operator()(const std::unique_ptr<Branch>& a,
                  const std::unique_ptr<Branch>& b) const {
    if (a->priority != b->priority) return a->priority > b->priority;
    if (a->utility != b->utility) return a->utility < b->utility;
    return a->serial > b->serial;
  }
};
using OpenSet =
    std::priority_queue<std::unique_ptr<Branch>,
                        std::vector<std::unique_ptr<Branch>>, BranchCmp>;

// Pure logging for the offline value-function study (experiments/
// hdc_value): GAR_DEBUG_BNB=1 prints fork/eval/expand/goal events. No
// search, scoring or derivation semantics are touched.
bool DebugBnb() {
  static const bool on = std::getenv("GAR_DEBUG_BNB") != nullptr;
  return on;
}

std::unique_ptr<Branch> ForkBranch(const Branch& parent,
                                   std::uint64_t serial) {
  auto g = std::make_unique<graph::TripleGraph>(*parent.graph);
  auto child = std::make_unique<Branch>(std::move(g), parent.state->attrs(),
                                        parent.state->rules(), parent.witness,
                                        parent.failures, parent.cost,
                                        parent.utility, parent.chain, serial);
  child->parent_serial = parent.serial;
  if (DebugBnb()) {
    std::fprintf(stderr, "[bnbl] fork %llu %llu %.0f %.6f %zu\n",
                 static_cast<unsigned long long>(serial),
                 static_cast<unsigned long long>(parent.serial), parent.cost,
                 parent.utility, parent.chain.size());
  }
  return child;
}

// A pushed branch only survives evaluation when some candidate can satisfy
// c + price <= B and c + price < Ĉ; since price >= w0 >= 1, branches with
// c + 1 > B or c + 1 >= Ĉ provably die at evaluation. Not pushing them
// saves the clone; the bound itself is applied again at evaluation (line 4
// proper).
bool Viable(double cost, std::uint64_t budget, double best_cost) {
  return cost + 1.0 <= static_cast<double>(budget) && cost + 1.0 < best_cost;
}

// Deferred evaluation: generate, score and line-4-filter the branch's
// candidates; on survival, cache them re-ranked (line 6) and set the true
// priority. Returns false when the branch is to be discarded (line 5).
bool EvaluateBranch(Branch& b, const AgentContext& ctx,
                    const ExecConfig& cfg, const ExecKit& kit,
                    double best_cost) {
  std::vector<Motif> mus;
  kit.generator->Candidates(*b.state, ctx.distances, *ctx.table,
                            cfg.match_cap_per_rule, b.failures, &mus);
  if (mus.empty()) {
    if (DebugBnb()) {
      std::fprintf(stderr, "[bnbl] discard %llu\n",
                   static_cast<unsigned long long>(b.serial));
    }
    return false;
  }
  const ScoringContext sctx{&ctx.distances, ctx.table, &b.witness,
                            b.state.get()};
  std::vector<ScoredMotif> scored;
  kit.scorer->ScoreBatch(b.state->rules(), mus, sctx, &scored);
  // Attach the enumeration position for fully deterministic tie-breaks.
  std::vector<std::size_t> ord(scored.size());
  for (std::size_t i = 0; i < ord.size(); ++i) ord[i] = i;

  // Line 4: admissible, c + price(mu) <= B, c + price(mu) < Ĉ.
  std::vector<std::size_t> keep;
  for (const std::size_t i : ord) {
    const ScoredMotif& sm = scored[i];
    if (sm.admissible &&
        b.cost + sm.price <= static_cast<double>(cfg.budget) &&
        b.cost + sm.price < best_cost) {
      keep.push_back(i);
    }
  }
  if (keep.empty()) {
    if (DebugBnb()) {
      std::fprintf(stderr, "[bnbl] discard %llu\n",
                   static_cast<unsigned long long>(b.serial));
    }
    return false;
  }

  // Line 6, first half: keep the top-k by the score (k=0 keeps all — the
  // exact mode on which Theorem 5's optimality certificate rests).
  std::sort(keep.begin(), keep.end(), [&](std::size_t x, std::size_t y) {
    if (scored[x].total != scored[y].total) {
      return scored[x].total > scored[y].total;
    }
    return x < y;
  });
  if (cfg.top_k > 0 && keep.size() > cfg.top_k) {
    keep.resize(cfg.top_k);
  }
  // Line 6, second half: re-rank by price, ties by rel.
  std::sort(keep.begin(), keep.end(), [&](std::size_t x, std::size_t y) {
    if (scored[x].price != scored[y].price) {
      return scored[x].price < scored[y].price;
    }
    if (scored[x].rel != scored[y].rel) return scored[x].rel > scored[y].rel;
    return x < y;
  });

  std::vector<ScoredMotif> cached;
  cached.reserve(keep.size());
  for (const std::size_t i : keep) cached.push_back(std::move(scored[i]));
  b.priority = b.cost + cached.front().price;  // min price is up front now
  if (kit.value_model != nullptr) {
    // hdc-value priority (ordering only): cost + predicted remaining cost.
    // Feature order mirrors the meta `names` (and the [bnbl] eval log):
    // ncand minprice relfam relevd conf dcost total dp0 wit.
    const ScoredMotif& f0 = cached.front();
    const GARule& fr = b.state->rules()[f0.motif.rule];
    std::uint32_t dp0 = analysis::kUnreachable;
    if (fr.consequent) {
      const std::uint32_t pid =
          ctx.table->Find(analysis::PredicateIdOf(*fr.consequent, fr));
      if (pid != analysis::kInvalidPredicate) dp0 = ctx.distances[pid];
    }
    // chain_len follows the training convention: the fork-time PARENT
    // chain length (this branch's own chain minus its last step).
    const std::array<double, 11> raw = {
        b.cost,
        static_cast<double>(b.chain.empty() ? 0 : b.chain.size() - 1),
        static_cast<double>(cached.size()),
        f0.price,
        f0.rel_fam,
        f0.rel_evd,
        fr.quality.confidence,
        f0.dcost,
        f0.total,
        static_cast<double>(dp0),
        static_cast<double>(b.witness.Count())};
    b.priority = b.cost +
                 std::max(0.0, kit.value_model->PredictRemaining(raw));
  }
  b.scored = std::move(cached);
  b.evaluated = true;
  if (DebugBnb()) {
    const ScoredMotif& f0 = b.scored.front();
    const GARule& fr = b.state->rules()[f0.motif.rule];
    std::uint32_t dp0 = analysis::kUnreachable;
    if (fr.consequent) {
      const std::uint32_t pid =
          ctx.table->Find(analysis::PredicateIdOf(*fr.consequent, fr));
      if (pid != analysis::kInvalidPredicate) dp0 = ctx.distances[pid];
    }
    std::fprintf(stderr,
                 "[bnbl] eval %llu %zu %.3f %.4f %.4f %.4f %.4f %.4f %u %llu\n",
                 static_cast<unsigned long long>(b.serial), b.scored.size(),
                 f0.price, f0.rel_fam, f0.rel_evd, fr.quality.confidence,
                 f0.dcost, f0.total, dp0,
                 static_cast<unsigned long long>(b.witness.Count()));
  }
  return true;
}

}  // namespace

AgentResult BranchBoundExecutor::Run(const Goal& goal, AgentState* state,
                                     WitnessLog* witness,
                                     FailureRegistry* failures,
                                     const AgentContext& ctx,
                                     const ExecConfig& cfg,
                                     const ExecKit& kit) {
  AgentResult res;
  const auto& rules = state->rules();
  const double B = static_cast<double>(cfg.budget);

  // Same upfront check as the greedy executor: a fully ground goal that is
  // already present needs no chain.
  if (goal.args[0] && goal.args[1] &&
      state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    return res;
  }

  // Utility of a derived fact: w_q = gamma^{d(q)} (§3.2; d(q) is always
  // finite here — only admissible candidates, d(p0) < inf, are applied).
  auto fact_utility = [&](const graph::DerivedEdge& d) {
    const std::uint32_t id =
        ctx.table->Find(state->graph().RelationName(d.slot));
    const std::uint32_t dq = id == analysis::kInvalidPredicate
                                 ? analysis::kUnreachable
                                 : ctx.distances[id];
    return dq == analysis::kUnreachable
               ? 0.0
               : std::pow(cfg.utility_gamma, static_cast<double>(dq));
  };

  // Line 1: Ĉ = +inf, C* = none, F = {(Γ0, 0, ∅)}.
  double best_cost = std::numeric_limits<double>::infinity();
  std::vector<AgentStep> best_chain;
  double best_utility = 0.0;
  OpenSet open;
  std::uint64_t next_serial = 0;
  {
    auto g0 = std::make_unique<graph::TripleGraph>(state->graph());
    open.push(std::make_unique<Branch>(std::move(g0), state->attrs(),
                                       state->rules(), *witness, *failures,
                                       0.0, 0.0, std::vector<AgentStep>{},
                                       next_serial++));
    if (DebugBnb()) {
      std::fprintf(stderr, "[bnbl] root 0\n");
    }
  }

  // Anytime tracking: the highest-utility branch ever pushed (ties: lower
  // cost), snapshotted on improvement; the root qualifies with utility 0.
  std::unique_ptr<Branch> best_anytime;
  double best_anytime_utility = -1.0;
  auto track_anytime = [&](const Branch& b) {
    if (b.utility > best_anytime_utility ||
        (b.utility == best_anytime_utility && best_anytime &&
         b.cost < best_anytime->cost)) {
      best_anytime_utility = b.utility;
      best_anytime = ForkBranch(b, next_serial++);
    }
  };
  track_anytime(*open.top());

  // Line 2: while F is non-empty.
  while (!open.empty()) {
    std::unique_ptr<Branch> b =
        std::move(const_cast<std::unique_ptr<Branch>&>(open.top()));
    open.pop();

    // Deferred best-first evaluation (see branch_bound_executor.h): an
    // unevaluated branch only computes its true priority and re-queues.
    if (!b->evaluated) {
      if (EvaluateBranch(*b, ctx, cfg, kit, best_cost)) {
        open.push(std::move(b));
      }  // line 5: no surviving candidate -> discard the node
      continue;
    }
    ++res.expansions;
    if (res.expansions % 32 == 0) {
      std::fprintf(stderr, "[bnb] expanded=%llu open=%zu best_cost=%.0f\n",
                   static_cast<unsigned long long>(res.expansions),
                   open.size(),
                   best_cost == std::numeric_limits<double>::infinity()
                       ? -1.0
                       : best_cost);
    }

    // Line 7: apply the best motif (cheapest price, ties by rel — the
    // cache was re-ranked at evaluation).
    const ScoredMotif sm = b->scored.front();
    const Motif& mu = sm.motif;
    const GARule& rule = rules[mu.rule];
    const std::uint32_t* emb = mu.embedding.data();
    const std::uint32_t n =
        static_cast<std::uint32_t>(rule.pattern.num_vertices());
    const RuleHead head = ResolveRuleHead(rule);
    const std::uint64_t key = MotifHash(mu.rule, emb, n);
    const double c_mu = kit.cost_model->RuleLowerBound(rule);  // unit: 1

    // Same defensive guard as the greedy executor: the conclusion already
    // present means "not an application" — retire it statically on this
    // branch (a present head stays present) and re-queue uncharged.
    if (b->state->ContainsEdge(head.slot, emb[head.pu], emb[head.pv])) {
      b->failures.RecordFailure(key, /*blocking_pred_is_dynamic=*/false,
                                b->state->version());
      b->evaluated = false;
      b->scored.clear();
      if (Viable(b->cost, cfg.budget, best_cost)) open.push(std::move(b));
      continue;
    }

    const bool ok = graph::EvalAntecedent(rule.antecedent, emb,
                                          b->state->attrs(),
                                          b->state->graph());
    if (DebugBnb()) {
      std::fprintf(stderr, "[bnbl] expand %llu %u %d %.0f\n",
                   static_cast<unsigned long long>(b->serial), mu.rule,
                   static_cast<int>(ok), b->cost);
    }
    std::vector<Fact> premises;
    GroundPremises(rule, mu, b->state->graph(), *ctx.table, &premises);
    const std::size_t n_pattern = rule.pattern.num_edges();

    if (!ok) {
      // Line 8: failure. Classify static/dynamic (chase semantics) and
      // record against this fact set in BOTH continuations.
      const FailureInfo fi = ClassifyFailure(rule, emb, b->state->attrs(),
                                             b->state->graph(), ctx);
      AgentStep step{mu, false, c_mu, std::nullopt};

      // Charged continuation (Γ, c+c_mu, R): paid the false start, and its
      // witness log keeps the match ("a match computed is information
      // gained").
      if (Viable(b->cost + c_mu, cfg.budget, best_cost)) {
        auto charged = ForkBranch(*b, next_serial++);
        charged->cost += c_mu;
        if (kit.value_model != nullptr) {
          charged->priority = b->priority;  // inherit the model's key
        }
        charged->failures.RecordFailure(key, fi.dynamic,
                                        charged->state->version());
        const std::size_t upto =
            fi.fail_idx < rule.antecedent.size()
                ? n_pattern + fi.fail_idx + 1
                : premises.size();
        for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
          charged->witness.WitnessFact(premises[i], ctx.encoder);
        }
        charged->chain.push_back(step);
        track_anytime(*charged);
        open.push(std::move(charged));
      }
      // Unpaid sibling (Γ, c, R): skips the motif (retired at this fact
      // set — re-admitted on Gamma growth when dynamic) and carries no new
      // evidence.
      b->failures.RecordFailure(key, fi.dynamic, b->state->version());
      b->evaluated = false;
      b->scored.clear();
      if (Viable(b->cost, cfg.budget, best_cost)) open.push(std::move(b));
      continue;
    }

    // Lines 10-13: success. The success child owns Γ' = Γ ∪ {p0(h)}.
    auto child = ForkBranch(*b, next_serial++);
    child->cost += c_mu;
    if (kit.value_model != nullptr) {
      child->priority = b->priority;  // inherit the model's key
    }
    const graph::DerivedEdge d{emb[head.pu], emb[head.pv], head.slot,
                               mu.rule,
                               static_cast<std::uint32_t>(
                                   child->chain.size() + 1)};
    child->state->ApplyDerivation(d);
    child->utility += fact_utility(d);
    for (const auto& f : premises) {
      child->witness.WitnessFact(f, ctx.encoder);
    }
    child->witness.WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst),
                               ctx.encoder);
    child->chain.push_back(AgentStep{mu, true, c_mu, d});

    if (GoalSatisfied(goal, d) && child->cost <= B) {
      // Line 11: record the chain and tighten Ĉ. First-found wins ties
      // (strict <), keeping the result deterministic; the goal branch is
      // not pushed further (any extension is dominated).
      if (DebugBnb()) {
        std::fprintf(stderr, "[bnbl] goal %llu %.0f\n",
                     static_cast<unsigned long long>(child->serial),
                     child->cost);
      }
      if (child->cost < best_cost) {
        best_cost = child->cost;
        best_chain = child->chain;
        best_utility = child->utility;
      }
    } else if (child->cost <= B) {
      // Line 12: push the new state.
      track_anytime(*child);
      open.push(std::move(child));
    }

    // Line 13: the skip sibling — (Γ, c, R ∪ {(μ, Γ, static)}). Retiring a
    // success here loses nothing (Figure 6's simulation argument).
    b->failures.RecordFailure(key, /*blocking_pred_is_dynamic=*/false,
                              b->state->version());
    b->evaluated = false;
    b->scored.clear();
    if (Viable(b->cost, cfg.budget, best_cost)) {
      track_anytime(*b);
      open.push(std::move(b));
    }
  }

  if (!best_chain.empty()) {
    // C* recorded; exact when line 6 discarded no candidate (Theorem 5).
    res.chain = std::move(best_chain);
    res.total_cost = best_cost;
    res.utility = best_utility;
    res.goal_derived = true;
    res.certified_optimal = cfg.top_k == 0;
    res.budget_exhausted = best_cost >= B;
    res.termination = "goal";
  } else {
    // Line 14: anytime fallback — greedily extend the highest-utility
    // partial chain with admissible best-rel motifs while the budget
    // lasts. With top_k == 0 the bounds certified that no chain fits the
    // budget.
    res.certified_no_chain = cfg.top_k == 0;
    res.anytime_fallback = true;
    res.termination = "anytime_fallback";
    std::unique_ptr<Branch> fb = std::move(best_anytime);
    while (fb->cost < B) {
      if (!EvaluateBranch(*fb, ctx, cfg, kit,
                          std::numeric_limits<double>::infinity())) {
        break;
      }
      // Best-rel pick (ties: lower price, then enumeration order).
      const ScoredMotif* pick = nullptr;
      for (const auto& cand : fb->scored) {
        if (pick == nullptr || cand.rel > pick->rel ||
            (cand.rel == pick->rel && cand.price < pick->price)) {
          pick = &cand;
        }
      }
      const ScoredMotif sm = *pick;
      const Motif& mu = sm.motif;
      const GARule& rule = rules[mu.rule];
      const std::uint32_t* emb = mu.embedding.data();
      const std::uint32_t n =
          static_cast<std::uint32_t>(rule.pattern.num_vertices());
      const RuleHead head = ResolveRuleHead(rule);
      const double c_mu = kit.cost_model->RuleLowerBound(rule);
      const std::uint64_t key = MotifHash(mu.rule, emb, n);
      if (fb->state->ContainsEdge(head.slot, emb[head.pu], emb[head.pv])) {
        fb->failures.RecordFailure(key, false, fb->state->version());
        continue;
      }
      const bool ok = graph::EvalAntecedent(rule.antecedent, emb,
                                            fb->state->attrs(),
                                            fb->state->graph());
      std::vector<Fact> premises;
      GroundPremises(rule, mu, fb->state->graph(), *ctx.table, &premises);
      const std::size_t n_pattern = rule.pattern.num_edges();
      fb->cost += c_mu;
      if (ok) {
        const graph::DerivedEdge d{
            emb[head.pu], emb[head.pv], head.slot, mu.rule,
            static_cast<std::uint32_t>(fb->chain.size() + 1)};
        fb->state->ApplyDerivation(d);
        fb->utility += fact_utility(d);
        for (const auto& f : premises) {
          fb->witness.WitnessFact(f, ctx.encoder);
        }
        fb->witness.WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst),
                                ctx.encoder);
        fb->chain.push_back(AgentStep{mu, true, c_mu, d});
      } else {
        const FailureInfo fi =
            ClassifyFailure(rule, emb, fb->state->attrs(), fb->state->graph(),
                            ctx);
        fb->failures.RecordFailure(key, fi.dynamic, fb->state->version());
        const std::size_t upto =
            fi.fail_idx < rule.antecedent.size()
                ? n_pattern + fi.fail_idx + 1
                : premises.size();
        for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
          fb->witness.WitnessFact(premises[i], ctx.encoder);
        }
        fb->chain.push_back(AgentStep{mu, false, c_mu, std::nullopt});
      }
    }
    // The returned chain is the anytime partial chain — per the phase
    // contract goal_derived stays false (it is not a certified chain),
    // even when the greedy extension happens to reach the goal.
    res.chain = std::move(fb->chain);
    res.total_cost = fb->cost;
    res.utility = fb->utility;
    res.goal_derived = false;
    res.budget_exhausted = fb->cost >= B;
  }

  for (const auto& s : res.chain) {
    if (s.derived) res.derived.push_back(*s.derived);
  }
  std::fprintf(stderr,
               "[bnb] done: expanded=%llu goal=%s best_cost=%.0f "
               "certified_optimal=%d certified_no_chain=%d\n",
               static_cast<unsigned long long>(res.expansions),
               res.goal_derived ? "true" : "false",
               res.goal_derived ? best_cost : -1.0, res.certified_optimal,
               res.certified_no_chain);
  if (kit.value_model != nullptr) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    std::fprintf(stderr,
                 "[bnb] value-model: calls=%llu infer_ms=%.1f avg_us=%.2f "
                 "maxrss_mb=%ld\n",
                 static_cast<unsigned long long>(
                     kit.value_model->infer_calls),
                 kit.value_model->infer_ns / 1e6,
                 kit.value_model->infer_calls
                     ? kit.value_model->infer_ns / 1e3 /
                           static_cast<double>(kit.value_model->infer_calls)
                     : 0.0,
                 ru.ru_maxrss / 1024);
  }
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
