#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/task/cpu_task/hom_match.cuh"
#include "gar/core/graph/graph.h"

// PatternMatcher: enumerates embeddings of a small connected pattern on a
// Graph, under homomorphism semantics (distinct pattern vertices may
// map to the same data vertex — the same convention as ChainMiner).
//
// A pattern edge (u -> v, slot s) matches a data edge emb[u] -> emb[v] in
// relation slot s (slots include inverse relations, id >= num_relations).
//
// Matching is backtracking over a greedy vertex order: starting from vertex
// 0, each next vertex is an unbound vertex adjacent to the bound set,
// preferring the one with the most edges back into the bound set. Candidate
// bindings are driven by the first edge to an already-bound endpoint (a
// binary search over the slot's sorted pair array, or over the flipped
// slot's array when the bound endpoint is the destination); every other
// edge to a bound vertex is checked for existence via the unified
// out-adjacency (OutEdgeRange binary search).
namespace gar::core::graph {

struct PatternSpec {
  std::uint32_t num_vertices;
  // Edges as (src, dst, slot); slot is a Graph slot id (incl. inverses).
  std::vector<std::array<std::uint32_t, 3>> edges;
};

class PatternMatcher {
 public:
  explicit PatternMatcher(const Graph& g) : g_(g) {
    if (g_.version() == 0) {
      // Reuse the graph's already-materialized base adjacency (sorted pairs +
      // hash sets) instead of rebuilding it from raw edges (O(E log E)).
      static_index_ = std::make_unique<
          sics::matrixgraph::core::task::cpu_task::HomMatchIndex>(
          g_.BaseRelPairs(), g_.BaseRelPairSets(),
          static_cast<int>(g_.num_relations()));
    }
  }

  // Enumerates embeddings, appending them flat (num_vertices per embedding)
  // to *out_embeddings. Stops once match_cap embeddings have been produced
  // and sets *capped. Returns the number of embeddings produced.
  //
  // The graph must be loaded with inverses (with_inverses=true): candidate
  // generation for backward edges walks the flipped slot's pair array.
  std::uint64_t Enumerate(const PatternSpec& p, std::uint64_t match_cap,
                          std::vector<std::uint32_t>* out_embeddings,
                          bool* capped) const;

  // Pinned variant: `pins[v] = w` forces emb[v] == w — used by goal-directed
  // search to bind a rule's head to the goal constants, collapsing the
  // candidate space to goal-relevant motifs. Enumeration order is unchanged
  // when pins is empty (the plain overload above is byte-identical behavior;
  // it forwards here). Pinning a seed-side vertex narrows the seed scan via
  // binary search on the slot's sorted pairs (or the flipped slot's when the
  // pinned endpoint is the destination).
  std::uint64_t Enumerate(
      const PatternSpec& p, std::uint64_t match_cap,
      std::vector<std::uint32_t>* out_embeddings, bool* capped,
      const std::unordered_map<std::uint32_t, std::uint32_t>& pins) const;

  // Bulk-delta: enumerate over a provided batch of seed-slot pairs (the new
  // edges) instead of binary-searching the full slot array per request. Builds
  // the pattern setup once and runs the join backtracking in a tight loop over
  // the seeds with reused buffers. `seed_edge` selects which pattern edge is the
  // seed (its two endpoints bind order[0]/order[1]); `seed_pairs` are raw packed
  // (src,dst) pairs of that edge's slot, in the slot's own orientation.
  std::uint64_t EnumerateSeeds(const PatternSpec& p, std::uint32_t seed_edge,
                               std::uint64_t match_cap,
                               const std::vector<std::uint64_t>& seed_pairs,
                               std::vector<std::uint32_t>* out_embeddings,
                               bool* capped) const;

 private:
  const Graph& g_;
  std::unique_ptr<sics::matrixgraph::core::task::cpu_task::HomMatchIndex>
      static_index_;
  sics::matrixgraph::core::task::cpu_task::HomMatcher static_matcher_;
};

}  // namespace gar::core::graph
