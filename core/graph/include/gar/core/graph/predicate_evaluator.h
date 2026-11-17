#pragma once

#include <cstdint>
#include <vector>

#include "gar/core/graph/graph.h"
#include "gar/core/graph/attributes.h"
#include "gar/core/rule.h"

namespace gar::core::graph {

// Evaluates a rule's antecedent conjunction against one embedding:
// const (x.A⊕c) / var (x.A⊕y.B) / exists (x.A) against the vertex
// attribute table (nullptr attrs makes every attribute predicate false),
// edge (l(x,y)) against the graph; ml predicates return false. Shared by
// the chase and the validate_rules app.
bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttributes* attrs,
                    const Graph& g);

}  // namespace gar::core::graph
