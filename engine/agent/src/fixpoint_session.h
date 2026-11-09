// FixpointSession: one FixpointExecutor::Run's working state. src-local;
// not a public API — the public executor only constructs and runs it.
// No internal type (RulePlan/partial embeddings/WorkingSet) ever leaves
// the engine/agent translation units.
//
// The session owns rule plans, R/W, exact-score subscriptions, indexed delta
// maintenance, and the two execution loops. Timing fields are observational;
// recursive-goal pin suppression is part of the logical completeness contract.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fixpoint_matcher.h"
#include "fixpoint_preconditions.h"
#include "fixpoint_producers.h"
#include "fixpoint_working_set.h"
#include "gar/engine/agent/executor.h"
#include "gar/graph/pattern_matcher.h"

namespace gar {
namespace engine {
namespace agent {

class FixpointSession {
 public:
  FixpointSession(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit)
      : goal_(goal),
        state_(state),
        witness_(witness),
        ctx_(ctx),
        cfg_(cfg),
        kit_(kit),
        g_(state->graph()),
        matcher_(g_),
        sctx_{&ctx.distances, ctx.table, witness, state} {}

  AgentResult Run();

 private:
  // Collision-safe action identity: hash bucket -> full embeddings, compared
  // elementwise (the hash only narrows the search; two different embeddings
  // never alias).
  class ActionIndex {
   public:
    bool Insert(std::uint32_t rule, const std::vector<std::uint32_t>& emb);
    bool Insert(std::uint32_t rule, std::vector<std::uint32_t>&& emb);

   private:
    std::unordered_map<std::uint64_t,
                       std::vector<std::pair<std::uint32_t,
                                             std::vector<std::uint32_t>>>>
        m_;
  };

  void BuildPlans();
  std::unordered_map<std::uint32_t, std::uint32_t> GoalPins(
      const fixpoint::RulePlan& pl) const;
  std::unordered_map<std::uint32_t, std::uint32_t> GoalSubPins(
      const fixpoint::RulePlan& pl, const fixpoint::DeferrableEdge& de) const;

  bool IncrementalScores() const;
  fixpoint::ActionRecord& EnqueueReady(Motif mu, RuleHead head);
  void ScoreReady(fixpoint::ActionRecord* rec);
  std::array<double, 11> ActionFeatures(
      const fixpoint::ActionRecord& rec) const;
  void WriteQTrace() const;
  void UntrackReady(const fixpoint::ActionRecord& rec);
  void RecordWitness(const Fact& fact);

  void AddComplete(std::uint32_t pi, const std::vector<std::uint32_t>& emb);
  void AddPartials(std::uint32_t pi, const fixpoint::DeferrableEdge& de,
                   const std::unordered_map<std::uint32_t, std::uint32_t>&
                       sub_pins);
  void GenerateStar(std::uint32_t pi);
  void VerifyGeneration();

  bool WallExceeded() const;
  bool BudgetHit() const;

  void OnDerivation(const graph::DerivedEdge& d);
  void CompleteDeltas(const graph::DerivedEdge& d, std::uint32_t mirror);
  void CompletePartials(const graph::DerivedEdge& d, std::uint32_t mirror);
  void GrowPartials(const graph::DerivedEdge& d, std::uint32_t mirror);

  // 2 = all present (derive), 1 = dynamic missing (suspended),
  // 0 = static false (prune), 3 = static false answered from H.
  int Evaluate(const fixpoint::ActionRecord& rec);
  graph::DerivedEdge Derive(const fixpoint::ActionRecord& rec);
  bool RevivalPass();  // true iff W made progress (revive or prune)
  fixpoint::ActionRecord* SelectReady();

  void Trace(std::string ev);
  void RefreshPeaks();

  const Goal& goal_;
  AgentState* state_;
  WitnessLog* witness_;
  const AgentContext& ctx_;
  const ExecConfig& cfg_;
  const ExecKit& kit_;
  const graph::TripleGraph& g_;
  const graph::PatternMatcher matcher_;
  const ScoringContext sctx_;

  AgentResult res_;
  std::vector<fixpoint::RulePlan> plans_;
  std::optional<fixpoint::ProducerTracker> prod_;
  fixpoint::WorkingSet ws_;
  fixpoint::PreconditionCache H_;
  ActionIndex seen_actions_;
  std::vector<ActionIndex> seen_partials_;
  std::unordered_map<FactKey, std::unordered_set<std::uint64_t>, FactKeyHash>
      premise_actions_;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      complete_slot_users_;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      partial_slot_users_;

  struct QTraceRow {
    std::array<double, 11> features;
    std::uint64_t action_id = 0;
    std::uint32_t rule = 0;
  };
  std::vector<QTraceRow> q_trace_rows_;

  std::chrono::steady_clock::time_point t0_;
  std::uint32_t next_star_ = 0;  // first unexpanded star (star discipline)
  std::uint64_t selects_since_fair_ = 0;
  std::uint64_t round_no_ = 0;
  bool verify_fx_ = false;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
