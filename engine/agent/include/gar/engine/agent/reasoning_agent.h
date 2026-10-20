#pragma once

// ReasoningAgent: policy-guided deduction (paper §3-§5). This is the
// COMPOSITION ROOT of the agent engine: it owns the injected pluggable
// components (generator / scorer / policy / executor / cost model), holds
// the dependency graph of the rule set, and per Run computes the goal's
// d(.) table and wires the chain-level witness + failure registry.
//
// Layering: the agent engine knows gar::graph / gar::analysis / gar::hdc /
// gar::core only. It never sees the sibling deduction engine; composing
// agent output with a full closure (--verify-against-closure) happens at
// the app layer.

#include <cstdint>
#include <memory>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/executor.h"
#include "gar/hdc/encoder.h"

namespace gar {
namespace engine {
namespace agent {

class ReasoningAgent {
 public:
  struct Components {
    std::unique_ptr<MotifGenerator> generator;
    std::unique_ptr<Scorer> scorer;
    std::unique_ptr<Policy> policy;
    std::unique_ptr<Executor> executor;
    std::unique_ptr<CostModel> cost_model;
  };

  ReasoningAgent(Components comps, const analysis::DependencyGraph* dg,
                 hdc::HdcEncoder encoder)
      : comps_(std::move(comps)), dg_(dg), encoder_(encoder) {}

  // Runs one chain toward `goal`. Throws std::invalid_argument when the
  // goal's relation is not derivable by any rule (no d(.) table exists).
  AgentResult Run(AgentState* state, const Goal& goal,
                  const ExecConfig& cfg) const {
    AgentContext ctx{dg_, {}, &dg_->table(), encoder_};
    const std::string& goal_name = state->graph().RelationName(goal.slot);
    const std::uint32_t gp = ctx.table->Find(goal_name);
    if (gp == analysis::kInvalidPredicate) {
      throw std::invalid_argument("goal predicate not derivable by any "
                                  "rule: " +
                                  goal_name);
    }
    ctx.distances = dg_->DistancesTo(gp);

    WitnessLog witness(encoder_.dim(), &state->graph(), ctx.table);
    FailureRegistry failures;
    const ExecKit kit{comps_.generator.get(), comps_.scorer.get(),
                      comps_.policy.get(), comps_.cost_model.get()};
    return comps_.executor->Run(goal, state, &witness, &failures, ctx, cfg,
                                kit);
  }

 private:
  Components comps_;
  const analysis::DependencyGraph* dg_;
  hdc::HdcEncoder encoder_;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
