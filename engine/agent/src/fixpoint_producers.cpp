#include "fixpoint_producers.h"

#include "gar/analysis/predicate_table.h"

namespace gar {
namespace engine {
namespace agent {
namespace fixpoint {

namespace {

// Predicate-table id of a slot's relation name (same naming the DG uses).
std::uint32_t PredOfSlot(std::uint32_t slot, const GARule& rule,
                         const analysis::PredicateTable& table) {
  return table.Find(analysis::LabelNameOf(slot, rule));
}

}  // namespace

ProducerTracker::ProducerTracker(const std::vector<RulePlan>& plans,
                                 const std::vector<GARule>& rules,
                                 const analysis::PredicateTable* table)
    : plans_(plans),
      rules_(rules),
      table_(table),
      generated_(plans.size(), 0),
      pending_(plans.size(), 0) {
  for (std::uint32_t i = 0; i < plans.size(); ++i) {
    const GARule& r = rules_[plans_[i].rule];
    if (!r.consequent) continue;
    const std::uint32_t p =
        table_->Find(analysis::PredicateIdOf(*r.consequent, r));
    if (p != analysis::kInvalidPredicate) producers_[p].push_back(i);
  }
}

void ProducerTracker::MarkGenerated(std::uint32_t plan_idx) {
  generated_[plan_idx] = 1;
  ++book_gen_;
}

void ProducerTracker::SetPending(std::uint32_t plan_idx, bool has_pending) {
  if (pending_[plan_idx] != has_pending) {
    pending_[plan_idx] = has_pending;
    ++book_gen_;
  }
}

std::uint32_t ProducerTracker::PlanOfRule(std::uint32_t rule_idx) const {
  for (std::uint32_t i = 0; i < plans_.size(); ++i) {
    if (plans_[i].rule == rule_idx) return i;
  }
  return UINT32_MAX;
}

void ProducerTracker::Recompute(std::uint64_t version) {
  if (computed_version_ == version && computed_book_gen_ == book_gen_) return;
  computed_version_ = version;
  computed_book_gen_ = book_gen_;
  // Dead-set least fixpoint: a rule is dead when generated, no pending
  // actions, and every dynamic slot it references is dead; a predicate is
  // dead when all its producers are dead. Iterate until stable (the dead
  // sets only grow).
  const std::uint32_t np = plans_.size();
  rule_alive_.assign(np, 1);
  pred_alive_.clear();
  // Seed: predicates with at least one cone producer start alive.
  for (const auto& [p, prods] : producers_) pred_alive_[p] = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::uint32_t i = 0; i < np; ++i) {
      if (!rule_alive_[i]) continue;
      if (!generated_[i] || pending_[i]) continue;  // still alive
      // rule dies only if every dynamic slot it could delta-match on is dead.
      bool any_alive_slot = false;
      const RulePlan& pl = plans_[i];
      const GARule& r = rules_[pl.rule];
      for (const auto& e : pl.spec.edges) {
        const std::uint32_t p = PredOfSlot(e[2], r, *table_);
        auto it = pred_alive_.find(p);
        if (it != pred_alive_.end() && it->second) {
          any_alive_slot = true;
          break;
        }
      }
      for (const auto& pred : r.antecedent) {
        if (pred.kind != gar::PredKind::kEdge) continue;
        const std::uint32_t p =
            table_->Find(analysis::PredicateIdOf(pred, r));
        auto it = pred_alive_.find(p);
        if (it != pred_alive_.end() && it->second) {
          any_alive_slot = true;
          break;
        }
      }
      if (!any_alive_slot) {
        rule_alive_[i] = 0;
        changed = true;
        // this rule may head a predicate others wait on: recompute preds.
        for (const auto& [p, prods] : producers_) {
          if (!pred_alive_[p]) continue;
          bool any = false;
          for (std::uint32_t pi : prods) {
            if (rule_alive_[pi]) {
              any = true;
              break;
            }
          }
          if (!any) pred_alive_[p] = 0;
        }
      }
    }
  }
}

bool ProducerTracker::Alive(std::uint32_t pred, std::uint64_t version) {
  Recompute(version);
  const auto it = pred_alive_.find(pred);
  // A predicate with no cone producer is dead for our purposes (nothing in
  // the cone will ever derive it).
  return it != pred_alive_.end() && it->second != 0;
}

}  // namespace fixpoint
}  // namespace agent
}  // namespace engine
}  // namespace gar
