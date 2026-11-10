// AdaDeduce executor internals: producer aliveness for safe pruning of
// suspended actions. src-local; not a public API.
//
// A W entry waiting on fact(s) of predicate p may only be pruned when p has
// NO live producer. A producer rule is alive while it can still generate
// new actions: not yet generated, or holding pending actions in R/W, or
// referencing a derivable slot that is itself still alive (new facts there
// create new matches). Aliveness is therefore a least fixpoint over the
// goal cone and is recomputed lazily per Γ version.
//
// Pruning is CONSERVATIVE by construction: an empty current R never kills
// producers that are ungenerated or hold W entries.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "partial_matcher.h"
#include "gar/analysis/dependency_graph.h"
#include "gar/rule.h"

namespace gar::core::engine {
namespace agent {
namespace deduce {

class ProducerTracker {
 public:
  // plans: the goal-cone rule plans (index i refers to plans[i].rule =
  // global rule idx). rules: the full rule set (for consequent reads).
  ProducerTracker(const std::vector<RulePlan>& plans,
                  const std::vector<GARule>& rules,
                  const gar::core::analysis::PredicateTable* table);

  // Rule bookkeeping from the executor.
  void MarkGenerated(std::uint32_t plan_idx);
  void SetPending(std::uint32_t plan_idx, bool has_pending);

  // True while predicate `pred` (table id) may still gain facts from the
  // cone. Recomputed when `version` (Γ version) changes.
  bool Alive(std::uint32_t pred, std::uint64_t version);

  // Plan index of a global rule index (UINT32_MAX when not in the cone).
  std::uint32_t PlanOfRule(std::uint32_t rule_idx) const;

 private:
  void Recompute(std::uint64_t version);
  // pending_/generated_ change WITHIN one Γ version (stars expand and drain
  // between derivations): every bookkeeping mutation bumps this generation,
  // and the aliveness fixpoint is recomputed whenever EITHER the Γ version
  // or the bookkeeping generation moved — never answered from a stale cache
  // (a stale "dead" would wrongly prune a wait whose producer just expanded).
  std::uint64_t book_gen_ = 0;
  std::uint64_t computed_book_gen_ = UINT64_MAX;

  const std::vector<RulePlan>& plans_;
  const std::vector<GARule>& rules_;
  const gar::core::analysis::PredicateTable* table_;
  // base predicate id -> producer plan indices.
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> producers_;
  std::vector<char> generated_;
  std::vector<char> pending_;
  // per-version fixpoint caches.
  std::uint64_t computed_version_ = UINT64_MAX;
  std::vector<char> rule_alive_;
  std::unordered_map<std::uint32_t, char> pred_alive_;
};

}  // namespace deduce
}  // namespace agent
}  // namespace gar::core::engine
