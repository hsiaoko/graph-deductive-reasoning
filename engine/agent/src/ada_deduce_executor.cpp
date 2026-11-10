// AdaDeduceExecutor: policy-guided deduction with suspension and revival
// (docs/fixpoint_implementation_plan.md; approved semantics: per-star
// expansion + dynamic-atom suspension + delta completion — NOT "complete Q
// match on the initial graph only").
//
// The public executor only constructs a DeduceSession and runs it; every
// mechanism lives in src-local fixpoint_*.{h,cpp}:
//   deduce_session      — one Run's state (R/W loops, scheduling)
//   partial_matcher      — rule plans, deferrable dynamic edges, sub-specs
//   working_set  — R (complete actions) / W (suspended)
//   precondition_cache— per-predicate status + the H cache
//   producer_tracker    — producer aliveness for safe pruning

#include "gar/engine/agent/executor.h"

#include "deduce_session.h"

namespace gar {
namespace engine {
namespace agent {

AgentResult AdaDeduceExecutor::Run(const Goal& goal, AgentState* state,
                                  WitnessLog* witness,
                                  FailureRegistry* failures,
                                  const AgentContext& ctx,
                                  const ExecConfig& cfg,
                                  const ExecKit& kit) {
  (void)failures;  // motif-level registry unused; H is the fixpoint cache
  return DeduceSession(goal, state, witness, ctx, cfg, kit).Run();
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
