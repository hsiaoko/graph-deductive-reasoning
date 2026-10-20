#include "gar/rule.h"

#include <stdexcept>

namespace gar {

void GARule::Validate() const {
  pattern.Validate();
  const auto n = pattern.num_vertices();

  if (!node_labels.empty() && node_labels.size() != n) {
    throw std::runtime_error("GARule '" + name +
                             "': node_labels size != num_vertices");
  }
  if (!node_label_names.empty() && node_label_names.size() != n) {
    throw std::runtime_error("GARule '" + name +
                             "': node_label_names size != num_vertices");
  }

  auto in_range = [n](VertexID v) { return static_cast<std::size_t>(v) < n; };
  auto check_node = [this, in_range](VertexID v, const char* where) {
    if (!in_range(v)) {
      throw std::runtime_error("GARule '" + name + "': " + where +
                               " references unknown node v" +
                               std::to_string(v));
    }
  };
  auto check_pred = [&](const Predicate& p, const char* where) {
    switch (p.kind) {
      case PredKind::kConst:
      case PredKind::kExists:
        check_node(p.node, where);
        break;
      case PredKind::kVar:
        check_node(p.node, where);
        check_node(p.rhs_node, where);
        break;
      case PredKind::kEdge:
        check_node(p.src, where);
        check_node(p.dst, where);
        break;
      case PredKind::kMl:
        if (p.ml_vars.size() != 2) {
          throw std::runtime_error("GARule '" + name + "': " + where +
                                   " ML predicate expects exactly 2 variables");
        }
        check_node(p.ml_vars[0], where);
        check_node(p.ml_vars[1], where);
        break;
    }
  };

  for (VertexID v : pivot) check_node(v, "pivot");
  for (const auto& p : antecedent) check_pred(p, "antecedent");
  if (consequent) check_pred(*consequent, "consequent");
}

void GARule::Print(std::ostream& os) const {
  os << "GARule: " << (name.empty() ? id : name) << "\n";
  if (!description.empty()) {
    os << "  description: " << description << "\n";
  }

  std::unordered_map<LabelID, const std::string*> id_to_name;
  for (const auto& [label_name, label_id] : labels) {
    id_to_name.emplace(label_id, &label_name);
  }
  auto label_of = [&](LabelID lid) -> std::string {
    if (lid == kWildcardLabel) return "*";
    const auto it = id_to_name.find(lid);
    return it != id_to_name.end() ? *it->second
                                  : "label_" + std::to_string(lid);
  };

  // Q: pattern graph, rendered in the style of MatrixGraph's
  // ImmutableCSR::PrintGraph (per-vertex header + in/out adjacency, edges
  // annotated with their label names).
  const auto nv = static_cast<std::uint32_t>(pattern.num_vertices());
  std::vector<EdgeIndex> indeg(nv, 0);
  for (VertexID v : pattern.indices) {
    if (v < nv) ++indeg[v];
  }
  os << "  Q (pattern CSR):\n";
  os << "    ### num_vertices: " << nv << ", num_edges: " << pattern.num_edges()
     << " ###\n";
  for (std::uint32_t u = 0; u < nv; ++u) {
    os << "    ===vid: x" << u << ", indegree: " << indeg[u]
       << ", outdegree: " << (pattern.offsets[u + 1] - pattern.offsets[u])
       << ", label: ";
    if (u < node_labels.size() && node_labels[u] != kWildcardLabel) {
      if (u < node_label_names.size() && !node_label_names[u].empty()) {
        os << node_label_names[u];
      } else {
        os << label_of(node_labels[u]);
      }
    } else {
      os << "*";
    }
    os << "===\n";
    if (indeg[u] != 0) {
      os << "      Incoming edges: ";
      for (std::uint32_t s = 0; s < nv; ++s) {
        for (EdgeIndex e = pattern.offsets[s]; e < pattern.offsets[s + 1]; ++e) {
          if (pattern.indices[e] != u) continue;
          os << "x" << s;
          if (pattern.has_edge_labels()) {
            os << "[" << label_of(pattern.edge_labels[e]) << "]";
          }
          os << ", ";
        }
      }
      os << "\n";
    }
    const auto nbrs = pattern.neighbors(u);
    if (!nbrs.empty()) {
      os << "      Outgoing edges: ";
      for (std::size_t e = 0; e < nbrs.size(); ++e) {
        os << "x" << nbrs[e];
        if (pattern.has_edge_labels()) {
          os << "[" << label_of(pattern.edge_labels[pattern.offsets[u] + e])
             << "]";
        }
        os << ", ";
      }
      os << "\n";
    }
    os << "    ****************************************\n";
  }

  // D: the matching dependency X -> p0 over Q's matches — same ### header
  // convention as the pattern section, predicates listed one per line.
  auto kind_str = [](PredKind k) -> const char* {
    switch (k) {
      case PredKind::kConst:  return "const";
      case PredKind::kVar:    return "var";
      case PredKind::kEdge:   return "edge";
      case PredKind::kExists: return "exists";
      case PredKind::kMl:     return "ml";
    }
    return "?";
  };
  os << "  D (dependency):\n";
  os << "    ### |X|: " << antecedent.size();
  if (!pivot.empty()) {
    os << ", pivot:";
    for (VertexID v : pivot) os << " x" << v;
  }
  if (quality.support > 0 || quality.confidence > 0) {
    os << ", support: " << quality.support
       << ", confidence: " << quality.confidence;
  }
  os << " ###\n";
  if (antecedent.empty()) {
    os << "    X: <empty>\n";
  } else {
    os << "    X:\n";
    for (const auto& p : antecedent) {
      os << "      [" << kind_str(p.kind) << "] " << p << "\n";
    }
  }
  os << "    Y: ";
  if (consequent) {
    os << "[" << kind_str(consequent->kind) << "] " << *consequent << "\n";
  } else {
    os << "<none> (pattern query)\n";
  }
}

}  // namespace gar
