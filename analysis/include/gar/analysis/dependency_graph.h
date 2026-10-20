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
};

// Anchor flags for DerivationDepths: a predicate is base-anchored when its
// preconditions read data directly — attribute/ML predicates (no relation
// by that name in the graph) always anchor; a relation anchors iff it has
// at least one base edge in `g`.
[[nodiscard]] std::vector<char> BaseAnchoredPredicates(
    const DependencyGraph& dg, const graph::TripleGraph& g);

}  // namespace analysis
}  // namespace gar
