#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "core/io/attributes_io.cuh"

namespace gar::core::graph {

using Attribute = sics::matrixgraph::core::data_structures::Attribute;
using VertexAttributes = sics::matrixgraph::core::io::AttributesIO::Storage;

inline const Attribute* FindAttribute(const VertexAttributes& attrs,
                                      const std::string& name) {
  for (const auto& column : attrs.columns) {
    if (column.name == name) return &column;
  }
  return nullptr;
}

inline bool AttributeValid(const Attribute& attr, std::uint32_t row) {
  return row < attr.n_rows &&
         sics::matrixgraph::core::data_structures::IsValidAt(attr, row);
}

inline double AttributeValue(const Attribute& attr, std::uint32_t row) {
  namespace mg = sics::matrixgraph::core::data_structures;
  switch (attr.type) {
    case mg::ValueType::kInt: return static_cast<double>(mg::GetInt(attr, row));
    case mg::ValueType::kFloat64: return mg::GetFloat64(attr, row);
    case mg::ValueType::kFloat32: return mg::GetFloat32(attr, row);
    case mg::ValueType::kTime:
      return static_cast<double>(mg::GetTime(attr, row));
    case mg::ValueType::kBool: return mg::GetBool(attr, row) ? 1.0 : 0.0;
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

}  // namespace gar::core::graph
