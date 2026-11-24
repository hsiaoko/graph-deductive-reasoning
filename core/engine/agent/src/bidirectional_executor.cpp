#include <utility>

#include "gar/core/agent/executor.h"

namespace gar::core::engine::agent {

AgentResult BidirectionalExecutor::Run(const Goal& goal, AgentState* state,
                                       WitnessLog* witness,
                                       const AgentContext& ctx,
                                       const ExecConfig& cfg,
                                       const ExecKit& kit) {
  if (!goal.args[0] || !goal.args[1]) {
    throw std::invalid_argument(
        "BidirectionalExecutor requires a fully bound goal");
  }

  // A bounded backward attempt first keeps the cheap proof-directed win on
  // shallow goals (backward derives a handful of facts and stops).
  BackwardExecutor backward;
  AgentResult acc = backward.Run(goal, state, witness, ctx, cfg, kit);
  if (acc.goal_derived) {
    return acc;
  }

  // Backward exhausted its bounded reverse search (central predicate). Alternate
  // bounded forward frontier passes with backward connect passes over the shared
  // state: forward derives the middle facts backward would otherwise recurse to
  // find, so each backward pass meets the forward frontier sooner.
  auto merge = [&acc](AgentResult& part) {
    acc.chain.insert(acc.chain.end(), part.chain.begin(), part.chain.end());
    acc.execution_cost_ms += part.execution_cost_ms;
    acc.logical_cost += part.logical_cost;
  };
  auto finish = [&]() {
    acc.derived = state->journal();
    acc.goal_derived =
        state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1]);
    acc.termination = acc.goal_derived ? "goal" : "bidirectional_exhausted";
    return std::move(acc);
  };

  std::size_t last_journal = state->journal().size();
  for (std::uint32_t round = 0; round < cfg.bidirectional_rounds; ++round) {
    ExecConfig fwd_cfg = cfg;
    fwd_cfg.budget = cfg.bidirectional_forward_budget;
    AdaDeduceExecutor forward;
    AgentResult fwd = forward.Run(goal, state, witness, ctx, fwd_cfg, kit);
    merge(fwd);
    if (fwd.goal_derived) {
      return finish();
    }

    BackwardExecutor connect;
    AgentResult back = connect.Run(goal, state, witness, ctx, cfg, kit);
    merge(back);
    if (back.goal_derived) {
      return finish();
    }

    // Neither direction committed new facts: further alternation cannot meet.
    const std::size_t now = state->journal().size();
    if (now == last_journal) {
      break;
    }
    last_journal = now;
  }

  // Alternation exhausted without a proof: fall back to full forward AdaDeduce
  // from the shared state so completeness is preserved (degrades to hybrid on
  // goals where the two searches cannot meet).
  if (!acc.goal_derived) {
    AdaDeduceExecutor forward;
    AgentResult fwd = forward.Run(goal, state, witness, ctx, cfg, kit);
    merge(fwd);
  }
  return finish();
}

}  // namespace gar::core::engine::agent