#pragma once

// Motif: mu = (phi, h) — a candidate rule application: rule index plus one
// embedding of its pattern, matched by labels and edges alone (PatternMatcher
// homomorphism + vertex-label post-filter, the exact miner/chase convention;
// preconditions X are NOT tested at selection time, paper §3.1).

#include <cstdint>
#include <vector>

#include "gar/engine/agent/fact.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule.h"

namespace gar {
namespace analysis {
class PredicateTable;
}  // namespace analysis

namespace engine {
namespace agent {

struct Motif {
  std::uint32_t rule = 0;
  std::vector<std::uint32_t> embedding;  // one vertex id per pattern vertex
};

// ScoredMotif: a motif with its score breakdown (paper §4.1).
struct ScoredMotif {
  Motif motif;
  double rel_fam = 0.0;   // familiarity: gamma^{sum_q d(q)} (App. H ScoreF)
  double rel_evd = 0.0;   // evidence: witnessed-premise share / HDC cosine
  double rel = 0.0;       // rel_fam + nu*rel_evd + nu2*conf (relevance part)
  double dcost = 0.0;     // estimated application cost, unwitnessed premises
  double price = 0.0;     // completion-price lower bound w0 + d(p0) (§3.4)
  double total = 0.0;     // alpha*rel - beta*dcost
  bool admissible = false;  // d(p0) < inf and the grounded conclusion is new
};

// Rule head resolution, identical to the chase (cpu_chase_engine.cpp): the
// slot comes from the consequent's edge label; the grounded endpoints are
// the consequent's (src, dst), overridden by `pivot` when it has size 2.
struct RuleHead {
  bool valid = false;
  std::uint32_t slot = 0;
  std::uint32_t pu = 0, pv = 0;  // pattern vertices whose grounding is the head
};
RuleHead ResolveRuleHead(const gar::GARule& rule);

// Stable key of a motif for the failure registry: rule + embedding.
std::uint64_t MotifHash(std::uint32_t rule, const std::uint32_t* emb,
                        std::uint32_t n);

// Grounds every premise of the motif to a Fact: each pattern edge (inverse
// slots normalized to base slot with swapped endpoints) plus each grounded
// antecedent predicate. The conclusion is NOT a premise. Used by the
// scorers (rel_evd / dcost) and by the executor (witness on application).
void GroundPremises(const gar::GARule& rule, const Motif& mu,
                    const graph::TripleGraph& g,
                    const analysis::PredicateTable& table,
                    std::vector<Fact>* out);

}  // namespace agent
}  // namespace engine
}  // namespace gar
