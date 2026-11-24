#include "gar/core/agent/executor.h"

#include <stdexcept>
#include <string>

#include "gar/core/agent/learning/goal_logistic_model.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"

namespace gar::core::engine::agent {

AgentResult LearnedStrategyExecutor::Run(const Goal& goal, AgentState* state,
                                         WitnessLog* witness,
                                         const AgentContext& ctx,
                                         const ExecConfig& cfg,
                                         const ExecKit& kit) {
  if (cfg.strategy_model_path.empty())
    throw std::invalid_argument(
        "LearnedStrategyExecutor requires --strategy-model");
  const learning::GoalLogisticModel model = learning::GoalLogisticModel::Load(
      cfg.strategy_model_path, "gar_strategy_v1");
  const auto& g = state->graph();
  const auto cone = gar::core::analysis::GoalConeRuleIndices(
      state->rules(), g.RelationName(goal.slot));
  const double degree = static_cast<double>(g.RelationPairs(goal.slot).size());
  const double p = model.Probability(static_cast<double>(cone.size()), degree);

  if (p >= model.threshold) {
    HybridExecutor hybrid;
    return hybrid.Run(goal, state, witness, ctx, cfg, kit);
  }
  AdaDeduceExecutor forward;
  return forward.Run(goal, state, witness, ctx, cfg, kit);
}

}  // namespace gar::core::engine::agent