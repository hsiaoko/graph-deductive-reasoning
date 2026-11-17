#pragma once

#include <filesystem>

#include "gar/core/graph/graph.h"

namespace gar::core::graph {

// Loads a Graph from a MatrixGraph ImmutableCSR directory: topology
// from graphs/0.bin (out-CSR), relation ids from edge_attr/relationship.bin
// (row i == outgoing_edges slot i), names from dict/entities.tsv and
// dict/relations.tsv (written by kg_loader convert).
gar::core::graph::Graph LoadGraph(
    const std::filesystem::path& immutable_csr_dir, bool with_inverses = true);

}  // namespace gar::core::graph
