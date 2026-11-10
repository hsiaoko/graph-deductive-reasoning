#pragma once

// DependencyGraph: predicate-level dependency graph of a rule set (paper
// BuildDG, Appendix H). q -> p iff some rule derives p using q (pattern
// edge, attribute/ML precondition, anything). NOT a DAG in general —
// recursive rules form cycles; d(.) via backward BFS handles them, and SCC
// condensation is exposed as a first-class analysis result.
//
// Whereas the paper builds D per goal, the graph here is built once over
// the whole rule set and d(.) is computed per goal on demand
// (DistancesTo), O(|arcs|) per call.
//
// Storage: CSR-style forward AND reverse adjacency over uint32 ids, in the
// repo's usual layout; a possible later migration into MatrixGraph is noted
// but deliberately not done (this module must stay MatrixGraph-free).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "gar/analysis/predicate_table.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

namespace gar {
namespace analysis {

inline constexpr std::uint32_t kUnreachable = UINT32_MAX;

class DependencyGraph {
 public:
  // Builds arcs q -> p for every rule deriving p (its consequent) from q
  // (pattern edge labels + antecedent predicates). Query rules (no
  // consequent) contribute no arcs. Interns all names into `table`.
  void Build(const std::vector<gar::GARule>& rules, PredicateTable* table);

  // Backward unit-cost BFS from `goal_pred` (d(goal)=0, d(q)=d(p)+1 over
  // arcs q->p); with w0 != nullptr (indexed by rule) it is Dijkstra with
  // arc weight w0(rule of the arc), the sums rounded to the nearest uint32
  // — the public contract stays a uint32 table (the completion-price lower
  // bound price(mu) = w0(phi) + d(p0) of Sections 3.4/5.4 only needs
  // integers at this layer). Unreachable predicates get kUnreachable.
  [[nodiscard]] std::vector<std::uint32_t> DistancesTo(
      std::uint32_t goal_pred, const std::vector<double>* w0 = nullptr) const;

  // Convenience single-predicate form (unit cost).
  [[nodiscard]] std::uint32_t Distance(std::uint32_t pred,
                                       std::uint32_t goal) const;

  // Tarjan SCCs over the forward adjacency. `recursive` marks size > 1 or
  // a self-loop (a rule deriving p from p) — i.e. a recursive rule family.
  struct Scc {
    std::vector<std::uint32_t> members;
    bool recursive;
  };
  [[nodiscard]] std::vector<Scc> StronglyConnectedComponents() const;

  // SCC id of `pred` when it belongs to a recursive family, -1 otherwise.
  // The Tarjan result is computed once and cached (mutable cache, logically
  // const).
  [[nodiscard]] int RecursiveFamilyOf(std::uint32_t pred) const;

  // One step of a shortest rule path toward the goal: the next predicate
  // hop and the rule witnessing the arc. BFS tree parent, so d(parent) =
  // d(pred) - 1 in the unit-cost DistancesTo table (weighted Dijkstra and
  // unit BFS agree on the path SET for unit weights).
  struct PathParent {
    std::uint32_t parent = kInvalidPredicate;  // next hop toward the goal
    std::uint32_t via_rule = UINT32_MAX;       // contributing rule index
  };
  // Unit-cost BFS parent table toward `goal_pred`; unreachable predicates
  // keep parent == kInvalidPredicate.
  [[nodiscard]] std::vector<PathParent> ShortestPathTo(
      std::uint32_t goal_pred) const;

  // Renders the shortest derivation route from `from_pred` to `goal_pred`
  // as explainable text, e.g.
  //   medals_won --[gar_000063]--> athletes
  // (rule labels come from `rule_ids` when given, else the rule index as
  // "rN"). Returns "" when no path exists; from == goal renders just the
  // predicate name. Used to hand the scorer/policy a derivation route from
  // a candidate's conclusion to the goal.
  [[nodiscard]] std::string RenderPath(
      std::uint32_t from_pred, std::uint32_t goal_pred,
      const std::vector<std::string>* rule_ids = nullptr) const;

  [[nodiscard]] std::uint32_t NumPredicates() const;
  [[nodiscard]] std::uint64_t NumArcs() const { return arcs_.size(); }
  [[nodiscard]] std::uint32_t InDegree(std::uint32_t p) const {
    return in_off_[p + 1] - in_off_[p];
  }
  [[nodiscard]] std::uint32_t OutDegree(std::uint32_t q) const {
    return out_off_[q + 1] - out_off_[q];
  }
  [[nodiscard]] const PredicateTable& table() const { return *table_; }

  // Forward derivation depths (the B* lower-bound read of the graph,
  // gar_analyze gen-goals): target(p) = fewest rule applications that can
  // derive a NEW fact of predicate p, counted predicate-level (data-blind
  // beyond the anchor set): 1 + min over rules deriving p of max over
  // premise costs, where a premise costs 0 when base-anchored (a
  // data-readable precondition: attribute/ML, or a relation with base
  // instances) and its own target depth otherwise. An anchored predicate
  // that is also derivable still gets a finite target from its rules.
  // Cycles that never bottom out at an anchor stay kUnreachable. This is a
  // LOWER bound on the instance-level chain length (per premise it counts
  // the cheapest derivation of the predicate, not of the specific
  // instance); the chase's first-derivation round is the empirical
  // counterpart (see gar_analyze gen-goals' output header). Dijkstra over
  // the rule hypergraph, lazy PQ.
  [[nodiscard]] std::vector<std::uint32_t> DerivationDepths(
      const std::vector<char>& base_anchored) const;

  // Serializes the graph as Graphviz dot (SCCs as clusters; when
  // `distances` is given, node labels carry " (d=k)") and as YAML
  // (nodes / arcs / per-SCC annotation).
  void WriteDot(const std::filesystem::path& out,
                const std::vector<std::uint32_t>* distances = nullptr) const;
  void WriteYaml(const std::filesystem::path& out) const;

 private:
  struct Arc {
    std::uint32_t src;   // q: predicate used
    std::uint32_t dst;   // p: predicate derived
    std::uint32_t rule;  // contributing rule index
  };

  PredicateTable* table_ = nullptr;
  std::vector<Arc> arcs_;
  // Forward adjacency: out_off_[q]..out_off_[q+1] indexes arcs sorted by
  // (src, dst); reverse adjacency: in_*/rin_* indexes arc ids sorted by
  // (dst, src).
  std::vector<std::uint32_t> out_off_;
  std::vector<std::uint32_t> out_arcs_;  // arc ids
  std::vector<std::uint32_t> in_off_;
  std::vector<std::uint32_t> in_arcs_;   // arc ids

  // Tarjan cache (filled lazily by EnsureSccCache; logically const).
  void EnsureSccCache() const;
  mutable std::optional<std::vector<Scc>> scc_cache_;
  mutable std::vector<int> recursive_family_of_;  // pred -> scc id, -1 = none
};

// Anchor flags for DerivationDepths: a predicate is base-anchored when its
// preconditions read data directly — attribute/ML predicates (no relation
// by that name in the graph) always anchor; a relation anchors iff it has
// at least one base edge in `g`.
[[nodiscard]] std::vector<char> BaseAnchoredPredicates(
    const DependencyGraph& dg, const graph::TripleGraph& g);

// Returns the indices of rules whose consequent predicate can reach
// `goal_predicate` in the dependency graph — the goal's dependency cone.
// Pattern queries (no consequent) are always dropped. Builds the dependency
// graph internally; `goal_predicate` must be a predicate name as interned
// by PredicateIdOf / LabelNameOf (i.e., a relation name). An unknown goal
// predicate (no rule derives it) yields an empty cone.
[[nodiscard]] std::vector<std::size_t> GoalConeRuleIndices(
    const std::vector<gar::GARule>& rules, const std::string& goal_predicate);

}  // namespace analysis
}  // namespace gar
