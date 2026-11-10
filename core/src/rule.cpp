#include "gar/rule.h"

#include <stdexcept>

namespace gar {

void GARule::Validate() const {
  const std::uint32_t n = n_vertices;

  if (!node_labels.empty() && node_labels.size() != n) {
    throw std::runtime_error("GARule '" + name +
                             "': node_labels size != num_vertices");
  }
  if (!node_label_names.empty() && node_label_names.size() != n) {
    throw std::runtime_error("GARule '" + name +
                             "': node_label_names size != num_vertices");
  }
  for (std::size_t e = 0; e < edge_src.size(); ++e) {
    if (edge_src[e] < 0 || edge_dst[e] < 0 ||
        static_cast<std::uint32_t>(edge_src[e]) >= n ||
        static_cast<std::uint32_t>(edge_dst[e]) >= n) {
      throw std::runtime_error("GARule '" + name +
                               "': pattern edge endpoint out of range");
    }
  }

  auto in_range = [n](VertexID v) { return v < n; };
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
  const auto nv = n_vertices;
  std::vector<EdgeIndex> indeg(nv, 0);
  std::vector<EdgeIndex> outdeg(nv, 0);
  for (std::size_t e = 0; e < edge_src.size(); ++e) {
    const auto s = static_cast<std::uint32_t>(edge_src[e]);
    const auto d = static_cast<std::uint32_t>(edge_dst[e]);
    if (d < nv) ++indeg[d];
    if (s < nv) ++outdeg[s];
  }
  os << "  Q (pattern):\n";
  os << "    ### num_vertices: " << nv << ", num_edges: " << edge_src.size()
     << " ###\n";
  for (std::uint32_t u = 0; u < nv; ++u) {
    os << "    ===vid: x" << u << ", indegree: " << indeg[u]
       << ", outdegree: " << outdeg[u] << ", label: ";
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
    {
      bool first = true;
      for (std::size_t e = 0; e < edge_src.size(); ++e) {
        if (static_cast<std::uint32_t>(edge_dst[e]) != u) continue;
        os << (first ? "      Incoming edges: " : "");
        os << "x" << edge_src[e] << "["
           << label_of(static_cast<LabelID>(edge_labels[e])) << "], ";
        first = false;
      }
      if (!first) os << "\n";
    }
    {
      bool first = true;
      for (std::size_t e = 0; e < edge_src.size(); ++e) {
        if (static_cast<std::uint32_t>(edge_src[e]) != u) continue;
        os << (first ? "      Outgoing edges: " : "");
        os << "x" << edge_dst[e] << "["
           << label_of(static_cast<LabelID>(edge_labels[e])) << "], ";
        first = false;
      }
      if (!first) os << "\n";
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
