// Fixpoint executor internals: the ready set R and the suspended set W.
// src-local; not a public API.
//
// R holds only COMPLETE actions (full Motif — embeddings that satisfy every
// pattern edge on the current Γ). W holds two suspension kinds:
//   - eval-suspended complete actions (dynamic antecedent missing): the
//     Motif stays complete; `missing` holds every absent ground key.
//   - pattern-suspended PARTIAL actions (a deferrable pattern edge is
//     absent): the partial embedding lives here in original-vertex space
//     with UINT32_MAX at unbound vertices and NEVER leaves the executor
//     internals (never handed to Motif/Scorer/Policy/EvalAntecedent).
//
// Action identity: (rule, full embedding) via the validated MotifHash key
// mechanism, with the motifs themselves retained for equality-checked
// lookups at insertion (the hash selects a bucket; we also keep the full
// embedding so a collision would be caught by the per-rule dedup list —
// see FixpointSession::ActionIndex).
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fixpoint_matcher.h"
#include "gar/engine/agent/fact.h"
#include "gar/engine/agent/motif.h"

namespace gar {
namespace engine {
namespace agent {
namespace fixpoint {

// A suspended pattern edge's wait condition. both-bound waits key on the
// exact ground fact; open-side waits key on (slot, bound endpoint, side).
struct WaitKey {
  FactKey ground{};            // both-bound: (slot, src, dst)
  bool open_side = false;      // one endpoint unbound
  std::uint32_t slot = 0;      // open-side: the deferred edge's slot
  std::uint32_t bound_value = 0;    // the bound endpoint's entity
  bool bound_is_src = true;        // which endpoint is bound
};

struct OpenWaitKey {
  std::uint32_t slot = 0;
  std::uint32_t bound_value = 0;
  bool bound_is_src = true;
  bool operator==(const OpenWaitKey&) const = default;
};

struct OpenWaitKeyHash {
  std::size_t operator()(const OpenWaitKey& k) const {
    std::uint64_t h = (static_cast<std::uint64_t>(k.slot) << 32) |
                      k.bound_value;
    h ^= static_cast<std::uint64_t>(k.bound_is_src) +
         0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

struct ActionRecord {
  std::uint64_t id = 0;    // stable action id (insertion order)
  Motif motif;             // complete embedding only
  RuleHead head;
  std::uint64_t seq = 0;   // R enqueue sequence (fairness)
  ScoredMotif score;       // maintained by FixpointSession for exact/null
  double selection_priority = 0.0;  // score plus any policy perturbation
  std::vector<FactKey> premise_keys;  // exact-witness update subscriptions
};

struct SuspendedAction {
  std::uint64_t id = 0;
  std::uint32_t rule = 0;
  bool is_partial = false;
  // eval-suspended (is_partial=false): complete motif + all missing keys.
  Motif motif;
  std::vector<FactKey> missing;
  // pattern-suspended (is_partial=true): partial embedding + wait key.
  std::size_t defer_idx = 0;              // into RulePlan::defers
  std::vector<std::uint32_t> partial;     // UINT32_MAX = unbound (internal!)
  WaitKey wait;
  std::uint64_t wait_since_round = 0;
};

class WorkingSet {
 public:
  // R ops.
  ActionRecord& AddReady(Motif mu, RuleHead head);
  void RemoveReady(std::uint64_t id);
  ActionRecord* FindReady(std::uint64_t id);
  std::vector<ActionRecord>& ready() { return ready_; }
  bool ReadyEmpty() const { return ready_.empty(); }
  void TouchReady(std::uint64_t id);
  ActionRecord* HighestReady();
  void DiscardHighestReady();

  // W ops. Partial waits are indexed by their exact ground key or open-side
  // binding; MatchingPartials returns ids in current suspended-vector order.
  SuspendedAction& AddSuspended(SuspendedAction sw);
  void RemoveSuspended(std::uint64_t id);
  SuspendedAction* FindSuspended(std::uint64_t id);
  std::vector<std::uint64_t> MatchingPartials(
      const graph::DerivedEdge& d, std::uint32_t mirror) const;
  std::vector<SuspendedAction>& suspended() { return suspended_; }
  bool SuspendedEmpty() const { return suspended_.empty(); }

 private:
  struct ReadyPriority {
    double priority = 0.0;
    std::size_t position = 0;
    std::uint64_t id = 0;
    bool admissible = false;
  };
  void RebuildReadyHeap();

  std::vector<ActionRecord> ready_;
  std::unordered_map<std::uint64_t, std::size_t> ready_positions_;
  std::vector<ReadyPriority> ready_heap_;
  bool ready_priority_enabled_ = false;
  std::vector<SuspendedAction> suspended_;
  std::unordered_map<std::uint64_t, std::size_t> suspended_positions_;
  std::unordered_map<FactKey, std::vector<std::uint64_t>, FactKeyHash>
      ground_waiters_;
  std::unordered_map<OpenWaitKey, std::vector<std::uint64_t>, OpenWaitKeyHash>
      open_waiters_;
  std::uint64_t next_ready_id_ = 1;
  std::uint64_t next_seq_ = 0;
  std::uint64_t next_wid_ = 1;
};

}  // namespace fixpoint
}  // namespace agent
}  // namespace engine
}  // namespace gar
