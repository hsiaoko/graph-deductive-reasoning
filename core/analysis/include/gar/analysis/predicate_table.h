#pragma once

// PredicateTable: interns predicate names to dense uint32 ids — the single
// source of predicate identity shared by rules, facts, the dependency graph
// and the scorers. Built once per rule set.
//
// Predicate identity (what counts as "the same predicate" across rules):
//   * edge predicate / rule consequent / pattern edge  ->  the relation
//     label NAME (e.g. "owns"); pattern edges carry only a LabelID, resolved
//     through the rule's `labels` dictionary by reverse lookup. Inverse
//     slots ("r^-1") are normalized to the base relation name: r^-1(x,y) is
//     the same relation r read backwards, and dependency/trackability is a
//     property of the relation, not of the walking direction (consequents
//     always name base relations, so without normalization every inverse
//     body edge would be an underivable source node with d = inf).
//   * const (x.A<c) / var (x.A<y.B) / exists (x.A)  ->  the attribute name
//     A; different thresholds/operators on the same attribute share one
//     node (paper Fig. 4: "amount>theta" is a single node). A var
//     predicate's rhs attribute folds into the same identity rule — the
//     node is the lhs attribute name (attributes are data, never derived,
//     so attribute nodes are always sources either way).
//   * ml (M(x,y,l))  ->  the model name (e.g. "M_anom").
// No other PredKind exists in gar::Predicate; should one be added, it gets
// a node per its own naming spirit (see PredicateIdOf).

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/rule.h"

namespace gar::core::analysis {

inline constexpr std::uint32_t kInvalidPredicate = UINT32_MAX;

class PredicateTable {
 public:
  // Idempotent: interning an existing name returns its id.
  std::uint32_t Intern(const std::string& name);
  [[nodiscard]] std::uint32_t Find(const std::string& name) const;
  [[nodiscard]] const std::string& Name(std::uint32_t id) const;
  [[nodiscard]] std::size_t size() const { return names_.size(); }

 private:
  std::unordered_map<std::string, std::uint32_t> ids_;
  std::vector<std::string> names_;
};

// Identity of an edge label as a predicate name: reverse lookup in the
// rule's `labels` dictionary (when several names map to the same id — the
// dictionary mixes node-label and edge-label id spaces — the
// lexicographically smallest name wins, keeping the choice deterministic),
// with a trailing "^-1" stripped (base-relation normalization). Falls back
// to "label:<id>" when the id is unresolved.
std::string LabelNameOf(gar::LabelID label, const gar::GARule& rule);

// Identity of a predicate per the rules documented above.
std::string PredicateIdOf(const gar::Predicate& pred, const gar::GARule& rule);

}  // namespace gar::core::analysis
