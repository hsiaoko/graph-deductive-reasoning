#pragma once

#include <filesystem>

#include "gar/graph/triple_graph.h"

namespace gar::core::bridge {

// Loads a TripleGraph from a MatrixGraph ImmutableCSR directory: topology
// from graphs/0.bin (out-CSR), relation ids from edge_attr/relationship.bin
// (row i == outgoing_edges slot i), names from dict/entities.tsv and
// dict/relations.tsv (written by kg_loader convert).
gar::core::graph::TripleGraph LoadTripleGraph(
    const std::filesystem::path& immutable_csr_dir, bool with_inverses = true);

}  // namespace gar::core::bridge
