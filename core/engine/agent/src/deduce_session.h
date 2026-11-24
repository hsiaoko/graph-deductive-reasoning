// DeduceSession: one AdaDeduceExecutor::Run's working state. src-local;
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
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gar/core/agent/executor.h"
#include "gar/core/agent/fact_encoder.h"
#include "gar/core/agent/learning/policy_trace.h"
#include "match_backend.h"
#include "partial_matcher.h"
#include "precondition_cache.h"
#include "producer_tracker.h"
#include "working_set.h"

namespace gar::core::engine {
namespace agent {

class DeduceSession {
 public:
  DeduceSession(const Goal& goal, AgentState* state, WitnessLog* witness,
                const AgentContext& ctx, const ExecConfig& cfg,
                const ExecKit& kit)
      : goal_(goal),
        state_(state),
        witness_(witness),
        ctx_(ctx),
        cfg_(cfg),
        kit_(kit),
        g_(state->graph()),
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
    std::unordered_map<
        std::uint64_t,
        std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>>>
        m_;
  };

  void BuildPlans();
  std::unordered_map<std::uint32_t, std::uint32_t> GoalPins(
      const deduce::RulePlan& pl) const;
  std::unordered_map<std::uint32_t, std::uint32_t> GoalSubPins(
      const deduce::RulePlan& pl, const deduce::DeferrableEdge& de) const;

  bool IncrementalScores() const;
  deduce::ActionRecord& EnqueueReady(Motif mu, RuleHead head);
  void ScoreReady(deduce::ActionRecord* rec);
  std::array<double, 11> ActionFeatures(const deduce::ActionRecord& rec) const;
  void UntrackReady(const deduce::ActionRecord& rec);
  void RecordWitness(const Fact& fact);

  void AddComplete(std::uint32_t pi, const std::vector<std::uint32_t>& emb);
  void AddPartialMatches(std::uint32_t pi, const deduce::DeferrableEdge& de,
                         const std::vector<std::uint32_t>& embeddings);
  void GenerateStar(std::uint32_t pi);
  void VerifyGeneration();

  bool WallExceeded() const;
  bool BudgetHit() const;

  struct DeltaMatchJob {
    std::uint32_t pi = 0;
    std::size_t defer = 0;
    bool partial = false;
  };

  void OnDerivations(const std::vector<gar::core::graph::DerivedEdge>& derived);
  void CompleteDeltas(const std::vector<gar::core::graph::DerivedEdge>& derived,
                      std::vector<MatchRequest>* requests,
                      std::vector<DeltaMatchJob>* jobs);
  void CompletePartials(const gar::core::graph::DerivedEdge& d,
                        std::uint32_t mirror);
  void GrowPartials(const std::vector<gar::core::graph::DerivedEdge>& derived,
                    std::vector<MatchRequest>* requests,
                    std::vector<DeltaMatchJob>* jobs);
  void FlushDerivations();

  // 2 = all present (derive), 1 = dynamic missing (suspended),
  // 0 = static false (prune), 3 = static false answered from H.
  int Evaluate(const deduce::ActionRecord& rec);
  gar::core::graph::DerivedEdge Derive(const deduce::ActionRecord& rec);
  double GoalProofCost() const;
  double MeasuredCostMs() const;
  bool RevivalPass();  // true iff W made progress (revive or prune)
  deduce::ActionRecord* SelectReady();
  std::vector<deduce::ActionRecord> SelectReadyBatch();
  void PrepareStaticBatch(const std::vector<deduce::ActionRecord>& batch);
  bool ProcessAction(const deduce::ActionRecord& rec);

  void Trace(std::string ev);
  void RefreshPeaks();
  void WriteDecisionTrace(const deduce::ActionRecord& chosen);
  void WritePolicyTrace();

  const Goal& goal_;
  AgentState* state_;
  WitnessLog* witness_;
  const AgentContext& ctx_;
  const ExecConfig& cfg_;
  const ExecKit& kit_;
  const gar::core::graph::Graph& g_;
  std::unique_ptr<MatchBackend> match_backend_;
  const ScoringContext sctx_;

  AgentResult res_;
  std::vector<deduce::RulePlan> plans_;
  std::optional<deduce::ProducerTracker> prod_;
  deduce::WorkingSet ws_;
  deduce::PreconditionCache H_;
  ActionIndex seen_actions_;
  std::vector<ActionIndex> seen_partials_;
  std::unordered_map<FactKey, std::unordered_set<std::uint64_t>, FactKeyHash>
      premise_actions_;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      complete_slot_users_;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      partial_slot_users_;

  std::vector<gar::core::graph::DerivedEdge> pending_derivations_;

  std::chrono::steady_clock::time_point t0_;
  std::uint32_t next_star_ = 0;  // first unexpanded star (star discipline)
  std::uint64_t selects_since_fair_ = 0;
  std::uint64_t round_no_ = 0;
  std::ofstream decision_trace_;
  std::uint64_t decision_index_ = 0;
  std::optional<learning::PolicyTrace> policy_trace_;
  bool verify_ = false;
};

}  // namespace agent
}  // namespace gar::core::engine
