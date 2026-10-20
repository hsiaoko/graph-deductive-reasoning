#pragma once

// BranchBoundExecutor: the HybNS best-first branch-and-bound (paper Figure
// 6, §5.4). Declared in executor.h; the branch-state design lives here.
//
// Branch-state engineering (the key decision): every branch needs its own
// fact set Gamma, WitnessLog and FailureRegistry — AgentState wraps a
// mutable TripleGraph&, which cannot be shared across branches without
// pollution. A branch therefore CLONES the TripleGraph (vector memcpy:
// fb15k-scale is a few MB and milliseconds per fork, trivially affordable
// for searches of tens-to-hundreds of expansions) and wraps the clone in a
// fresh AgentState. The witness/failure logs fork by plain copy (the
// WitnessLog's HDC evidence is an int16 accumulator array, 32 KB at
// D=16384). Anchor for later: a fact-overlay (branches share the base graph
// and carry only their derived-edge delta) would cut the fork cost to the
// journal size; deliberately not done at this scale.
//
// Best-first order (line 3): branch priority = cost + min price(mu) over
// the branch's candidates. Candidates exist only after generation, so
// priorities are computed by DEFERRED evaluation: a branch is pushed with
// key = cost (an admissible lower bound, since min price >= 1); on pop, an
// unevaluated branch generates/scores its candidates, gets its true
// priority and is re-pushed; a branch is expanded only when popped WITH a
// computed priority (every other branch's key is then a lower bound >= the
// popped one, which is exactly best-first). The scored candidates are
// cached for the expansion, so generation happens once per branch.

#include <cstdint>
#include <memory>
#include <vector>

#include "gar/engine/agent/executor.h"

namespace gar {
namespace engine {
namespace agent {

struct Branch {
  Branch(std::unique_ptr<graph::TripleGraph> g,
         const graph::VertexAttrs* attrs, const std::vector<GARule>& rules,
         WitnessLog w, FailureRegistry f, double c, double u,
         std::vector<AgentStep> ch, std::uint64_t s)
      : graph(std::move(g)),
        state(std::make_unique<AgentState>(*graph, attrs, rules)),
        witness(std::move(w)),
        failures(std::move(f)),
        cost(c),
        utility(u),
        chain(std::move(ch)),
        serial(s) {}

  std::unique_ptr<graph::TripleGraph> graph;  // base + this branch's facts
  std::unique_ptr<AgentState> state;
  WitnessLog witness;      // forked from the parent branch
  FailureRegistry failures;
  double cost;             // c: cost paid on this branch
  double utility;          // sum of w_q over derived facts (§3.2)
  std::vector<AgentStep> chain;
  std::uint64_t serial;    // deterministic tie-break (push order)

  // Deferred best-first evaluation (see header comment).
  bool evaluated = false;
  double priority = 0.0;               // cost + min price (once evaluated)
  std::vector<ScoredMotif> scored;     // candidate cache for the expansion
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
