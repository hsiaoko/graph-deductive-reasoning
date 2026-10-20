#pragma once

#include <cstdint>
#include <limits>

// Basic scalar types shared by the whole library. Widths match MatrixGraph
// (core/common/types.h: VertexID/VertexLabel/EdgeIndex are all uint32_t),
// so CSR buffers can be handed to MatrixGraph without conversion.
namespace gar {

using VertexID = std::uint32_t;   // pattern/graph vertex id
using EdgeIndex = std::uint32_t;  // CSR offset type
using LabelID = std::uint32_t;    // vertex/edge label id

inline constexpr LabelID kWildcardLabel = std::numeric_limits<LabelID>::max();

}  // namespace gar
