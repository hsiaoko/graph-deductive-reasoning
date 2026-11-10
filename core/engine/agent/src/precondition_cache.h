// AdaDeduce executor internals: per-predicate precondition status and the
// precondition cache H (paper plan §4.1). src-local; not a public API.
//
// Cache keys are STRUCTURED (the earlier FactKey-based cache collapsed
// distinct logical predicates — x.A>=18 and x.A<18 shared one key and
// cross-pruned): GroundPreconditionKey keeps the full logical Predicate
// (Predicate::operator== covers kind/op/value/rhs_attr/label/...) plus the
// grounded binding, with full equality in the map — no collisions, no
// cross-pruning. Dynamic edge facts additionally keep the normalized
// FactKey for Γ/waiting lookups (public Fact untouched).
//
// Status semantics:
//   kStaticTrue / kStaticFalse  — attr/ML/edge-on-derive-nothing predicates
//                                 read G only; G never changes, so the value
//                                 is permanent (cache forever).
//   kDynamicPresent             — derivable edge currently in Γ; facts only
//                                 accumulate, so a positive stays true
//                                 (cache forever).
//   kDynamicMissing             — derivable edge currently absent; MUST NOT
//                                 be cached permanently: entries carry the
//                                 Γ version and miss after any growth.
//
// Evaluation collects the status of EVERY precondition (no short-circuit),
// so a suspension records all missing facts, not just the first failure.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "apply_helpers.h"  // IsDynamicPredicate
#include "gar/engine/agent/fact.h"
#include "gar/engine/agent/goal.h"
#include "gar/graph/predicate_evaluator.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

namespace gar::core::engine {
namespace agent {
namespace deduce {

enum class PrecondState { kStaticTrue, kStaticFalse, kDynamicPresent,
                          kDynamicMissing };

// Structured ground-precondition key: full logical predicate + grounded
// binding ((subject, object) in this order: node/src, rhs_node/dst; ML
// keeps both ml_vars). Equality is total (Predicate::operator== + binding).
struct GroundPreconditionKey {
  gar::Predicate pred;
  std::array<std::uint32_t, 2> binding{0, 0};

  bool operator==(const GroundPreconditionKey& o) const {
    return pred == o.pred && binding == o.binding;
  }
};

struct GroundPreconditionKeyHash {
  std::size_t operator()(const GroundPreconditionKey& k) const;
};

// Build the key for an antecedent predicate under `emb` (grounding all
// variable references). Always succeeds for the predicate kinds we cache.
GroundPreconditionKey MakePreconditionKey(const gar::Predicate& pred,
                                          const std::uint32_t* emb);

// Ground a single antecedent predicate to its normalized Fact identity
// (edge facts only: inverse slots normalize to base slot with swapped
// endpoints) — used for Γ membership checks and W wait keys.
bool GroundEdgeFact(const gar::Predicate& pred, const std::uint32_t* emb,
                    const gar::core::graph::TripleGraph& g, Fact* out);

// Evaluate one grounded antecedent predicate (single-predicate
// EvalAntecedent, same semantics as the chase's short-circuiting call —
// used per predicate so every status is known).
PrecondState ClassifyPredicate(const gar::Predicate& pred,
                               const gar::GARule& rule,
                               const std::uint32_t* emb,
                               const gar::core::graph::VertexAttrs* attrs,
                               const gar::core::graph::TripleGraph& g,
                               const AgentContext& ctx);

// H: the precondition cache.
class PreconditionCache {
 public:
  // Static entries are answered forever; dynamic entries answer positives
  // forever and negatives only while `version` is current.
  std::optional<bool> Lookup(const GroundPreconditionKey& k, bool is_static,
                             std::uint64_t version) const;
  void Record(const GroundPreconditionKey& k, bool value, bool is_static,
              std::uint64_t version);

 private:
  struct Entry {
    bool value;
    bool is_static;
    std::uint64_t version;  // Γ version when a dynamic NEGATIVE was seen
  };
  std::unordered_map<GroundPreconditionKey, Entry, GroundPreconditionKeyHash>
      m_;
};

}  // namespace deduce
}  // namespace agent
}  // namespace gar::core::engine

