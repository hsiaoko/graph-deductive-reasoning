#pragma once

// Executor: the agent loop driver. GreedyExecutor (this phase) commits to
// the policy's single choice per step; BranchBoundExecutor (next phase)
// will fork over the top-k — the interface already passes everything it
// needs (ExecKit collaborators, ExecConfig::top_k, per-goal AgentContext).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/cost_model.h"
#include "gar/engine/agent/failure_registry.h"
#include "gar/engine/agent/goal.h"
#include "gar/engine/agent/motif.h"
#include "gar/engine/agent/motif_generator.h"
#include "gar/engine/agent/policy.h"
#include "gar/engine/agent/scorer.h"
#include "gar/engine/agent/state.h"
#include "gar/engine/agent/witness.h"
#include "gar/hdc/encoder.h"

namespace gar {
namespace engine {
namespace agent {

// Per-goal run context, built by ReasoningAgent.
struct AgentContext {
  const analysis::DependencyGraph* dg;
  std::vector<std::uint32_t> distances;  // d(.) toward the goal predicate
  const analysis::PredicateTable* table;
  hdc::HdcEncoder encoder;
};

struct ExecConfig {
  std::uint64_t budget = 100;
  std::uint32_t match_cap_per_rule = 256;
  std::uint32_t top_k = 0;  // reserved for branch-and-bound (next phase)
};

// The collaborators an executor needs; ReasoningAgent fills it from its
// injected components.
struct ExecKit {
  const MotifGenerator* generator;
  const Scorer* scorer;
  Policy* policy;
  const CostModel* cost_model;
};

struct AgentStep {
  Motif motif;
  bool success = false;
  double cost = 0.0;
  std::optional<graph::DerivedEdge> derived;
};

struct AgentResult {
  std::vector<AgentStep> chain;
  std::vector<graph::DerivedEdge> derived;  // every fact derived, in order
  double total_cost = 0.0;
  bool goal_derived = false;
  bool budget_exhausted = false;
  std::string termination;  // "goal" | "no_candidates" | "budget" |
                            // "goal_already_in_graph"
};

class Executor {
 public:
  virtual ~Executor() = default;
  virtual AgentResult Run(const Goal& goal, AgentState* state,
                          WitnessLog* witness, FailureRegistry* failures,
                          const AgentContext& ctx, const ExecConfig& cfg,
                          const ExecKit& kit) = 0;
};

// GreedyExecutor (§5.1 deterministic loop): generate candidates -> score ->
// policy selects one -> apply. Every application is charged (success or
// failure; unit cost = 1). Terminates on goal derived, no candidates, or
// budget exhausted.
class GreedyExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
