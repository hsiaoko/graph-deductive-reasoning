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
  std::uint32_t top_k = 0;  // bnb: 0 = keep all candidates (exact mode,
                            // Theorem 5 certificate); k > 0 truncates
  // Subgoal-utility decay (§3.2): a derived fact on predicate q earns
  // w_q = utility_gamma^{d(q)}. Mirrors the scorer's gamma; the app wires
  // HandcraftedConfig::gamma through.
  double utility_gamma = 0.5;
  // BackwardExecutor: bound on subgoal recursion depth (a goal needing a
  // round-k chase derivation nests at most ~k deep).
  std::uint32_t max_subgoal_depth = 4;
  // Degenerate single-path arm (--direction backward --executor greedy):
  // only the top-ranked candidate per target, no open-edge fallback.
  bool no_backtrack = false;
  // BackwardBnBExecutor lower bound: false = predicate derivation depth
  // (default, bit-identical Phase-2 behaviour); true = instance-level
  // (--backward-bnb-lb instance).
  bool backward_bnb_instance_lb = false;
  // BackwardBnBExecutor candidate enumeration: false = eager (default,
  // bit-identical); true = lazy rule-order enumeration with an
  // incumbent-triggered stop (--backward-bnb-enum lazy; certificate-exact:
  // rules are visited in rule_est ascending order and enumeration stops
  // when the next rule's est exceeds the incumbent window).
  bool backward_bnb_lazy_enum = false;
  // Max distinct facts collected per open-sided (variable) target in
  // BackwardExecutor; 8 = legacy default (bit-identical). Raise via
  // --open-collect-cap for full-answer workloads.
  std::uint32_t open_collect_cap = 8;
  // BackwardExecutor candidate ordering (--backward-order): "score" =
  // scorer-ranked, deterministic tie-breaks (default, bit-identical);
  // "random" = seeded full shuffle (scorer ignored for ordering);
  // "hybrid" = seeded shuffle inside the top-8 score window, score order
  // beyond it. The seed travels here because ExecConfig is what the
  // executor sees.
  std::string backward_order = "score";
  std::uint64_t seed = 42;
  // Full-extension mode for open-sided goals (--backward-full-enum): loop
  // pass1/pass2 scans at the top target until the journal stops growing
  // (fixpoint) or the budget is exhausted, instead of returning after the
  // first productive scan. Pass-1 candidates whose conclusion is already in
  // the graph are skipped (they would burn budget on re-derivations and
  // starve the open-edge fallback). Default off (bit-identical).
  bool backward_full_enum = false;
  // GreedyExecutor analogue (--forward-full-enum): for open-sided goals,
  // do not stop at the first goal fact; keep stepping until the candidate
  // pool is exhausted (fixpoint), the budget, or the wall limit. Default
  // off (bit-identical).
  bool forward_full_enum = false;
  // Wall-clock limit in seconds (0 = off). Checked inside the executor
  // loops so a wall-limited run still terminates cleanly (termination
  // "timeout") and writes its partial chain/journal — an external kill
  // would lose them.
  double wall_limit_sec = 0.0;
  // FixpointExecutor: every `fair_interval` policy selections, force the
  // longest-waiting ready action (0 = off, pure policy order). The fair
  // picks are marked in the event trace.
  std::uint32_t fair_interval = 0;
  // "input" preserves rule-path order; "goal" stable-sorts stars by
  // consequent-to-goal DG distance. Both expand every cone star unless the
  // goal, a cap, or a budget/wall limit stops the run.
  std::string fixpoint_star_order = "input";
  // Optional per-evaluated-action trace for offline cost-to-go training.
  // Empty disables all tracing overhead; labels are finite only on goal hits.
  std::string fixpoint_q_trace_path;
};

// The collaborators an executor needs; ReasoningAgent fills it from its
// injected components.
class ValueModel;
struct ExecKit {
  const Scorer* scorer;
  Policy* policy;
  const CostModel* cost_model;
  // Optional value model for BNB branch or Fixpoint action ordering.
  const ValueModel* value_model = nullptr;
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
  double utility = 0.0;       // sum of w_q over derived facts (§3.2)
  bool goal_derived = false;
  bool budget_exhausted = false;
  std::string termination;  // "goal" | "no_candidates" | "budget" |
                            // "goal_already_in_graph" | "anytime_fallback" |
                            // "timeout" | "full_enum_fixpoint"
  // Branch-and-bound certificates (greedy leaves all of these false):
  bool certified_optimal = false;    // top_k==0 and search exhausted (Thm 5)
  bool certified_no_chain = false;   // top_k==0, no chain fits the budget
  bool anytime_fallback = false;     // returned chain is the line-14 fallback
  std::uint64_t expansions = 0;      // branches actually expanded (bnb)
  std::uint64_t first_goal_expansions = 0;  // 0 until the first goal
  double first_goal_wall_ms = -1.0;         // executor-local; -1 if none

  // Incremental-greedy instrumentation (only set by GreedyExecutor):
  // enumerations, raw embeddings generated, candidates served from cache,
  // capped enumerations, and the match/apply/score wall-clock split.
  struct ForwardStats {
    std::uint64_t rule_selections = 0;
    std::uint64_t rules_initialized = 0;
    std::uint64_t delta_enums = 0;
    std::uint64_t embeddings_generated = 0;
    std::uint64_t candidates_reused = 0;
    std::uint64_t capped_enums = 0;
    double match_wall_ms = 0.0;
    double apply_wall_ms = 0.0;
    double score_wall_ms = 0.0;
  };
  std::optional<ForwardStats> fwd_stats;

  // FixpointExecutor instrumentation (only set by it): event trace
  // (select/fair_select/evaluate/suspend/revive/prune/derive, one text row
  // per event, written by the app to fixpoint_trace.tsv) and counters.
  // Cost separation (the backward cost-semantics lesson): proof_cost =
  // cost of the successful derivation chain; search_cost = every charged
  // evaluation (incl. failed/suspended); selection/evaluation event counts
  // are separate counters; wall is wall.
  struct FixpointStats {
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
  std::optional<FixpointStats> fx_stats;
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

// FixpointExecutor (docs/fixpoint_implementation_plan.md): two working sets
// and two loops. The MAIN loop generates actions for goal-cone rules
// (complete matches on G∪Γ plus partial matches that defer one dynamic
// pattern edge), schedules only R via the injected Policy, and evaluates
// every precondition (collecting all misses): full pass derives, static
// failure prunes (shared via the precondition cache), dynamic absence
// suspends into W. The FIXPOINT loop revives W entries whose missing facts
// arrived (or whose partial wait completed on a delta) and prunes dead
// waits (no live producer). policy never schedules W.
class FixpointExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

// BranchBoundExecutor: the HybNS best-first branch-and-bound (paper
// Figure 6) — see branch_bound_executor.h for the branch-state design.
class BranchBoundExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

// BackwardExecutor: goal-directed backward chaining. Candidates are
// generated by PINNED enumeration (PatternMatcher with the rule's head
// bound to the goal constants) — the candidate space collapses to motifs
// that can derive the goal, instead of forward enumeration truncated by a
// per-rule cap (the hitfix root cause: the goal embedding sat at position
// hundreds-to-thousands in enumeration order while cap 8 kept only the
// first ones). Missing derivable premises become recursive subgoals
// (bounded by ExecConfig::max_subgoal_depth). The policy is not consulted
// for ordering — backward chaining is itself the guidance; the scorer
// (rel_fam / rel_evd / confidence) still ranks the pinned candidates.
// BackwardBnBExecutor (Phase 2, --direction backward --executor bnb):
// DFBnB optimal-chain search over the backward AND-OR structure; see
// backward_bnb_executor.cpp's header comment for the certificate scope
// (seen candidate space, DAG cost, grounded-subgoal coverage) and the
// admissible-bound argument.
class BackwardBnBExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

class BackwardExecutor final : public Executor {
 public:
  AgentResult Run(const Goal& goal, AgentState* state, WitnessLog* witness,
                  FailureRegistry* failures, const AgentContext& ctx,
                  const ExecConfig& cfg, const ExecKit& kit) override;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
