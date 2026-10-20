#pragma once

// MotifGenerator: proposes the candidate set A(S) at each agent step.
//
// NaiveMotifGenerator: re-enumerates per step. Rules whose consequent
// predicate cannot reach the goal (d(p0) = inf) are never matched at all —
// the hard symbolic filter of §5.3, settled by one lookup per rule.
// Embeddings come from graph::PatternMatcher under the exact chase
// convention: homomorphism enumeration, vertex-label post-filter, and the
// head-existence check (a grounded conclusion already in Gamma derives
// nothing new, paper §3.1). Retired motifs (FailureRegistry) are excluded.

#include <cstdint>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/failure_registry.h"
#include "gar/engine/agent/motif.h"
#include "gar/engine/agent/state.h"

namespace gar {
namespace engine {
namespace agent {

class MotifGenerator {
 public:
  virtual ~MotifGenerator() = default;
  virtual void Candidates(const AgentState& state,
                          const std::vector<std::uint32_t>& distances,
                          const analysis::PredicateTable& table,
                          std::uint32_t match_cap_per_rule,
                          const FailureRegistry& failures,
                          std::vector<Motif>* out) const = 0;
};

class NaiveMotifGenerator final : public MotifGenerator {
 public:
  void Candidates(const AgentState& state,
                  const std::vector<std::uint32_t>& distances,
                  const analysis::PredicateTable& table,
                  std::uint32_t match_cap_per_rule,
                  const FailureRegistry& failures,
                  std::vector<Motif>* out) const override;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
