#pragma once

// GoalSampler: candidate goal facts sampled from the data graph. A goal is
// a ground edge fact (slot, src, dst) whose slot is derivable (some rule's
// consequent names its base relation). This is pure sampling only —
// reachability validation (is the goal actually derivable under a budget?)
// and B* calibration happen at the app level, where a DeductionEngine is
// available (L4 composition); this L2 module deliberately does not chase.

#include <cstdint>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/graph/triple_graph.h"

namespace gar::core::analysis {

struct GoalCandidate {
  std::uint32_t slot;  // base relation slot (see DerivableSlots note)
  std::uint32_t src;
  std::uint32_t dst;
};

class GoalSampler {
 public:
  GoalSampler(const gar::core::graph::TripleGraph& g, const DependencyGraph& dg)
      : g_(g), dg_(dg) {}

  // Slots whose base relation is derivable (named by some rule's
  // consequent, i.e. a dependency-graph node with in-degree > 0). Only
  // BASE slots are returned: inverse slots are not normalized into their
  // base slot because a goal fact is stated on a relation in its stored
  // orientation — TripleGraph names inverses "r^-1" with slot ids >=
  // num_relations, and rule consequents always name base relations, so the
  // derivable set lives entirely below num_relations.
  [[nodiscard]] std::vector<std::uint32_t> DerivableSlots() const;

  // Uniformly samples at most `per_slot` edges present in the graph per
  // derivable slot (all of them when the slot has fewer). A fixed `seed`
  // makes the result reproducible (mt19937_64, slots visited in id order).
  [[nodiscard]] std::vector<GoalCandidate> Sample(std::uint32_t per_slot,
                                                  std::uint64_t seed) const;

 private:
  const gar::core::graph::TripleGraph& g_;
  const DependencyGraph& dg_;
};

}  // namespace gar::core::analysis
