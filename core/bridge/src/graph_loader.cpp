#include "gar/bridge/graph_loader.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace gar::core::bridge {

namespace {

std::vector<std::string> ReadNames(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("TripleGraph: cannot open " + path.string());
  }
  // Names are stored in id order: "id \t name".
  std::vector<std::pair<std::uint32_t, std::string>> rows;
  std::string line;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    rows.emplace_back(static_cast<std::uint32_t>(std::stoul(line.substr(0, tab))),
                      line.substr(tab + 1));
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> names(rows.empty() ? 0 : rows.back().first + 1);
  for (auto& [id, name] : rows) names[id] = std::move(name);
  return names;
}

}  // namespace

gar::core::graph::TripleGraph LoadTripleGraph(const std::filesystem::path& dir,
                                   bool with_inverses) {
  // meta.yaml: GraphMetadata counts (first occurrence wins; the subgraph
  // block repeats them with identical values for a single-graph store).
  std::uint32_t n = 0;
  std::uint64_t m = 0;
  {
    std::ifstream meta(dir / "meta.yaml");
    if (!meta) {
      throw std::runtime_error("TripleGraph: cannot open " +
                               (dir / "meta.yaml").string());
    }
    std::string key;
    while (meta >> key) {
      if (key == "num_vertices:" && n == 0) {
        std::uint64_t v;
        meta >> v;
        n = static_cast<std::uint32_t>(v);
      } else if (key == "num_outgoing_edges:" && m == 0) {
        meta >> m;
      }
    }
  }
  if (n == 0) {
    throw std::runtime_error("TripleGraph: meta.yaml missing num_vertices");
  }

  // graphs/0.bin packed layout (9 arrays, all uint32).
  std::ifstream in(dir / "graphs/0.bin", std::ios::binary);
  if (!in) {
    throw std::runtime_error("TripleGraph: cannot open " +
                             (dir / "graphs/0.bin").string());
  }
  auto read_vec = [&](std::size_t count) {
    std::vector<std::uint32_t> v(count);
    in.read(reinterpret_cast<char*>(v.data()),
            static_cast<std::streamsize>(count * sizeof(std::uint32_t)));
    if (!in) {
      throw std::runtime_error("TripleGraph: graphs/0.bin truncated");
    }
    return v;
  };
  in.seekg(sizeof(std::uint32_t) * 3 * n, std::ios::beg);  // skip gid/degrees
  auto in_off = read_vec(n + 1);
  auto out_off = read_vec(n + 1);
  (void)in_off;
  if (m == 0) m = out_off[n];
  in.seekg(sizeof(std::uint32_t) * m, std::ios::cur);  // skip in_edges
  auto out_edges = read_vec(m);

  // edge_attr/relationship.bin: int64 per out-edge slot.
  std::ifstream rin(dir / "edge_attr/relationship.bin", std::ios::binary);
  if (!rin) {
    throw std::runtime_error("TripleGraph: cannot open " +
                             (dir / "edge_attr/relationship.bin").string());
  }
  std::vector<std::int64_t> rel(m);
  rin.read(reinterpret_cast<char*>(rel.data()),
           static_cast<std::streamsize>(m * sizeof(std::int64_t)));
  if (!rin) {
    throw std::runtime_error("TripleGraph: relationship.bin truncated");
  }

  // Expand CSR slots to triples.
  std::vector<std::uint32_t> buf(3 * m);
  for (std::uint32_t v = 0; v < n; ++v) {
    for (std::uint32_t e = out_off[v]; e < out_off[v + 1]; ++e) {
      buf[3 * e] = v;
      buf[3 * e + 1] = static_cast<std::uint32_t>(rel[e]);
      buf[3 * e + 2] = out_edges[e];
    }
  }

  auto g = gar::core::graph::TripleGraph::Build(ReadNames(dir / "dict/entities.tsv"),
                                     ReadNames(dir / "dict/relations.tsv"),
                                     std::move(buf), with_inverses);

  // label/0.bin: optional uint32 per-vertex labels; dict/labels.tsv: optional
  // label names. Uniformly labeled graphs keep has_vertex_labels_ = false.
  {
    std::ifstream lin(dir / "label/0.bin", std::ios::binary | std::ios::ate);
    if (lin && static_cast<std::uint64_t>(lin.tellg()) ==
                   static_cast<std::uint64_t>(n) * sizeof(std::uint32_t)) {
      g.vertex_labels_.resize(n);
      lin.seekg(0);
      lin.read(reinterpret_cast<char*>(g.vertex_labels_.data()),
               static_cast<std::streamsize>(n * sizeof(std::uint32_t)));
      std::uint32_t first = g.vertex_labels_[0];
      bool uniform = true;
      std::uint32_t max_label = first;
      for (std::uint32_t l : g.vertex_labels_) {
        max_label = std::max(max_label, l);
        if (l != first) {
          uniform = false;
          break;
        }
      }
      if (!uniform) {
        for (std::uint32_t l : g.vertex_labels_) {
          max_label = std::max(max_label, l);
        }
        g.num_labels_ = max_label + 1;
        g.has_vertex_labels_ = true;
        const std::filesystem::path names_path = dir / "dict/labels.tsv";
        if (std::filesystem::exists(names_path)) {
          g.label_names_ = ReadNames(names_path);
        }
      } else {
        g.vertex_labels_.clear();
      }
    }
  }
  return g;
}

}  // namespace gar::core::bridge
