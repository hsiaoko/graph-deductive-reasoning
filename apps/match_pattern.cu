// match_pattern: materializes the match table of a mined GAR rule's pattern
// on a knowledge graph, reusing MatrixGraph's GPU GARMatch task.
//
// Pipeline position: mining (mine_rules) produces rules whose pattern Q is a
// CSR topology + vertex/edge labels; this app plays Q back on the graph and
// collects the matches as a GARMatchArrays table (rows grouped by pivot,
// values deduplicated per pattern position) — the relational view on which
// attribute predicates are mined. With --attr-stats it also profiles the
// vertex attribute columns (vertex_attr/, read through MatrixGraph's
// AttributesIO) over the matched vertices and prints const-predicate
// candidates x.A = c with their pivot-weighted share.
//
// Usage:
//   match_pattern --csr <immutable_csr_dir> --rule <rule_dir>
//                 [--row-cap 1000000] [--match-cap 16777216] [--print 10]
//                 [--attr-stats]
#include <thread>  // must precede MatrixGraph headers (format_converter.cuh)

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// NOTE: do not include <execution> here — it drags in AVX512 intrinsics
// headers that nvcc + GCC 12 cannot compile.

#include "core/data_structures/gar_graph_arrays.h"
#include "core/data_structures/gar_match_arrays.h"
#include "core/data_structures/gar_pattern_arrays.h"
#include "core/io/attributes_io.cuh"
#include "core/task/gpu_task/gar_match.cuh"
#include "gar/miner/triple_graph.h"
#include "gar/rule_loader.h"

namespace {

using sics::matrixgraph::core::data_structures::GARGraphArrays;
using sics::matrixgraph::core::data_structures::GARMatchArrays;
using sics::matrixgraph::core::data_structures::GARPatternArrays;

struct Args {
  std::string csr_dir;
  std::string rule_dir;
  std::uint64_t row_cap = 1'000'000;
  std::uint64_t match_cap = 16'777'216;
  std::size_t print_n = 10;
  bool attr_stats = false;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << k << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--csr") a.csr_dir = next();
    else if (k == "--rule") a.rule_dir = next();
    else if (k == "--row-cap") a.row_cap = std::stoull(next());
    else if (k == "--match-cap") a.match_cap = std::stoull(next());
    else if (k == "--print") a.print_n = std::stoull(next());
    else if (k == "--attr-stats") a.attr_stats = true;
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.csr_dir.empty() || a.rule_dir.empty()) {
    std::cerr << "usage: match_pattern --csr <immutable_csr_dir> --rule "
                 "<rule_dir> [--row-cap N] [--match-cap N] [--print N] "
                 "[--attr-stats]\n";
    std::exit(2);
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    gar::miner::TripleGraph g =
        gar::miner::TripleGraph::LoadImmutableCSR(args.csr_dir,
                                                  /*with_inverses=*/false);
    gar::GARule rule = gar::RuleLoader::Load(args.rule_dir);
    std::cerr << "[match_pattern] graph |V|=" << g.num_entities()
              << " |E|=" << g.num_edges() << ", rule: " << rule.name << "\n";

    // ---- GARGraphArrays (identity vertex ids; base relations only) ----
    const std::uint32_t n = g.num_entities();
    std::vector<std::uint32_t> v_id(n);
    std::iota(v_id.begin(), v_id.end(), 0);
    std::vector<std::int32_t> v_label(n, 0);
    if (g.has_vertex_labels()) {
      for (std::uint32_t v = 0; v < n; ++v) {
        v_label[v] = static_cast<std::int32_t>(g.VertexLabel(v));
      }
    }
    std::vector<std::uint32_t> e_src, e_dst, e_id;
    std::vector<std::int32_t> e_label;
    e_src.reserve(g.num_edges());
    e_dst.reserve(g.num_edges());
    e_id.reserve(g.num_edges());
    e_label.reserve(g.num_edges());
    for (std::uint32_t v = 0; v < n; ++v) {
      const auto [b, e] = g.OutEdgeRange(v);
      for (std::size_t i = b; i < e; ++i) {
        e_src.push_back(v);
        e_dst.push_back(g.OutEdgeDsts()[i]);
        e_id.push_back(static_cast<std::uint32_t>(e_id.size()));
        e_label.push_back(static_cast<std::int32_t>(g.OutEdgeSlots()[i]));
      }
    }
    GARGraphArrays gg{v_id.data(),    v_label.data(),
                      static_cast<int>(n),
                      e_src.data(),   e_dst.data(),
                      e_id.data(),    e_label.data(),
                      static_cast<int>(e_src.size())};

    // ---- GARPatternArrays from the rule (inverse slots folded back to
    // base relations with swapped endpoints) ----
    const auto& topo = rule.pattern;
    const auto pn = static_cast<int>(topo.num_vertices());
    const std::uint32_t num_rel = g.num_relations();
    std::vector<std::int32_t> p_node_label(pn, 0);
    for (int i = 0; i < pn; ++i) {
      const bool constrained = static_cast<std::size_t>(i) <
                                   rule.node_labels.size() &&
                               rule.node_labels[i] != gar::kWildcardLabel;
      if (constrained) {
        p_node_label[i] = static_cast<std::int32_t>(rule.node_labels[i]);
      } else if (g.has_vertex_labels()) {
        std::cerr << "error: pattern node x" << i
                  << " is unconstrained but the graph is typed; GARMatch "
                     "requires concrete vertex labels\n";
        return 1;
      }
    }
    std::vector<std::int32_t> p_src, p_dst, p_label;
    for (int u = 0; u < pn; ++u) {
      const auto nbrs = topo.neighbors(static_cast<gar::VertexID>(u));
      for (std::size_t e = 0; e < nbrs.size(); ++e) {
        if (!topo.has_edge_labels()) {
          std::cerr << "error: pattern edges carry no labels\n";
          return 1;
        }
        const gar::LabelID l = topo.edge_labels[topo.offsets[u] + e];
        if (l == gar::kWildcardLabel) {
          std::cerr << "error: pattern edge x" << u << "->x" << nbrs[e]
                    << " has a wildcard label\n";
          return 1;
        }
        const auto w = static_cast<int>(nbrs[e]);
        if (l >= num_rel) {  // inverse slot: r^-1(u,w) == r(w,u)
          p_src.push_back(w);
          p_dst.push_back(u);
          p_label.push_back(static_cast<std::int32_t>(l - num_rel));
        } else {
          p_src.push_back(u);
          p_dst.push_back(w);
          p_label.push_back(static_cast<std::int32_t>(l));
        }
      }
    }
    GARPatternArrays gp{p_node_label.data(), pn, p_src.data(), p_dst.data(),
                        p_label.data(), static_cast<int>(p_src.size())};

    // ---- match buffers + GPU GARMatch ----
    std::vector<std::uint32_t> row_pivot(args.row_cap);
    std::vector<std::int32_t> row_cond(args.row_cap), row_pos(args.row_cap),
        row_offset(args.row_cap), row_count(args.row_cap);
    std::vector<std::uint32_t> matched(args.match_cap);
    int num_conditions = 0, row_size = 0, match_size = 0;
    GARMatchArrays out{&num_conditions,
                       row_pivot.data(),
                       row_cond.data(),
                       row_pos.data(),
                       row_offset.data(),
                       row_count.data(),
                       static_cast<int>(args.row_cap),
                       &row_size,
                       matched.data(),
                       static_cast<int>(args.match_cap),
                       &match_size};

    sics::matrixgraph::core::task::GARMatch matcher(gg, gp, &out);
    matcher.Run();
    if (matcher.Status() != 0) {
      std::cerr << "error: GARMatch failed with status " << matcher.Status()
                << "\n";
      return 1;
    }

    std::cout << "[match_pattern] pivots=" << row_size / std::max(1, pn)
              << " rows=" << row_size << " matched_pool=" << match_size
              << " num_conditions=" << num_conditions << "\n";
    if (row_size == static_cast<int>(args.row_cap) ||
        match_size == static_cast<int>(args.match_cap)) {
      std::cout << "[match_pattern] WARNING: output truncated at capacity\n";
    }

    // ---- match table preview ----
    const int show = static_cast<int>(
        std::min<std::size_t>(args.print_n, static_cast<std::size_t>(row_size)));
    for (int i = 0; i < show; ++i) {
      std::cout << "  pivot=" << g.EntityName(row_pivot[i]) << " x"
                << row_pos[i] << " | " << row_count[i] << " matched: ";
      const int begin = row_offset[i];
      const int end = std::min(begin + row_count[i], match_size);
      for (int j = begin; j < std::min(begin + 5, end); ++j) {
        if (j > begin) std::cout << ", ";
        std::cout << g.EntityName(matched[j]);
      }
      if (end - begin > 5) std::cout << ", ...";
      std::cout << "\n";
    }

    // ---- const-predicate candidates from vertex attribute columns ----
    if (args.attr_stats) {
      namespace mg_io = sics::matrixgraph::core::io;
      namespace mg_attr = sics::matrixgraph::core::data_structures;
      mg_io::AttributesIO::Storage st;
      try {
        mg_io::AttributesIO::Read(args.csr_dir + "/vertex_attr", &st);
      } catch (const std::exception& e) {
        std::cout << "[attr-stats] no readable vertex_attr (" << e.what()
                  << ")\n";
        return 0;
      }
      std::cout << "[attr-stats] const-predicate candidates (pivot-weighted "
                   "share over matched vertices):\n";
      for (const auto& col : st.columns) {
        if (col.type != mg_attr::ValueType::kInt) continue;
        const auto* data = static_cast<const std::int64_t*>(col.data);
        for (int pos = 0; pos < pn; ++pos) {
          std::unordered_map<std::int64_t, std::uint64_t> hist;
          std::uint64_t total = 0;
          for (int row = 0; row < row_size; ++row) {
            if (row_pos[row] != pos) continue;
            const int begin = row_offset[row];
            const int end = std::min(begin + row_count[row], match_size);
            for (int j = begin; j < end; ++j) {
              if (matched[j] < col.n_rows) {
                ++hist[data[matched[j]]];
                ++total;
              }
            }
          }
          if (total == 0) continue;
          std::vector<std::pair<std::int64_t, std::uint64_t>> top(
              hist.begin(), hist.end());
          std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
          });
          for (std::size_t t = 0; t < top.size() && t < 5; ++t) {
            std::cout << "  x" << pos << "." << col.name << " = " << top[t].first
                      << "   share=" << static_cast<double>(top[t].second) / total
                      << " (" << top[t].second << "/" << total << ")\n";
          }
        }
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
