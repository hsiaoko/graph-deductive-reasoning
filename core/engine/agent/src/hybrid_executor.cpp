#include <utility>

#include "gar/core/agent/executor.h"

namespace gar::core::engine::agent {

AgentResult HybridExecutor::Run(const Goal& goal, AgentState* state,
                                WitnessLog* witness, const AgentContext& ctx,
                                const ExecConfig& cfg, const ExecKit& kit) {
  BackwardExecutor backward;
  AgentResult back = backward.Run(goal, state, witness, ctx, cfg, kit);
  if (back.goal_derived) {
    return back;
  }

  // Backward exhausted its bounded reverse search without a proof (typical for
  // central predicates). Its derivations are sound and already committed to the
  // shared state, so forward AdaDeduce continues from there to completion.
  AdaDeduceExecutor forward;
  AgentResult fwd = forward.Run(goal, state, witness, ctx, cfg, kit);

  // Both executors report derived = state->journal(), so fwd.derived already
  // contains backward's committed facts; merging it again would double-count.
  // Only the per-phase chains and measured costs need combining.
  fwd.chain.insert(fwd.chain.begin(), back.chain.begin(), back.chain.end());
  fwd.execution_cost_ms += back.execution_cost_ms;
  fwd.logical_cost += back.logical_cost;
  fwd.goal_derived = fwd.goal_derived || back.goal_derived;
  return fwd;
}

}  // namespace gar::core::engine::agent