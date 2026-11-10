#include "gar/bridge/pattern_bridge.h"

namespace gar {
namespace bridge {

sics::matrixgraph::core::data_structures::GARPatternArrays ToPatternArrays(
    const GARule& rule) {
  namespace mg = sics::matrixgraph::core::data_structures;

  // Direct zero-copy view over GARule's flat edge arrays. The loader keeps
  // them GARPatternArrays-compatible (int32 src/dst/label, CSR order), so no
  // conversion is needed here; the returned pointers are valid for as long
  // as `rule` is alive (this is what makes the old thread_local scratch
  // unnecessary and makes the view thread-safe).
  mg::GARPatternArrays out;
  out.node_label_idx =
      rule.node_labels.empty()
          ? nullptr
          : reinterpret_cast<const std::int32_t*>(rule.node_labels.data());
  out.n_nodes = static_cast<int>(rule.pattern.num_vertices());
  out.edge_src = rule.edge_src.data();
  out.edge_dst = rule.edge_dst.data();
  out.edge_label_idx = rule.edge_labels.data();
  out.n_edges = static_cast<int>(rule.edge_src.size());
  return out;
}

}  // namespace bridge
}  // namespace gar
