#pragma once

// ReasoningAgent: policy-guided deduction (paper §3-§5). This is the
// COMPOSITION ROOT of the agent engine: it owns the injected pluggable
// components (scorer / policy / executor / cost model), holds
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
    std::unique_ptr<Scorer> scorer;
    std::unique_ptr<Policy> policy;
    std::unique_ptr<Executor> executor;
    std::unique_ptr<CostModel> cost_model;
    // Optional bnb branch value model (hdc-value priority, ordering only).
    std::shared_ptr<ValueModel> value_model;
    // --direction backward --executor greedy arm: single-path backward.
    bool backward_no_backtrack = false;
  };

  ReasoningAgent(Components comps, const analysis::DependencyGraph* dg,
                 hdc::HdcEncoder encoder, bool guide_with_dg = true)
      : comps_(std::move(comps)),
        dg_(dg),
        encoder_(encoder),
        guide_with_dg_(guide_with_dg) {}

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
    if (guide_with_dg_) {
      ctx.distances = dg_->DistancesTo(gp);
    } else {
      // --no-dg ablation: DG guidance off. An all-zero d(.) table makes
      // every consequent predicate look goal-adjacent: the generator's
      // d(p0)<inf hard filter never prunes, rel_fam = gamma^0 = 1 is
      // neutral, price = w0 + 0. The DG is still built (failure
      // classification reads its in-degrees); only guidance is removed.
      ctx.distances.assign(dg_->NumPredicates(), 0);
    }

    WitnessLog witness(encoder_.dim(), &state->graph(), ctx.table,
                       comps_.scorer->NeedsHdcWitness());
    FailureRegistry failures;
    const ExecKit kit{comps_.scorer.get(), comps_.policy.get(),
                      comps_.cost_model.get(), comps_.value_model.get()};
    ExecConfig cfg2 = cfg;
    cfg2.no_backtrack = comps_.backward_no_backtrack;
    return comps_.executor->Run(goal, state, &witness, &failures, ctx, cfg2,
                                kit);
  }

 private:
  Components comps_;
  const analysis::DependencyGraph* dg_;
  hdc::HdcEncoder encoder_;
  bool guide_with_dg_ = true;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
