#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/io/attributes_io.cuh"
#include "gar/core/graph/attributes.h"

namespace gar::core::graph {

inline gar::core::graph::VertexAttributes LoadVertexAttributes(
    const std::string& csr_dir) {
  namespace mg = sics::matrixgraph::core::data_structures;
  using sics::matrixgraph::core::io::AttributesIO;
  gar::core::graph::VertexAttributes out;
  std::string dir = csr_dir;
  if (dir.back() != '/') dir += '/';
  if (!std::filesystem::exists(dir + "vertex_attr/meta.yaml")) return out;
  AttributesIO::Storage storage;
  AttributesIO::Read(dir + "vertex_attr", &storage);
  out.entity_id = storage.entity_id;
  out.manifest = std::move(storage.manifest);
  out.blobs = std::move(storage.blobs);
  for (const auto& column : storage.columns) {
    if (column.type == mg::ValueType::kInt ||
        column.type == mg::ValueType::kFloat64 ||
        column.type == mg::ValueType::kFloat32 ||
        column.type == mg::ValueType::kTime ||
        column.type == mg::ValueType::kBool) {
      out.columns.push_back(column);
    }
  }
  return out;
}

}  // namespace gar::core::graph
