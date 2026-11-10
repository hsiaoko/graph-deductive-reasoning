#pragma once

// AgentState: the agent's mutable state S = (G, Sigma, Gamma). Wraps the
// TripleGraph (derived edges are merged in, facts only ever accumulate),
// the vertex attributes (read-only) and the rule set. `version()` counts
// the growth of Gamma and drives failure re-admission.

#include <cstdint>
#include <vector>

#include "gar/graph/fact.h"
#include "gar/graph/triple_graph.h"
#include "gar/graph/vertex_attrs.h"
#include "gar/rule.h"

namespace gar::core::engine {
namespace agent {

class AgentState {
 public:
  AgentState(gar::core::graph::TripleGraph& g, const gar::core::graph::VertexAttrs* attrs,
             const std::vector<GARule>& rules)
      : g_(g), attrs_(attrs), rules_(rules),
        per_slot_derived_(g.num_slots(), 0) {}

  [[nodiscard]] gar::core::graph::TripleGraph& graph() { return g_; }
  [[nodiscard]] const gar::core::graph::TripleGraph& graph() const { return g_; }
  [[nodiscard]] const gar::core::graph::VertexAttrs* attrs() const { return attrs_; }
  [[nodiscard]] const std::vector<GARule>& rules() const { return rules_; }

  // Gamma growth counter (number of facts derived so far).
  [[nodiscard]] std::uint64_t version() const { return version_; }
  [[nodiscard]] std::uint64_t PerSlotDerivedCount(std::uint32_t slot) const {
    return per_slot_derived_[slot];
  }
  // Edge membership in the CURRENT (augmented) graph — base slots and
  // inverse slots alike, since derived edges are mirrored on insertion.
  [[nodiscard]] bool ContainsEdge(std::uint32_t slot, std::uint32_t src,
                                  std::uint32_t dst) const {
    return g_.RelationPairSet(slot).count(gar::core::graph::PackPair(src, dst)) != 0;
  }

  // Merges a freshly derived edge into the graph (base + inverse mirror)
  // and journals it. Caller contract: the edge is new (ContainsEdge false);
  // a duplicate is silently dropped without a version bump.
  void ApplyDerivation(const gar::core::graph::DerivedEdge& d) {
    if (g_.AddEdges(d.slot, {{d.src, d.dst}}) == 0) return;
    journal_.push_back(d);
    ++per_slot_derived_[d.slot];
    ++version_;
  }

  [[nodiscard]] const std::vector<gar::core::graph::DerivedEdge>& journal() const {
    return journal_;
  }

 private:
  gar::core::graph::TripleGraph& g_;
  const gar::core::graph::VertexAttrs* attrs_;
  const std::vector<GARule>& rules_;
  std::uint64_t version_ = 0;
  std::vector<std::uint64_t> per_slot_derived_;
  std::vector<gar::core::graph::DerivedEdge> journal_;
};

}  // namespace agent
}  // namespace gar::core::engine
