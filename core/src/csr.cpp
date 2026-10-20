#include "gar/csr.h"

#include <cstring>
#include <fstream>
#include <ostream>
#include <stdexcept>

namespace gar {

CSRTopology::CSRTopology(std::vector<EdgeIndex> offsets,
                         std::vector<VertexID> indices,
                         std::vector<LabelID> edge_labels)
    : offsets(std::move(offsets)),
      indices(std::move(indices)),
      edge_labels(std::move(edge_labels)) {
  Validate();
}

void CSRTopology::Validate() const {
  if (offsets.empty()) {
    throw std::runtime_error("CSRTopology: offsets must not be empty");
  }
  if (offsets.front() != 0) {
    throw std::runtime_error("CSRTopology: offsets[0] must be 0");
  }
  if (static_cast<std::size_t>(offsets.back()) != indices.size()) {
    throw std::runtime_error(
        "CSRTopology: offsets.back() (" + std::to_string(offsets.back()) +
        ") != indices.size() (" + std::to_string(indices.size()) + ")");
  }
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    if (offsets[i] < offsets[i - 1]) {
      throw std::runtime_error("CSRTopology: offsets not monotone at row " +
                               std::to_string(i - 1));
    }
  }
  const auto n = static_cast<VertexID>(num_vertices());
  for (VertexID dst : indices) {
    if (dst >= n) {
      throw std::runtime_error("CSRTopology: column index out of range: " +
                               std::to_string(dst));
    }
  }
  if (!edge_labels.empty() && edge_labels.size() != indices.size()) {
    throw std::runtime_error("CSRTopology: edge_labels size mismatch");
  }
}

namespace {

template <typename T>
std::vector<T> ReadArray(const std::filesystem::path& path, bool required) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (!required) return {};
    throw std::runtime_error("CSRTopology: cannot open " + path.string());
  }
  const auto bytes = in.tellg();
  if (bytes % static_cast<std::streamoff>(sizeof(T)) != 0) {
    throw std::runtime_error("CSRTopology: " + path.string() +
                             " size is not a multiple of element size");
  }
  std::vector<T> out(static_cast<std::size_t>(bytes) / sizeof(T));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out.data()), bytes);
  if (!in) {
    throw std::runtime_error("CSRTopology: failed reading " + path.string());
  }
  return out;
}

template <typename T>
void WriteArray(const std::filesystem::path& path, const std::vector<T>& v) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("CSRTopology: cannot write " + path.string());
  }
  out.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(T)));
}

}  // namespace

CSRTopology CSRTopology::LoadBin(const std::filesystem::path& dir) {
  CSRTopology g;
  g.offsets = ReadArray<EdgeIndex>(dir / "csr_offsets.bin", true);
  g.indices = ReadArray<VertexID>(dir / "csr_indices.bin", true);
  g.edge_labels = ReadArray<LabelID>(dir / "csr_edge_labels.bin", false);
  g.Validate();
  return g;
}

void CSRTopology::SaveBin(const std::filesystem::path& dir) const {
  std::filesystem::create_directories(dir);
  WriteArray(dir / "csr_offsets.bin", offsets);
  WriteArray(dir / "csr_indices.bin", indices);
  if (has_edge_labels()) {
    WriteArray(dir / "csr_edge_labels.bin", edge_labels);
  }
}

void CSRTopology::Print(std::ostream& os) const {
  os << "CSRTopology: |V|=" << num_vertices() << " |E|=" << num_edges()
     << (has_edge_labels() ? " (edge-labeled)" : "") << "\n";
  for (std::size_t v = 0; v < num_vertices(); ++v) {
    os << "  " << v << " ->";
    for (VertexID dst : neighbors(static_cast<VertexID>(v))) {
      os << " " << dst;
    }
    os << "\n";
  }
}

}  // namespace gar
