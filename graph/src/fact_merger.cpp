#include "gar/graph/fact_merger.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(GAR_GRAPH_HAS_TBB)
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#endif

namespace gar {
namespace graph {

namespace {

bool HasEdge(const TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  const auto [b, e] = g.OutEdgeRange(s);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  auto lo = std::lower_bound(dsts + b, dsts + e, d);
  for (auto it = lo; it != dsts + e && *it == d; ++it) {
    if (g.OutEdgeSlots()[it - dsts] == slot) return true;
  }
  return false;
}

}  // namespace

RoundMerge MergeRoundFacts(
    const std::vector<std::vector<DerivedEdge>>& contribs,
    std::uint64_t max_derivations, TripleGraph& g,
    std::vector<DerivedEdge>* out_derived,
    std::vector<std::uint64_t>* out_rule_derived) {
  // Ordered merge: dedupe (slot, src, dst) across rules; the lowest-index
  // rule producing a fact gets the credit.
  std::unordered_map<std::uint32_t,
                     std::vector<std::pair<std::uint32_t, std::uint32_t>>>
      pending;
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint64_t>>
      pending_sets;
  RoundMerge out;
  for (std::size_t i = 0; i < contribs.size() && !out.deriv_capped; ++i) {
    for (const auto& e : contribs[i]) {
      if (!pending_sets[e.slot]
               .insert(PackPair(e.src, e.dst))
               .second) {
        continue;
      }
      pending[e.slot].emplace_back(e.src, e.dst);
      ++out.new_facts;
      ++(*out_rule_derived)[i];
      out_derived->push_back(e);
      if (out_derived->size() >= max_derivations) {
        out.deriv_capped = true;
        break;
      }
    }
  }
  if (out.new_facts > 0) {
    // Merge the round's facts into the graph (base + inverse mirror), one
    // out-adjacency rebuild for the whole batch.
    g.AddEdgesBatch(pending);
  }
  return out;
}

void MarkAbsentEdges(const TripleGraph& g,
                     const std::uint32_t* src, const std::uint32_t* dst,
                     std::uint32_t slot, std::size_t n, char* keep) {
#if defined(GAR_GRAPH_HAS_TBB)
  tbb::parallel_for(
      tbb::blocked_range<std::size_t>(0, n),
      [&](const tbb::blocked_range<std::size_t>& r) {
        for (std::size_t t = r.begin(); t != r.end(); ++t) {
          keep[t] = !HasEdge(g, src[t], dst[t], slot) ? 1 : 0;
        }
      });
#else
  for (std::size_t t = 0; t < n; ++t) {
    keep[t] = !HasEdge(g, src[t], dst[t], slot) ? 1 : 0;
  }
#endif
}

}  // namespace graph
}  // namespace gar
