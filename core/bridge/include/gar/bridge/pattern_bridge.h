#pragma once

#include "core/data_structures/gar_pattern_arrays.h"
#include "gar/rule.h"

// Zero-copy bridge from a gar::GARule to MatrixGraph's pattern array layout
// (core/data_structures/gar_pattern_arrays.h), so rules can be fed directly
// into MatrixGraph's GAR matching kernels (apps/gar_match.cu).
//
// The returned struct holds pointers INTO `rule`; the rule must outlive it.
namespace gar {
namespace bridge {

sics::matrixgraph::core::data_structures::GARPatternArrays ToPatternArrays(
    const GARule& rule);

}  // namespace bridge
}  // namespace gar
