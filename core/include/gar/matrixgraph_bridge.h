#pragma once

#include "gar/rule.h"

#ifdef GAR_WITH_MATRIXGRAPH

#include "core/data_structures/gar_pattern_arrays.h"

// Zero-copy bridge from a gar::GARule to MatrixGraph's pattern array layout
// (core/data_structures/gar_pattern_arrays.h), so rules can be fed directly
// into MatrixGraph's GAR matching kernels (apps/gar_match.cu).
//
// The returned struct holds pointers INTO `rule`; the rule must outlive it.
namespace gar {

sics::matrixgraph::core::data_structures::GARPatternArrays ToPatternArrays(
    const GARule& rule);

}  // namespace gar

#endif  // GAR_WITH_MATRIXGRAPH
