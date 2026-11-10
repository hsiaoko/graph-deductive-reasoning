#pragma once

// Executor: the agent loop driver. Exactly one executor implements the
// paper's Algorithm 6 (AdaDeduce): AdaDeduceExecutor. It drives the two
// working sets — ready R (policy-scheduled) and suspended W (revival-only)
// — over the goal cone. The interface passes everything it needs
// (ExecKit collaborators, per-goal AgentContext).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/cost_model.h"
#include "gar/engine/agent/failure_registry.h"
#include "gar/engine/agent/goal.h"
#include "gar/engine/agent/motif.h"
#include "gar/engine/agent/policy.h"
#include "gar/engine/agent/scorer.h"
#include "gar/engine/agent/state.h"
#include "gar/engine/agent/witness.h"
#include "gar/hdc/encoder.h"

namespace gar::core::engine {
namespace agent {

// Per-goal run context, built by ReasoningAgent.
struct AgentContext {
  const gar::core::analysis::DependencyGraph* dg;
  std::vector<std::uint32_t> distances;  // d(.) toward the goal predicate
  const gar::core::analysis::PredicateTable* table;
  gar::core::hdc::HdcEncoder encoder;
};

struct ExecConfig {
  std::uint64_t budget = 100;
  std::uint32_t match_cap_per_rule = 256;
  // Subgoal-utility decay (§3.2): a derived fact on predicate q earns
  // w_q = utility_gamma^{d(q)}. Mirrors the scorer's gamma; the app wires
  // HandcraftedConfig::gamma through.
  double utility_gamma = 0.5;
  std::uint64_t seed = 42;
  // Wall-clock limit in seconds (0 = off). Checked inside the executor
  // loops so a wall-limited run still terminates cleanly (termination
  // "timeout") and writes its partial chain/journal — an external kill
  // would lose them.
  double wall_limit_sec = 0.0;
  // Every `fair_interval` policy selections, force the longest-waiting
  // ready action (0 = off, pure policy order). The fair picks are marked
  // in the event trace.
  std::uint32_t fair_interval = 0;
  // "input" preserves rule-path order; "goal" stable-sorts stars by
  // consequent-to-goal DG distance. Both expand every cone star unless the
  // goal, a cap, or a budget/wall limit stops the run.
  std::string star_order = "input";
  // Optional per-evaluated-action trace for offline cost-to-go training.
  // Empty disables all tracing overhead; labels are finite only on goal hits.
  std::string q_trace_path;
};

// The collaborators an executor needs; ReasoningAgent fills it from its
// injected components.
class ValueModel;
struct ExecKit {
  const Scorer* scorer;
  Policy* policy;
  const CostModel* cost_model;
  // Optional value model for BNB branch or AdaDeduce action ordering.
  const ValueModel* value_model = nullptr;
};

struct AgentStep {
  Motif motif;
  bool success = false;
  double cost = 0.0;
  std::optional<gar::core::graph::DerivedEdge> derived;
};

struct AgentResult {
  std::vector<AgentStep> chain;
  std::vector<gar::core::graph::DerivedEdge> derived;  // every fact derived, in order
  double total_cost = 0.0;
  double utility = 0.0;       // sum of w_q over derived facts (§3.2)
  bool goal_derived = false;
  bool budget_exhausted = false;
  std::string termination;  // "goal" | "no_candidates" | "budget" |
                            // "timeout" | "candidate_cap" | "fixpoint"

  // AdaDeduceExecutor instrumentation (only set by it): event trace
  // (select/fair_select/evaluate/suspend/revive/prune/derive, one text row
  // per event, written by the app to deduce_trace.tsv) and counters.
  // Cost separation (the backward cost-semantics lesson): proof_cost =
  // cost of the successful derivation chain; search_cost = every charged
  // evaluation (incl. failed/suspended); selection/evaluation event counts
  // are separate counters; wall is wall.
  struct DeduceStats {
    std::uint64_t select_events = 0;
    std::uint64_t fair_select_events = 0;
    std::uint64_t evaluate_events = 0;   // == search_cost charged
    std::uint64_t suspend_events = 0;
    std::uint64_t revive_events = 0;
    std::uint64_t prune_events = 0;
    std::uint64_t derive_events = 0;
    std::uint64_t candidate_caps = 0;    // enumerations that hit match cap
    std::uint64_t ready_peak = 0;
    std::uint64_t suspended_peak = 0;
    std::uint64_t generation_events = 0;
    std::uint64_t match_embeddings = 0;  // label-valid full embeddings
    std::uint64_t ready_insertions = 0;  // includes revival re-enqueues
    std::uint64_t revival_passes = 0;
    std::uint64_t score_calls = 0;
    std::uint64_t scored_candidates = 0;
    double search_cost = 0.0;
    double proof_cost = 0.0;
    double prepare_wall_ms = 0.0;
    double generation_wall_ms = 0.0;
    double selection_wall_ms = 0.0;
    double score_wall_ms = 0.0;
    double ready_remove_wall_ms = 0.0;
    double evaluation_wall_ms = 0.0;
    double derivation_wall_ms = 0.0;
    double delta_wall_ms = 0.0;
    double delta_complete_wall_ms = 0.0;
    double partial_complete_wall_ms = 0.0;
    double partial_grow_wall_ms = 0.0;
    double revival_wall_ms = 0.0;
    std::vector<std::string> trace;
  };
  std::optional<DeduceStats> stats;
};

class Executor {
 public:
  virtual ~Executor() = default;
  virtual AgentResult Run(const Goal& goal, AgentState* state,
                          WitnessLog* witness, FailureRegistry* failures,
                          const AgentContext& ctx, const ExecConfig& cfg,
                          const ExecKit& kit) = 0;
};

// AdaDeduceExecutor (docs/fixpoint_implementation_plan.md): two working sets
// and two loops. The MAIN loop generates actions for goal-cone rules
// (complete matches on G∪Γ plus partial matches that defer one dynamic
// pattern edge), schedules only R via the injected Policy, and evaluates
// every precondition (collecting all misses): full pass derives, static
// failure prunes (shared via the precondition cache), dynamic absence
// suspends into W. The revival loop revives W entries whose missing facts
// arrived (or whose partial wait completed on a delta) and prunes dead
// waits (no live producer). policy never schedules W.
class AdaDeduceExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

}  // namespace agent
}  // namespace gar::core::engine
