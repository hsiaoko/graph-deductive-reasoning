#pragma once

// EnumerateAllCandidates: the full candidate set A(S) over ALL DG-reachable
// rules, re-enumerated on each call — the forward bnb branch evaluator's
// enumeration helper (branch-and-bound needs the whole candidate space per
// branch for its optimality certificate, Theorem 5).
//
// This used to be the naive greedy engine's per-step path
// (NaiveMotifGenerator); the incremental GreedyExecutor no longer calls it
// (rule-level preselection + persistent per-rule candidate caches with
// delta matching). It remains EXCLUSIVELY as the bnb helper — do not
// reintroduce per-step all-rules enumeration into greedy.
//
// Rules whose consequent predicate cannot reach the goal (d(p0) = inf) are
// never matched at all — the hard symbolic filter of §5.3, settled by one
// lookup per rule. Embeddings come from graph::PatternMatcher under the
// exact chase convention: homomorphism enumeration, vertex-label
// post-filter, and the head-existence check (a grounded conclusion already
// in Gamma derives nothing new, paper §3.1). Retired motifs
// (FailureRegistry) are excluded.

#include <cstdint>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/failure_registry.h"
#include "gar/engine/agent/motif.h"
#include "gar/engine/agent/state.h"

namespace gar {
namespace engine {
namespace agent {

void EnumerateAllCandidates(const AgentState& state,
                            const std::vector<std::uint32_t>& distances,
                            const analysis::PredicateTable& table,
                            std::uint32_t match_cap_per_rule,
                            const FailureRegistry& failures,
                            std::vector<Motif>* out);

}  // namespace agent
}  // namespace engine
}  // namespace gar
