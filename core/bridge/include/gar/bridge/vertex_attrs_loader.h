#pragma once

// Shared vertex_attr loader for the deduce apps (CPU + GPU). Reads the
// vertex_attr/ directory through MatrixGraph's AttributesIO and converts the
// numeric columns into the graph kernel's plain CPU column type. Must be
// included from a .cu translation unit (AttributesIO is CUDA-compiled).

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/io/attributes_io.cuh"
#include "gar/graph/vertex_attrs.h"

namespace gar {
namespace bridge {

// Loads vertex_attr/ (if present) into plain CPU attribute columns. Columns
// of non-numeric types (strings/lists) are dropped — they carry no
// comparable values for const/var predicates.
inline graph::VertexAttrs LoadVertexAttrs(const std::string& csr_dir) {
  namespace mg_attr = sics::matrixgraph::core::data_structures;
  using sics::matrixgraph::core::io::AttributesIO;
  graph::VertexAttrs out;
  namespace fs = std::filesystem;
  std::string dir = csr_dir;
  if (dir.back() != '/') dir += '/';
  if (!fs::exists(dir + "vertex_attr/meta.yaml")) return out;
  AttributesIO::Storage st;
  AttributesIO::Read(dir + "vertex_attr", &st);
  for (const auto& a : st.columns) {
    graph::AttrColumn col;
    col.name = a.name;
    if (a.valid != nullptr) {
      col.valid.assign(a.valid, a.valid + a.n_rows);
    }
    if (a.type == mg_attr::ValueType::kInt) {
      col.is_int = true;
      col.ints.reserve(a.n_rows);
      for (std::uint32_t i = 0; i < a.n_rows; ++i) {
        col.ints.push_back(mg_attr::GetInt(a, i));
      }
    } else if (a.type == mg_attr::ValueType::kFloat64) {
      col.is_int = false;
      col.doubles.reserve(a.n_rows);
      for (std::uint32_t i = 0; i < a.n_rows; ++i) {
        col.doubles.push_back(mg_attr::GetFloat64(a, i));
      }
    } else {
      continue;  // strings/lists carry no comparable values (v1)
    }
    out.columns.push_back(std::move(col));
  }
  return out;
}

}  // namespace bridge
}  // namespace gar
