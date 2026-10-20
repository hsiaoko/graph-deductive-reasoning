#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "gar/types.h"

// CSRTopology: compressed-sparse-row storage of a rule pattern's structural
// dependency graph, in the RAPIDS/cuGraph layout:
//
//   offsets[num_vertices + 1]  row pointers
//   indices[num_edges]         column (neighbor) vertex ids
//   edge_labels[num_edges]     optional, per-edge label ids (empty = unlabeled)
//
// The out-neighbors of vertex v are indices[offsets[v] .. offsets[v+1]).
namespace gar {

class CSRTopology {
 public:
  CSRTopology() = default;
  CSRTopology(std::vector<EdgeIndex> offsets, std::vector<VertexID> indices,
              std::vector<LabelID> edge_labels = {});

  [[nodiscard]] std::size_t num_vertices() const {
    return offsets.empty() ? 0 : offsets.size() - 1;
  }
  [[nodiscard]] std::size_t num_edges() const { return indices.size(); }
  [[nodiscard]] bool has_edge_labels() const { return !edge_labels.empty(); }

  [[nodiscard]] EdgeIndex degree(VertexID v) const {
    return offsets[v + 1] - offsets[v];
  }
  [[nodiscard]] std::span<const VertexID> neighbors(VertexID v) const {
    return {indices.data() + offsets[v],
            static_cast<std::size_t>(degree(v))};
  }

  // Checks sizes, monotonic offsets and index bounds. Throws std::runtime_error.
  void Validate() const;

  // Binary layout (little-endian raw arrays, no header):
  //   <dir>/csr_offsets.bin       EdgeIndex[num_vertices + 1]
  //   <dir>/csr_indices.bin       VertexID[num_edges]
  //   <dir>/csr_edge_labels.bin   LabelID[num_edges]   (optional)
  static CSRTopology LoadBin(const std::filesystem::path& dir);
  void SaveBin(const std::filesystem::path& dir) const;

  void Print(std::ostream& os) const;

  std::vector<EdgeIndex> offsets;
  std::vector<VertexID> indices;
  std::vector<LabelID> edge_labels;
};

}  // namespace gar
