#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gar/graph/fact.h"
#include "gar/graph/triple_graph.h"

namespace gar::core::graph {

struct RoundMerge {
  std::uint64_t new_facts = 0;
  bool deriv_capped = false;  // max_derivations hit mid-merge
};

// Ordered merge of one chase round's per-rule candidate facts: dedupes
// (slot, src, dst) across rules in rule order (the lowest-index rule gets
// the credit), appends to *out_derived / (*out_rule_derived)[i], and merges
// the batch into the graph (base + inverse mirror). Shared by the CPU chase
// and the GPU deduce app so credit assignment is identical across engines.
RoundMerge MergeRoundFacts(
    const std::vector<std::vector<DerivedEdge>>& contribs,
    std::uint64_t max_derivations, TripleGraph& g,
    std::vector<DerivedEdge>* out_derived,
    std::vector<std::uint64_t>* out_rule_derived);

// keep[t] = edge (src[t], dst[t], slot) NOT present in g — the head
// pre-filter of the chase, factored out for the GPU deduce app (whose
// .cu translation unit cannot include TBB headers). Parallel over t when
// oneTBB is available.
void MarkAbsentEdges(const TripleGraph& g,
                     const std::uint32_t* src, const std::uint32_t* dst,
                     std::uint32_t slot, std::size_t n, char* keep);

}  // namespace gar::core::graph
