// kg_loader: loads knowledge graphs into MatrixGraph's ImmutableCSR.
//
// Modes:
//   kg_loader convert --graph <triple_graph_dir> --out <csr_dir>
//                     [--label-from-prefix]
//       Converts a TripleGraph directory (edges.bin + entities.tsv +
//       relations.tsv, see tools/prepare_triples.py) into MatrixGraph's
//       ImmutableCSR on-disk format (<csr_dir>/meta.yaml, graphs/0.bin,
//       label/0.bin), using MatrixGraph's own Edges / Edgelist2ImmutableCSR
//       / ImmutableCSR::Write pipeline.
//       With --label-from-prefix, the vertex label is taken from the entity
//       name prefix before the first ':' (e.g. "drug:CID..." -> "drug");
//       otherwise all vertex labels are 0.
//
//   kg_loader show --csr <csr_dir> [--print N]
//       Loads the graph back with MatrixGraph's ImmutableCSR::Read and
//       prints metadata, label histogram and N sample vertices.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
// NOTE: do not include <execution> here — it drags in AVX512 intrinsics
// headers that nvcc + GCC 12 cannot compile (MatrixGraph avoids it in .cu
// files for the same reason).

#include "core/data_structures/edgelist.h"
#include "core/data_structures/immutable_csr.cuh"
#include "core/io/attributes_io.cuh"
#include "core/util/format_converter.cuh"

namespace {

using sics::matrixgraph::core::common::EdgeIndex;
using sics::matrixgraph::core::common::VertexID;
using sics::matrixgraph::core::common::VertexLabel;
using sics::matrixgraph::core::data_structures::Edge;
using sics::matrixgraph::core::data_structures::EdgelistMetadata;
using sics::matrixgraph::core::data_structures::Edges;
using sics::matrixgraph::core::data_structures::ImmutableCSR;

struct Triples {
  std::uint32_t num_entities = 0;
  std::vector<std::uint32_t> buf;  // [src, rel, dst] * m
};

Triples ReadTriples(const std::string& dir) {
  std::ifstream in(dir + "/edges.bin", std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open " + dir + "/edges.bin");
  const auto bytes = static_cast<std::size_t>(in.tellg());
  if (bytes % (3 * sizeof(std::uint32_t)) != 0) {
    throw std::runtime_error("edges.bin size is not a multiple of 3*uint32");
  }
  Triples t;
  t.buf.resize(bytes / sizeof(std::uint32_t));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(t.buf.data()),
          static_cast<std::streamsize>(bytes));

  std::ifstream meta(dir + "/meta.yaml");
  std::string key;
  std::uint64_t value;
  while (meta >> key >> value) {
    if (key == "num_entities:") {
      t.num_entities = static_cast<std::uint32_t>(value);
    }
  }
  if (t.num_entities == 0) {  // fall back to max id + 1
    for (std::uint32_t v : t.buf) t.num_entities = std::max(t.num_entities, v);
    ++t.num_entities;
  }
  return t;
}

// Vertex labels from entity-name prefixes ("drug:CID..." -> label of "drug").
// When `label_names` is non-null, it is filled with the per-id prefix names.
std::vector<VertexLabel> LabelsFromPrefix(const std::string& dir,
                                          std::uint32_t n,
                                          std::vector<std::string>* label_names) {
  std::vector<VertexLabel> labels(n, 0);
  std::ifstream in(dir + "/entities.tsv");
  if (!in) return labels;
  std::unordered_map<std::string, VertexLabel> prefix_ids;
  std::string line;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const auto id = static_cast<std::uint32_t>(std::stoul(line.substr(0, tab)));
    const auto& name = line.substr(tab + 1);
    const auto colon = name.find(':');
    if (colon == std::string::npos || id >= n) continue;
    const auto prefix = name.substr(0, colon);
    auto [it, inserted] =
        prefix_ids.emplace(prefix, static_cast<VertexLabel>(prefix_ids.size()));
    labels[id] = it->second;
  }
  if (label_names) {
    label_names->assign(prefix_ids.size(), "");
    for (const auto& [p, l] : prefix_ids) {
      if (l < label_names->size()) (*label_names)[l] = p;
    }
  }
  std::cout << "[convert] " << prefix_ids.size() << " label(s) from prefix: ";
  for (const auto& [p, l] : prefix_ids) std::cout << p << "=" << l << " ";
  std::cout << "\n";
  return labels;
}

void Convert(const std::string& graph_dir, const std::string& out_dir,
             bool label_from_prefix) {
  Triples t = ReadTriples(graph_dir);
  const auto n = t.num_entities;
  const auto m = t.buf.size() / 3;
  std::cout << "[convert] " << graph_dir << ": |V|=" << n << " |E|=" << m
            << "\n";

  Edge* edges = new Edge[m];
  for (std::size_t i = 0; i < m; ++i) {
    edges[i] = Edge(t.buf[3 * i], t.buf[3 * i + 2]);
  }
  VertexID* local2global = new VertexID[n];
  std::iota(local2global, local2global + n, 0);

  std::vector<std::string> label_names;
  std::vector<VertexLabel> labels =
      label_from_prefix ? LabelsFromPrefix(graph_dir, n, &label_names)
                        : std::vector<VertexLabel>(n, 0);
  VertexLabel* vlabel = new VertexLabel[n];
  std::copy(labels.begin(), labels.end(), vlabel);

  EdgelistMetadata metadata;
  metadata.num_vertices = n;
  metadata.num_edges = m;
  metadata.max_vid = n - 1;
  metadata.min_vid = 0;

  Edges edgelist(metadata, edges, local2global, vlabel);
  auto* csr =
      sics::matrixgraph::core::util::format_converter::Edgelist2ImmutableCSR(
          edgelist);

  std::string out = out_dir;
  if (out.back() != '/') out += '/';
  csr->Write(out);
  std::cout << "[convert] wrote ImmutableCSR to " << out << "\n";

  // ---- edge attribute table: "relationship" (Int), in CSR slot order ----
  // MatrixGraph sorts outgoing edges by dst within each src, so the slot
  // order equals a stable (src, dst) sort of the input triples; multiedges
  // on the same (src,dst) are interchangeable among their slots.
  {
    namespace mg_attr = sics::matrixgraph::core::data_structures;
    using sics::matrixgraph::core::io::AttributesIO;

    std::vector<std::uint32_t> order(m);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
      const auto sa = t.buf[3 * a], sb = t.buf[3 * b];
      if (sa != sb) return sa < sb;
      return t.buf[3 * a + 2] < t.buf[3 * b + 2];
    });
    std::vector<std::int64_t> rel_col(m);
    for (std::size_t k = 0; k < m; ++k) {
      rel_col[k] = t.buf[3 * order[k] + 1];
    }
    mg_attr::Attribute rel_attr{};
    std::snprintf(rel_attr.name, sizeof(rel_attr.name), "relationship");
    rel_attr.type = mg_attr::ValueType::kInt;
    rel_attr.n_rows = static_cast<std::uint32_t>(m);
    rel_attr.n_elements = static_cast<std::uint32_t>(m);
    rel_attr.data = rel_col.data();
    AttributesIO::Write(out + "edge_attr", /*entity_id=*/1, &rel_attr, 1);
    std::cout << "[convert] wrote edge_attr/relationship (Int, " << m
              << " rows)\n";
  }

  // ---- vertex attribute table: "type" (Int), when labels come from prefix ----
  if (label_from_prefix) {
    namespace mg_attr = sics::matrixgraph::core::data_structures;
    using sics::matrixgraph::core::io::AttributesIO;

    std::vector<std::int64_t> type_col(n);
    for (std::size_t i = 0; i < n; ++i) type_col[i] = labels[i];
    mg_attr::Attribute type_attr{};
    std::snprintf(type_attr.name, sizeof(type_attr.name), "type");
    type_attr.type = mg_attr::ValueType::kInt;
    type_attr.n_rows = n;
    type_attr.n_elements = n;
    type_attr.data = type_col.data();
    AttributesIO::Write(out + "vertex_attr", /*entity_id=*/0, &type_attr, 1);
    std::cout << "[convert] wrote vertex_attr/type (Int, " << n << " rows)\n";
  }

  // Copy the name dictionaries so the CSR directory is self-contained
  // (mining and attribute lookup can run from it directly).
  {
    namespace fs = std::filesystem;
    fs::create_directories(out + "dict");
    fs::copy_file(graph_dir + "/entities.tsv", out + "dict/entities.tsv",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(graph_dir + "/relations.tsv", out + "dict/relations.tsv",
                  fs::copy_options::overwrite_existing);
    if (!label_names.empty()) {
      std::ofstream lf(out + "dict/labels.tsv");
      for (std::size_t i = 0; i < label_names.size(); ++i) {
        lf << i << "\t" << label_names[i] << "\n";
      }
    }
  }

  delete csr;
  delete[] vlabel;
  delete[] local2global;  // `edges` is owned by `edgelist`
}

void Show(const std::string& csr_dir, VertexID print_n) {
  std::string dir = csr_dir;
  if (dir.back() != '/') dir += '/';

  ImmutableCSR g;
  g.Read(dir);
  const auto md = g.GetMetadata();

  std::cout << "===== " << dir << " =====\n";
  std::cout << "|V|=" << md.num_vertices
            << " |E|=" << md.num_outgoing_edges
            << " (in: " << md.num_incoming_edges << ")"
            << " vid range: [" << md.min_vid << ", " << md.max_vid << "]\n";

  // Vertex-label histogram (top 10).
  const VertexLabel* vl = g.GetVLabelBasePointer();
  std::unordered_map<VertexLabel, std::uint64_t> hist;
  for (VertexID v = 0; v < md.num_vertices; ++v) hist[vl[v]]++;
  std::vector<std::pair<VertexLabel, std::uint64_t>> top(hist.begin(),
                                                         hist.end());
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  std::cout << "vertex labels: " << hist.size() << " distinct, top: ";
  for (std::size_t i = 0; i < top.size() && i < 10; ++i) {
    std::cout << "L" << top[i].first << "=" << top[i].second << "  ";
  }
  std::cout << "\n";

  // Attribute tables, if present.
  for (const char* sub : {"vertex_attr", "edge_attr"}) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir + sub)) continue;
    sics::matrixgraph::core::io::AttributesIO::Storage st;
    sics::matrixgraph::core::io::AttributesIO::Read(dir + sub, &st);
    std::cout << sub << " (entity_id=" << st.entity_id << "): ";
    for (const auto& cm : st.manifest) {
      std::cout << cm.name << ":"
                << sics::matrixgraph::core::data_structures::
                       ValueTypeToString(cm.type)
                << "[" << cm.n_rows << "]  ";
    }
    std::cout << "\n";
    // Sample the first few values of Int columns.
    for (const auto& a : st.columns) {
      if (a.type != sics::matrixgraph::core::data_structures::ValueType::kInt)
        continue;
      std::cout << "  " << a.name << "[0.."
                << std::min<std::uint32_t>(a.n_rows, 5) - 1 << "] =";
      for (std::uint32_t i = 0; i < std::min<std::uint32_t>(a.n_rows, 5); ++i) {
        std::cout << " " << sics::matrixgraph::core::data_structures::GetInt(a, i);
      }
      std::cout << "\n";
    }
  }

  if (print_n > 0) {
    std::cout << "--- sample vertices ---\n";
    g.PrintGraph(print_n);
  }
  std::cout << "\n";
}

// Adds vertex attribute columns (prepare_dbpedia_literals.py output:
// meta.tsv with "name \t Int|Double \t n_rows" plus one "<name>.tsv" of
// "vid \t value" rows per column) into <csr_dir>/vertex_attr, merging with
// any columns already there. Sparse coverage is recorded with a valid mask.
void AddVertexAttrs(const std::string& csr_dir, const std::string& attrs_dir) {
  namespace mg_attr = sics::matrixgraph::core::data_structures;
  using sics::matrixgraph::core::io::AttributesIO;

  std::string dir = csr_dir;
  if (dir.back() != '/') dir += '/';

  ImmutableCSR g;
  g.Read(dir);
  const auto n = g.GetMetadata().num_vertices;

  AttributesIO::Storage st;
  if (std::filesystem::exists(dir + "vertex_attr/meta.yaml")) {
    AttributesIO::Read(dir + "vertex_attr", &st);
  }

  std::ifstream meta(attrs_dir + "/meta.tsv");
  if (!meta) {
    throw std::runtime_error("add-vertex-attrs: cannot open " + attrs_dir +
                             "/meta.tsv");
  }

  std::vector<mg_attr::Attribute> cols(st.columns.begin(), st.columns.end());
  std::vector<std::unique_ptr<std::int64_t[]>> int_bufs;
  std::vector<std::unique_ptr<double[]>> dbl_bufs;
  std::vector<std::unique_ptr<std::uint8_t[]>> valid_bufs;

  std::string name, type;
  std::uint64_t expect_rows;
  while (meta >> name >> type >> expect_rows) {
    std::ifstream in(attrs_dir + "/" + name + ".tsv");
    if (!in) {
      throw std::runtime_error("add-vertex-attrs: cannot open " + attrs_dir +
                               "/" + name + ".tsv");
    }
    auto valid = std::make_unique<std::uint8_t[]>(n);
    mg_attr::Attribute attr{};
    std::snprintf(attr.name, sizeof(attr.name), "%s", name.c_str());
    attr.n_rows = n;
    attr.n_elements = n;
    attr.valid = valid.get();
    if (type == "Int") {
      auto buf = std::make_unique<std::int64_t[]>(n);
      std::uint64_t vid;
      std::int64_t v;
      std::uint64_t rows = 0;
      while (in >> vid >> v) {
        if (vid < n) {
          buf[vid] = v;
          valid[vid] = 1;
          ++rows;
        }
      }
      if (rows != expect_rows) {
        std::cerr << "[add-vertex-attrs] WARNING " << name << ": " << rows
                  << " rows, expected " << expect_rows << "\n";
      }
      attr.type = mg_attr::ValueType::kInt;
      attr.data = buf.get();
      int_bufs.push_back(std::move(buf));
    } else if (type == "Double") {
      auto buf = std::make_unique<double[]>(n);
      std::uint64_t vid;
      double v;
      std::uint64_t rows = 0;
      while (in >> vid >> v) {
        if (vid < n) {
          buf[vid] = v;
          valid[vid] = 1;
          ++rows;
        }
      }
      if (rows != expect_rows) {
        std::cerr << "[add-vertex-attrs] WARNING " << name << ": " << rows
                  << " rows, expected " << expect_rows << "\n";
      }
      attr.type = mg_attr::ValueType::kFloat64;
      attr.data = buf.get();
      dbl_bufs.push_back(std::move(buf));
    } else {
      throw std::runtime_error("add-vertex-attrs: unknown column type " +
                               type);
    }
    valid_bufs.push_back(std::move(valid));
    cols.push_back(attr);
    std::cout << "[add-vertex-attrs] " << name << " (" << type << ", "
              << expect_rows << " rows)\n";
  }

  AttributesIO::Write(dir + "vertex_attr", /*entity_id=*/0, cols.data(),
                      static_cast<std::uint32_t>(cols.size()));
  std::cout << "[add-vertex-attrs] wrote " << cols.size()
            << " column(s) to " << dir << "vertex_attr\n";
}

[[noreturn]] void Usage() {
  std::cerr << "usage:\n"
            << "  kg_loader convert --graph <triple_graph_dir> --out <csr_dir>"
               " [--label-from-prefix]\n"
            << "  kg_loader add-vertex-attrs --csr <csr_dir> --attrs <attrs_dir>\n"
            << "  kg_loader show --csr <csr_dir> [--print N]\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) Usage();
  const std::string mode = argv[1];
  std::string graph_dir, csr_dir, out_dir, attrs_dir;
  bool label_from_prefix = false;
  VertexID print_n = 3;
  for (int i = 2; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--graph") graph_dir = next();
    else if (k == "--csr") csr_dir = next();
    else if (k == "--out") out_dir = next();
    else if (k == "--attrs") attrs_dir = next();
    else if (k == "--print")
      print_n = static_cast<VertexID>(std::stoul(next()));
    else if (k == "--label-from-prefix") label_from_prefix = true;
    else Usage();
  }
  try {
    if (mode == "convert" && !graph_dir.empty() && !out_dir.empty()) {
      Convert(graph_dir, out_dir, label_from_prefix);
    } else if (mode == "add-vertex-attrs" && !csr_dir.empty() &&
               !attrs_dir.empty()) {
      AddVertexAttrs(csr_dir, attrs_dir);
    } else if (mode == "show" && !csr_dir.empty()) {
      Show(csr_dir, print_n);
    } else {
      Usage();
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
