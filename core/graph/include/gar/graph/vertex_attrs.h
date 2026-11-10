#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CPU-side vertex attribute columns, the graph kernel's plain attribute
// table. The bridge module converts MatrixGraph AttributesIO columns
// (Int / Float64, with valid mask) into these; the graph kernel never sees
// MatrixGraph types.
namespace gar::core::graph {

struct AttrColumn {
  std::string name;
  bool is_int = true;
  std::vector<std::uint8_t> valid;  // empty = dense (every row valid)
  std::vector<std::int64_t> ints;
  std::vector<double> doubles;

  [[nodiscard]] bool Valid(std::uint32_t row) const {
    return valid.empty() || (row < valid.size() && valid[row]);
  }
  [[nodiscard]] double Value(std::uint32_t row) const {
    return is_int ? static_cast<double>(ints[row]) : doubles[row];
  }
};

struct VertexAttrs {
  std::vector<AttrColumn> columns;
  [[nodiscard]] const AttrColumn* Find(const std::string& name) const {
    for (const auto& c : columns) {
      if (c.name == name) return &c;
    }
    return nullptr;
  }
};

}  // namespace gar::core::graph
