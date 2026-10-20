#include "gar/graph/triple_graph.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace gar {
namespace graph {

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

TripleGraph TripleGraph::Load(const std::filesystem::path& dir,
                              bool with_inverses) {
  auto entity_names = ReadNames(dir / "entities.tsv");
  auto relation_names = ReadNames(dir / "relations.tsv");

  // ---- read triples ----
  std::ifstream in(dir / "edges.bin", std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("TripleGraph: cannot open " +
                             (dir / "edges.bin").string());
  }
  const auto bytes = static_cast<std::size_t>(in.tellg());
  if (bytes % (3 * sizeof(std::uint32_t)) != 0) {
    throw std::runtime_error("TripleGraph: edges.bin size is not a multiple "
                             "of 3*uint32");
  }
  std::vector<std::uint32_t> buf(bytes / sizeof(std::uint32_t));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(bytes));

  return Build(std::move(entity_names), std::move(relation_names),
               std::move(buf), with_inverses);
}

TripleGraph TripleGraph::Build(std::vector<std::string> entity_names,
                               std::vector<std::string> relation_names,
                               std::vector<std::uint32_t> buf,
                               bool with_inverses) {
  TripleGraph g;
  g.entity_names_ = std::move(entity_names);
  g.relation_names_ = std::move(relation_names);
  g.num_entities_ = static_cast<std::uint32_t>(g.entity_names_.size());
  g.num_relations_ = static_cast<std::uint32_t>(g.relation_names_.size());
  g.num_edges_ = buf.size() / 3;

  const std::uint32_t slots =
      with_inverses ? g.num_relations_ * 2 : g.num_relations_;

  // ---- per-slot pair sets ----
  g.rel_pairs_.resize(slots);
  for (std::size_t i = 0; i < buf.size(); i += 3) {
    const std::uint32_t s = buf[i], r = buf[i + 1], d = buf[i + 2];
    if (s >= g.num_entities_ || d >= g.num_entities_ ||
        r >= g.num_relations_) {
      throw std::runtime_error("TripleGraph: edge endpoint out of range");
    }
    g.rel_pairs_[r].push_back(PackPair(s, d));
    if (with_inverses) {
      g.rel_pairs_[r + g.num_relations_].push_back(PackPair(d, s));
    }
  }
  g.rel_pair_sets_.resize(slots);
  for (std::uint32_t r = 0; r < slots; ++r) {
    auto& v = g.rel_pairs_[r];
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    g.rel_pair_sets_[r].reserve(v.size() * 2);
    g.rel_pair_sets_[r].insert(v.begin(), v.end());
  }

  // ---- unified out-adjacency (dst, slot) sorted by dst per vertex ----
  g.RebuildOutAdjacency();
  return g;
}

void TripleGraph::RebuildOutAdjacency() {
  const std::uint32_t slots = static_cast<std::uint32_t>(rel_pairs_.size());
  all_out_offsets_.assign(num_entities_ + 1, 0);
  for (std::uint32_t r = 0; r < slots; ++r) {
    for (std::uint64_t p : rel_pairs_[r]) {
      all_out_offsets_[static_cast<std::uint32_t>(p >> 32) + 1]++;
    }
  }
  for (std::uint32_t v = 1; v <= num_entities_; ++v) {
    all_out_offsets_[v] += all_out_offsets_[v - 1];
  }
  all_out_dsts_.resize(all_out_offsets_.back());
  all_out_slots_.resize(all_out_offsets_.back());
  {
    std::vector<std::uint32_t> cursor(all_out_offsets_.begin(),
                                      all_out_offsets_.end());
    for (std::uint32_t r = 0; r < slots; ++r) {
      for (std::uint64_t p : rel_pairs_[r]) {
        const auto pos = cursor[static_cast<std::uint32_t>(p >> 32)]++;
        all_out_dsts_[pos] = static_cast<std::uint32_t>(p);
        all_out_slots_[pos] = r;
      }
    }
    for (std::uint32_t v = 0; v < num_entities_; ++v) {
      const auto b = all_out_offsets_[v], e = all_out_offsets_[v + 1];
      // sort (dst, slot) pairs by dst, keeping the two arrays aligned
      std::vector<std::pair<std::uint32_t, std::uint32_t>> tmp;
      tmp.reserve(e - b);
      for (auto i = b; i < e; ++i) {
        tmp.emplace_back(all_out_dsts_[i], all_out_slots_[i]);
      }
      std::sort(tmp.begin(), tmp.end());
      for (auto i = b; i < e; ++i) {
        all_out_dsts_[i] = tmp[i - b].first;
        all_out_slots_[i] = tmp[i - b].second;
      }
    }
  }
}

std::uint64_t TripleGraph::AddEdges(
    std::uint32_t slot,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges) {
  return AddEdgesBatch({{slot, edges}});
}

std::uint64_t TripleGraph::AddEdgesBatch(
    const std::unordered_map<
        std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
        by_slot) {
  const bool mirrored = rel_pairs_.size() > num_relations_;
  auto merge_into = [](std::vector<std::uint64_t>& pairs,
                       std::unordered_set<std::uint64_t>& set,
                       std::vector<std::uint64_t>& fresh) {
    if (fresh.empty()) return;
    std::sort(fresh.begin(), fresh.end());
    fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());
    std::vector<std::uint64_t> merged;
    merged.reserve(pairs.size() + fresh.size());
    std::merge(pairs.begin(), pairs.end(), fresh.begin(), fresh.end(),
               std::back_inserter(merged));
    pairs.swap(merged);
    set.insert(fresh.begin(), fresh.end());
  };

  std::uint64_t added = 0;
  for (const auto& [slot, edges] : by_slot) {
    if (slot >= rel_pairs_.size()) {
      throw std::runtime_error("TripleGraph::AddEdges: slot out of range");
    }
    const std::uint32_t inv_slot =
        slot < num_relations_ ? slot + num_relations_ : slot - num_relations_;
    std::vector<std::uint64_t> fresh, fresh_inv;
    for (const auto& [s, d] : edges) {
      if (s >= num_entities_ || d >= num_entities_) {
        throw std::runtime_error(
            "TripleGraph::AddEdges: endpoint out of range");
      }
      if (rel_pair_sets_[slot].count(PackPair(s, d))) continue;
      fresh.push_back(PackPair(s, d));
      if (mirrored) fresh_inv.push_back(PackPair(d, s));
    }
    if (fresh.empty()) continue;
    merge_into(rel_pairs_[slot], rel_pair_sets_[slot], fresh);
    if (mirrored) {
      merge_into(rel_pairs_[inv_slot], rel_pair_sets_[inv_slot], fresh_inv);
    }
    if (slot < num_relations_) num_edges_ += fresh.size();
    added += fresh.size();
  }
  if (added > 0) RebuildOutAdjacency();
  return added;
}

}  // namespace graph
}  // namespace gar
