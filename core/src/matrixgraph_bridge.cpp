#include "gar/matrixgraph_bridge.h"

#ifdef GAR_WITH_MATRIXGRAPH

namespace gar {

sics::matrixgraph::core::data_structures::GARPatternArrays ToPatternArrays(
    const GARule& rule) {
  namespace mg = sics::matrixgraph::core::data_structures;

  // GARPatternArrays consumes an edge list (src/dst/label); expand the CSR
  // on the fly into thread-local scratch is avoided — the pattern edges are
  // already flat in CSR order, so we only need src/dst views. We synthesize
  // the src array lazily per rule; keep it beside the rule via static storage
  // is unsafe, so callers that need persistence should materialize it.
  // Here we expand into a function-local static-free buffer owned by the
  // caller through the returned views' backing store: NOT thread-safe.
  static thread_local std::vector<std::int32_t> src_scratch;
  static thread_local std::vector<std::int32_t> dst_scratch;
  static thread_local std::vector<std::int32_t> label_scratch;

  const auto& p = rule.pattern;
  const auto m = p.num_edges();
  src_scratch.clear();
  dst_scratch.clear();
  label_scratch.clear();
  src_scratch.reserve(m);
  dst_scratch.reserve(m);
  label_scratch.reserve(m);

  for (std::size_t v = 0; v < p.num_vertices(); ++v) {
    for (VertexID dst : p.neighbors(static_cast<VertexID>(v))) {
      src_scratch.push_back(static_cast<std::int32_t>(v));
      dst_scratch.push_back(static_cast<std::int32_t>(dst));
    }
  }
  for (std::size_t e = 0; e < m; ++e) {
    label_scratch.push_back(p.has_edge_labels() ? p.edge_labels[e] : 0);
  }

  mg::GARPatternArrays out;
  // Widths match (gar::LabelID == uint32_t == MatrixGraph VertexLabel);
  // GARPatternArrays declares int32_t pointers, so a reinterpret is needed.
  out.node_label_idx =
      rule.node_labels.empty()
          ? nullptr
          : reinterpret_cast<const std::int32_t*>(rule.node_labels.data());
  out.n_nodes = static_cast<int>(p.num_vertices());
  out.edge_src = src_scratch.data();
  out.edge_dst = dst_scratch.data();
  out.edge_label_idx = label_scratch.data();
  out.n_edges = static_cast<int>(m);
  return out;
}

}  // namespace gar

#endif  // GAR_WITH_MATRIXGRAPH
