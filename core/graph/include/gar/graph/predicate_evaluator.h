#pragma once

#include <cstdint>
#include <vector>

#include "gar/graph/triple_graph.h"
#include "gar/graph/vertex_attrs.h"
#include "gar/rule.h"

namespace gar::core::graph {

// Evaluates a rule's antecedent conjunction against one embedding:
// const (x.A⊕c) / var (x.A⊕y.B) / exists (x.A) against the vertex
// attribute table (nullptr attrs makes every attribute predicate false),
// edge (l(x,y)) against the graph; ml predicates return false. Shared by
// the chase and the validate_rules app.
bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttrs* attrs,
                    const TripleGraph& g);

}  // namespace gar::core::graph
