// FixpointExecutor: policy-guided deduction with suspension and revival
// (docs/fixpoint_implementation_plan.md; approved semantics: per-star
// expansion + dynamic-atom suspension + delta completion — NOT "complete Q
// match on the initial graph only").
//
// The public executor only constructs a FixpointSession and runs it; every
// mechanism lives in src-local fixpoint_*.{h,cpp}:
//   fixpoint_session      — one Run's state (R/W loops, scheduling)
//   fixpoint_matcher      — rule plans, deferrable dynamic edges, sub-specs
//   fixpoint_working_set  — R (complete actions) / W (suspended)
//   fixpoint_preconditions— per-predicate status + the H cache
//   fixpoint_producers    — producer aliveness for safe pruning

#include "gar/engine/agent/executor.h"

#include "fixpoint_session.h"

namespace gar {
namespace engine {
namespace agent {

AgentResult FixpointExecutor::Run(const Goal& goal, AgentState* state,
                                  WitnessLog* witness,
                                  FailureRegistry* failures,
                                  const AgentContext& ctx,
                                  const ExecConfig& cfg,
                                  const ExecKit& kit) {
  (void)failures;  // motif-level registry unused; H is the fixpoint cache
  return FixpointSession(goal, state, witness, ctx, cfg, kit).Run();
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
