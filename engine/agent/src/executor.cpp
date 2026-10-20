#include "gar/engine/agent/executor.h"

#include "apply_helpers.h"
#include "gar/graph/predicate_evaluator.h"

namespace gar {
namespace engine {
namespace agent {

AgentResult GreedyExecutor::Run(const Goal& goal, AgentState* state,
                                WitnessLog* witness, FailureRegistry* failures,
                                const AgentContext& ctx,
                                const ExecConfig& cfg, const ExecKit& kit) {
  AgentResult res;
  const auto& rules = state->rules();
  const graph::TripleGraph& g = state->graph();

  // A fully ground goal already present needs no chain.
  if (goal.args[0] && goal.args[1] &&
      state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    return res;
  }

  ScoringContext sctx{&ctx.distances, ctx.table, witness, state};
  double spent = 0.0;
  res.termination = "budget";

  while (spent < static_cast<double>(cfg.budget)) {
    std::vector<Motif> mus;
    kit.generator->Candidates(*state, ctx.distances, *ctx.table,
                              cfg.match_cap_per_rule, *failures, &mus);
    if (mus.empty()) {
      res.termination = "no_candidates";
      break;
    }
    std::vector<ScoredMotif> scored;
    kit.scorer->ScoreBatch(rules, mus, sctx, &scored);
    const auto sel = kit.policy->Select(scored);
    if (!sel) {
      res.termination = "no_candidates";
      break;
    }
    const ScoredMotif& sm = scored[*sel];
    const Motif& mu = sm.motif;
    const GARule& rule = rules[mu.rule];
    const std::uint32_t* emb = mu.embedding.data();
    const std::uint32_t n =
        static_cast<std::uint32_t>(rule.pattern.num_vertices());
    const RuleHead head = ResolveRuleHead(rule);

    AgentStep step;
    step.motif = mu;

    // Defensive: the conclusion became present between generation and
    // application (cannot happen in the serial greedy loop). Not an
    // application at all: retire it statically (a present head stays
    // present — facts only accumulate) and move on without charging.
    if (state->ContainsEdge(head.slot, emb[head.pu], emb[head.pv])) {
      failures->RecordFailure(MotifHash(mu.rule, emb, n),
                              /*blocking_pred_is_dynamic=*/false,
                              state->version());
      continue;
    }

    // Apply: test the preconditions under h — the exact chase semantics.
    const bool ok = graph::EvalAntecedent(rule.antecedent, emb,
                                          state->attrs(), g);
    step.cost = kit.cost_model->RuleLowerBound(rule);  // unit: exactly 1

    // Premise facts for witnessing (pattern edges, then antecedent in
    // order; every antecedent name of an applied rule is interned in the
    // table, so the two runs of GroundPremises align by index).
    std::vector<Fact> premises;
    GroundPremises(rule, mu, g, *ctx.table, &premises);
    const std::size_t n_pattern = rule.pattern.num_edges();

    if (ok) {
      step.success = true;
      const graph::DerivedEdge d{emb[head.pu], emb[head.pv], head.slot,
                                 mu.rule,
                                 static_cast<std::uint32_t>(
                                     res.chain.size() + 1)};
      state->ApplyDerivation(d);
      step.derived = d;
      for (const auto& f : premises) {
        witness->WitnessFact(f, ctx.encoder);
      }
      witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst), ctx.encoder);
      if (GoalSatisfied(goal, d)) {
        spent += step.cost;
        res.chain.push_back(std::move(step));
        res.total_cost = spent;
        res.goal_derived = true;
        res.termination = "goal";
        kit.policy->OnApplication(res.chain.back().motif, true,
                                  res.chain.back().cost);
        break;
      }
    } else {
      // Classify the blocking precondition (shared helper, chase
      // semantics): an edge predicate is dynamic iff some rule derives its
      // (base) relation, everything else is static (reads G only).
      const FailureInfo fi =
          ClassifyFailure(rule, emb, state->attrs(), g, ctx);
      failures->RecordFailure(MotifHash(mu.rule, emb, n), fi.dynamic,
                              state->version());
      // Witness the match and the evaluated prefix, failing one included
      // ("a match computed is information gained", §4.1.1).
      const std::size_t upto =
          fi.fail_idx < rule.antecedent.size()
              ? n_pattern + fi.fail_idx + 1
              : premises.size();
      for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
        witness->WitnessFact(premises[i], ctx.encoder);
      }
    }

    spent += step.cost;
    res.total_cost = spent;
    kit.policy->OnApplication(mu, step.success, step.cost);
    res.chain.push_back(std::move(step));
  }
  res.budget_exhausted = spent >= static_cast<double>(cfg.budget) &&
                         res.termination != "goal";
  res.derived = state->journal();
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
