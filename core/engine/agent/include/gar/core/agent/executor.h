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

#include "gar/core/agent/config.h"
#include "gar/core/agent/cost_model.h"
#include "gar/core/agent/goal.h"
#include "gar/core/agent/motif.h"
#include "gar/core/agent/policy.h"
#include "gar/core/agent/scorer.h"
#include "gar/core/agent/state.h"
#include "gar/core/agent/witness.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/hdc/encoder.h"

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
  std::uint32_t batch_size = 1;
  MatchEngine engine = MatchEngine::kCpu;
  std::uint32_t match_cap_per_rule = 256;
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
  // When R is empty, run completion before the next star once W reaches this
  // soft size threshold. Zero defers completion until every star is expanded.
  std::uint64_t revival_threshold = 0;
  // Input preserves rule-path order; goal stable-sorts stars by
  // consequent-to-goal DG distance; learned picks between the two per goal via
  // star_order_model_path. All expand every cone star unless the goal, a cap,
  // or a budget/wall limit stops the run.
  StarOrder star_order = StarOrder::kInput;
  // Goal-level logistic model (gar_star_order_v1) for StarOrder::kLearned.
  std::string star_order_model_path;
  // Initial LLM star-guide strength; zero disables the guide.
  double guide_strength = 0.0;
  // Positive weight enables proof-efficiency ordering. Goal-head stars use
  // pure efficiency; each upstream distance bucket anchors its first baseline
  // star and blends efficiency into the remainder. Zero preserves baseline.
  double proof_star_weight = 0.0;
  double proof_star_min_cost = 0.0;
  // Optional decision replay trace. Every chosen action is recorded; sampled
  // states additionally include their best unchosen ready actions.
  std::string decision_trace_path;
  std::uint64_t decision_sample_stride = 0;
  std::uint32_t decision_alternative_count = 0;
  std::string policy_trace_path;
  std::uint32_t policy_trace_negative_count = 256;
  std::uint32_t backward_max_depth = 64;
  std::uint64_t backward_max_calls = 100000;
  // Bidirectional search: after the initial bounded backward attempt fails,
  // alternate forward frontier passes and backward connect passes. Each forward
  // pass is bounded by this logical-cost budget; rounds caps the alternation.
  std::uint64_t bidirectional_forward_budget = 2000;
  std::uint32_t bidirectional_rounds = 8;
  // Learned strategy selector (path C): a goal-level logistic model predicting
  // backward success from cone size and predicate degree. When set, the
  // LearnedStrategyExecutor routes to backward only if predicted to succeed.
  std::string strategy_model_path;
};

// The collaborators an executor needs; ReasoningAgent fills it from its
// injected components.
class ValueModel;
namespace learning {
class RelevanceModel;
}  // namespace learning
struct ExecKit {
  const Scorer* scorer;
  Policy* policy;
  const CostModel* cost_model;
  const ValueModel* proof_star_model = nullptr;
  const learning::RelevanceModel* relevance_model = nullptr;
  LlmStarGuide* star_guide = nullptr;
  PaperGuide* paper_guide = nullptr;
};

struct AgentStep {
  Motif motif;
  bool success = false;
  double cost = 0.0;
  std::optional<gar::core::graph::DerivedEdge> derived;
  std::uint64_t action_id = 0;
  double handcrafted_score = 0.0;
};

struct AgentResult {
  std::vector<AgentStep> chain;
  std::vector<gar::core::graph::DerivedEdge>
      derived;                     // every fact derived, in order
  double logical_cost = 0.0;       // charged action cost used by the budget
  double execution_cost_ms = 0.0;  // measured non-overlapping execution phases
  bool goal_derived = false;
  bool budget_exhausted = false;
  std::string termination;  // "goal" | "no_candidates" | "budget" |
                            // "timeout" | "candidate_cap" | "fixpoint"

  // AdaDeduceExecutor instrumentation (only set by it): event trace
  // (select/fair_select/evaluate/suspend/revive/prune/derive, one text row
  // per event, written by the app to deduce_trace.tsv) and counters.
  // search_cost is logical work charged for every evaluated action, including
  // failed and suspended actions. proof_cost counts only distinct goal
  // ancestors. Measured phase wall times feed AgentResult::execution_cost_ms.
  struct DeduceStats {
    std::uint64_t select_events = 0;
    std::uint64_t fair_select_events = 0;
    std::uint64_t evaluate_events = 0;  // == search_cost charged
    std::uint64_t suspend_events = 0;
    std::uint64_t revive_events = 0;
    std::uint64_t prune_events = 0;
    std::uint64_t derive_events = 0;
    std::uint64_t candidate_caps = 0;  // enumerations that hit match cap
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
    // Derive() sub-phase breakdown (probe for the per-fact derivation cost).
    double derive_apply_ms = 0.0;
    double derive_ground_ms = 0.0;
    double derive_witness_ms = 0.0;
    double derive_chain_ms = 0.0;
    double derive_trace_ms = 0.0;
    // GenerateStar() sub-phase breakdown (probe for generation cost).
    double gen_match_ms = 0.0;
    double gen_add_ms = 0.0;
    double delta_wall_ms = 0.0;
    double delta_match_wall_ms = 0.0;
    double delta_complete_wall_ms = 0.0;
    double partial_complete_wall_ms = 0.0;
    double partial_grow_wall_ms = 0.0;
    double revival_wall_ms = 0.0;
    double proof_star_estimated_cost = 0.0;
    bool proof_star_active = false;
    std::vector<std::string> trace;
  };
  std::optional<DeduceStats> stats;
};

class Executor {
 public:
  virtual ~Executor() = default;
  virtual AgentResult Run(const Goal& goal, AgentState* state,
                          WitnessLog* witness, const AgentContext& ctx,
                          const ExecConfig& cfg, const ExecKit& kit) = 0;
};

// AdaDeduceExecutor: two working sets
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
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit) override;
};

class BackwardExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit) override;
};

// HybridExecutor: proof-directed backward first, forward fallback. Backward
// derives a goal with very few facts when its reverse search stays bounded, but
// fails on central predicates (call/depth limit). On failure we run forward
// AdaDeduce from the state backward left, so the goal is still completed. The
// returned result merges both phases (derived facts, chain, costs).
class HybridExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit) override;
};

// LearnedStrategyExecutor: a goal-level learned gate over the hybrid. A logistic
// model on dependency-cone size and predicate degree predicts whether backward
// search will succeed; route to backward (+ forward fallback) only when predicted
// to succeed, otherwise run forward directly. This skips futile backward attempts
// on central predicates while keeping the proof-directed gain on shallow goals.
class LearnedStrategyExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit) override;
};

// BidirectionalExecutor: bounded backward first, then alternating bounded
// forward frontier passes and backward connect passes over shared state. The
// forward passes derive the middle facts that backward's reverse search would
// otherwise recurse to find, so the two searches meet in the middle. Keeps
// backward's cheap win on shallow goals and extends proof-directed search to
// central predicates where backward alone explodes.
class BidirectionalExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit) override;
};

}  // namespace agent
}  // namespace gar::core::engine
