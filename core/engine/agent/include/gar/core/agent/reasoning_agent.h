#pragma once

// ReasoningAgent: policy-guided deduction (paper §3-§5). This is the
// COMPOSITION ROOT of the agent engine: it owns the injected pluggable
// components (scorer / policy / executor / cost model / initial star guide), holds
// the dependency graph of the rule set, and per Run computes the goal's
// d(.) table and builds the chain-level witness state.
//
// Layering: the agent engine knows gar::core::graph / gar::core::analysis / gar::core::hdc /
// gar::core only. It never sees the sibling deduction engine; composing
// agent output with a full closure (--verify-against-closure) happens at
// the app layer.

#include <cstdint>
#include <memory>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/agent/executor.h"
#include "gar/core/hdc/encoder.h"

namespace gar::core::engine {
namespace agent {
namespace learning {
class RelevanceModel;
}  // namespace learning

class ReasoningAgent {
 public:
  struct Components {
    std::unique_ptr<Scorer> scorer;
    std::unique_ptr<Policy> policy;
    std::unique_ptr<Executor> executor;
    std::unique_ptr<CostModel> cost_model;
    std::shared_ptr<ValueModel> proof_star_model;
    std::shared_ptr<learning::RelevanceModel> relevance_model;
    std::unique_ptr<LlmStarGuide> star_guide;
    std::unique_ptr<PaperGuide> paper_guide;
  };

  ReasoningAgent(Components comps, const gar::core::analysis::DependencyGraph* dg,
                 gar::core::hdc::HdcEncoder encoder, bool guide_with_dg = true)
      : comps_(std::move(comps)),
        dg_(dg),
        encoder_(encoder),
        guide_with_dg_(guide_with_dg) {}

  // Runs one chain toward `goal`. Throws std::invalid_argument when the
  // goal's relation is not derivable by any rule (no d(.) table exists).
  AgentResult Run(AgentState* state, const Goal& goal,
                  const ExecConfig& cfg) const {
    if (goal.args[0] && goal.args[1] && cfg.policy_trace_path.empty() &&
        state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
      AgentResult result;
      result.goal_derived = true;
      result.termination = "goal_already_in_graph";
      return result;
    }
    for (const auto& rule : state->rules()) {
      for (const auto& pred : rule.antecedent) {
        if (pred.kind == gar::PredKind::kMl &&
            (state->ml_models() == nullptr ||
             state->ml_models()->Find(pred.model) == nullptr)) {
          throw std::invalid_argument("unregistered ML model: " + pred.model);
        }
      }
    }
    AgentContext ctx{dg_, {}, &dg_->table(), encoder_};
    const std::string& goal_name = state->graph().RelationName(goal.slot);
    const std::uint32_t gp = ctx.table->Find(goal_name);
    if (gp == gar::core::analysis::kInvalidPredicate) {
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
    const ExecKit kit{comps_.scorer.get(), comps_.policy.get(),
                      comps_.cost_model.get(), comps_.proof_star_model.get(),
                      comps_.relevance_model.get(), comps_.star_guide.get(),
                      comps_.paper_guide.get()};
    return comps_.executor->Run(goal, state, &witness, ctx, cfg, kit);
  }

 private:
  Components comps_;
  const gar::core::analysis::DependencyGraph* dg_;
  gar::core::hdc::HdcEncoder encoder_;
  bool guide_with_dg_ = true;
};

}  // namespace agent
}  // namespace gar::core::engine
