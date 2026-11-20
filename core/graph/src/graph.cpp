#include "gar/core/graph/graph.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace gar::core::graph {

namespace {

std::vector<std::string> ReadNames(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Graph: cannot open " + path.string());
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

template <typename GetPairs>
void BuildOutAdjacency(std::uint32_t num_entities, std::uint32_t slots,
                       GetPairs&& get_pairs,
                       std::vector<std::uint32_t>* offsets,
                       std::vector<std::uint32_t>* dsts,
                       std::vector<std::uint32_t>* out_slots) {
  offsets->assign(num_entities + 1, 0);
  for (std::uint32_t r = 0; r < slots; ++r) {
    for (std::uint64_t p : get_pairs(r)) {
      (*offsets)[static_cast<std::uint32_t>(p >> 32) + 1]++;
    }
  }
  for (std::uint32_t v = 1; v <= num_entities; ++v) {
    (*offsets)[v] += (*offsets)[v - 1];
  }
  dsts->resize(offsets->back());
  out_slots->resize(offsets->back());
  std::vector<std::uint32_t> cursor(offsets->begin(), offsets->end());
  for (std::uint32_t r = 0; r < slots; ++r) {
    for (std::uint64_t p : get_pairs(r)) {
      const auto pos = cursor[static_cast<std::uint32_t>(p >> 32)]++;
      (*dsts)[pos] = static_cast<std::uint32_t>(p);
      (*out_slots)[pos] = r;
    }
  }
  for (std::uint32_t v = 0; v < num_entities; ++v) {
    const auto b = (*offsets)[v], e = (*offsets)[v + 1];
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tmp;
    tmp.reserve(e - b);
    for (auto i = b; i < e; ++i) {
      tmp.emplace_back((*dsts)[i], (*out_slots)[i]);
    }
    std::sort(tmp.begin(), tmp.end());
    for (auto i = b; i < e; ++i) {
      (*dsts)[i] = tmp[i - b].first;
      (*out_slots)[i] = tmp[i - b].second;
    }
  }
}

}  // namespace

Graph Graph::Load(const std::filesystem::path& dir, bool with_inverses) {
  auto entity_names = ReadNames(dir / "entities.tsv");
  auto relation_names = ReadNames(dir / "relations.tsv");

  std::ifstream in(dir / "edges.bin", std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("Graph: cannot open " +
                             (dir / "edges.bin").string());
  }
  const auto bytes = static_cast<std::size_t>(in.tellg());
  if (bytes % (3 * sizeof(std::uint32_t)) != 0) {
    throw std::runtime_error("Graph: edges.bin size is not a multiple "
                             "of 3*uint32");
  }
  std::vector<std::uint32_t> buf(bytes / sizeof(std::uint32_t));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(bytes));

  return Build(std::move(entity_names), std::move(relation_names),
               std::move(buf), with_inverses);
}

Graph Graph::Build(std::vector<std::string> entity_names,
                   std::vector<std::string> relation_names,
                   std::vector<std::uint32_t> buf, bool with_inverses) {
  auto base = std::make_shared<Base>();
  base->entity_names = std::move(entity_names);
  base->relation_names = std::move(relation_names);
  base->num_entities = static_cast<std::uint32_t>(base->entity_names.size());
  base->num_relations = static_cast<std::uint32_t>(base->relation_names.size());
  base->num_edges = buf.size() / 3;
  base->array_src.reserve(base->num_edges);
  base->array_dst.reserve(base->num_edges);
  base->array_labels.reserve(base->num_edges);

  const std::uint32_t slots =
      with_inverses ? base->num_relations * 2 : base->num_relations;
  base->rel_pairs.resize(slots);
  for (std::size_t i = 0; i < buf.size(); i += 3) {
    const std::uint32_t s = buf[i], r = buf[i + 1], d = buf[i + 2];
    if (s >= base->num_entities || d >= base->num_entities ||
        r >= base->num_relations) {
      throw std::runtime_error("Graph: edge endpoint out of range");
    }
    base->array_src.push_back(s);
    base->array_dst.push_back(d);
    base->array_labels.push_back(static_cast<std::int32_t>(r));
    base->rel_pairs[r].push_back(PackPair(s, d));
    if (with_inverses) {
      base->rel_pairs[r + base->num_relations].push_back(PackPair(d, s));
    }
  }
  base->rel_pair_sets.resize(slots);
  for (std::uint32_t r = 0; r < slots; ++r) {
    auto& v = base->rel_pairs[r];
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    base->rel_pair_sets[r].reserve(v.size() * 2);
    base->rel_pair_sets[r].insert(v.begin(), v.end());
  }

  BuildOutAdjacency(
      base->num_entities, slots,
      [&](std::uint32_t r) -> const std::vector<std::uint64_t>& {
        return base->rel_pairs[r];
      },
      &base->all_out_offsets, &base->all_out_dsts, &base->all_out_slots);
  base->arrays.n_vertices = static_cast<int>(base->num_entities);
  base->arrays.e_src = base->array_src.data();
  base->arrays.e_dst = base->array_dst.data();
  base->arrays.e_label_idx = base->array_labels.data();
  base->arrays.n_edges = static_cast<int>(base->array_src.size());

  Graph g;
  g.num_edges_ = base->num_edges;
  g.deltas_.resize(slots);
  g.base_ = std::move(base);
  g.ResetViews();
  return g;
}

void Graph::EnsureSlotView(std::uint32_t rel) const {
  const SlotDelta& delta = deltas_[rel];
  if (slot_views_valid_[rel] || (delta.added.empty() && delta.removed.empty())) {
    return;
  }

  std::vector<std::uint64_t> fresh(delta.added.begin(), delta.added.end());
  std::sort(fresh.begin(), fresh.end());
  const auto& base_pairs = base_->rel_pairs[rel];
  std::vector<std::uint64_t> visible;
  const std::vector<std::uint64_t>* existing = &base_pairs;
  if (!delta.removed.empty()) {
    visible.reserve(base_pairs.size());
    for (std::uint64_t pair : base_pairs) {
      if (!delta.removed.contains(pair)) visible.push_back(pair);
    }
    existing = &visible;
  }

  std::vector<std::uint64_t> merged;
  merged.reserve(existing->size() + fresh.size());
  std::merge(existing->begin(), existing->end(), fresh.begin(), fresh.end(),
             std::back_inserter(merged));
  rel_pairs_[rel].swap(merged);
  rel_pair_sets_[rel].clear();
  rel_pair_sets_[rel].reserve(rel_pairs_[rel].size() * 2);
  rel_pair_sets_[rel].insert(rel_pairs_[rel].begin(), rel_pairs_[rel].end());
  slot_views_valid_[rel] = 1;
}

bool Graph::AddDeltaEdge(std::uint32_t slot, std::uint64_t pair) {
  SlotDelta& delta = deltas_[slot];
  if (delta.added.contains(pair) ||
      (!delta.removed.contains(pair) &&
       base_->rel_pair_sets[slot].contains(pair))) {
    return false;
  }

  if (base_->rel_pair_sets[slot].contains(pair)) {
    delta.removed.erase(pair);
  } else {
    delta.added.insert(pair);
  }
  if (slot_views_valid_[slot]) {
    rel_pairs_[slot].insert(
        std::lower_bound(rel_pairs_[slot].begin(), rel_pairs_[slot].end(), pair),
        pair);
    rel_pair_sets_[slot].insert(pair);
  }
  has_delta_ = true;
  return true;
}

bool Graph::RemoveDeltaEdge(std::uint32_t slot, std::uint64_t pair) {
  SlotDelta& delta = deltas_[slot];
  const bool present =
      delta.added.contains(pair) ||
      (!delta.removed.contains(pair) &&
       base_->rel_pair_sets[slot].contains(pair));
  if (!present) return false;

  if (base_->rel_pair_sets[slot].contains(pair)) {
    delta.removed.insert(pair);
  } else {
    delta.added.erase(pair);
  }
  if (slot_views_valid_[slot]) {
    rel_pair_sets_[slot].erase(pair);
    auto& pairs = rel_pairs_[slot];
    pairs.erase(std::lower_bound(pairs.begin(), pairs.end(), pair));
  }
  has_delta_ = true;
  return true;
}

void Graph::RebuildOutAdjacency() {
  BuildOutAdjacency(
      num_entities(), num_slots(),
      [this](std::uint32_t r) -> const std::vector<std::uint64_t>& {
        return RelationPairs(r);
      },
      &all_out_offsets_, &all_out_dsts_, &all_out_slots_);
  out_dirty_ = false;
}

std::uint64_t Graph::AddEdges(
    std::uint32_t slot,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges) {
  const std::uint64_t added = MergeEdges({{slot, edges}});
  if (added > 0) {
    out_dirty_ = true;
    ++version_;
  }
  return added;
}

std::uint64_t Graph::AddEdgesBatch(
    const std::unordered_map<
        std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
        by_slot) {
  const std::uint64_t added = MergeEdges(by_slot);
  if (added > 0) {
    RebuildOutAdjacency();
    ++version_;
  }
  return added;
}

std::uint64_t Graph::MergeEdges(
    const std::unordered_map<
        std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
        by_slot) {
  const bool mirrored = num_slots() > num_relations();
  std::uint64_t added = 0;
  for (const auto& [slot, edges] : by_slot) {
    if (slot >= num_slots()) {
      throw std::runtime_error("Graph::AddEdges: slot out of range");
    }
    const std::uint32_t inv_slot =
        slot < num_relations() ? slot + num_relations() : slot - num_relations();
    for (const auto& [s, d] : edges) {
      if (s >= num_entities() || d >= num_entities()) {
        throw std::runtime_error("Graph::AddEdges: endpoint out of range");
      }
      if (!AddDeltaEdge(slot, PackPair(s, d))) continue;
      if (mirrored) AddDeltaEdge(inv_slot, PackPair(d, s));
      if (slot < num_relations()) ++num_edges_;
      ++added;
    }
  }
  return added;
}

std::uint64_t Graph::RemoveEdgesBatch(
    const std::unordered_map<
        std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
        by_slot) {
  const bool mirrored = num_slots() > num_relations();
  std::uint64_t removed = 0;
  for (const auto& [slot, edges] : by_slot) {
    if (slot >= num_slots()) {
      throw std::runtime_error("Graph::RemoveEdges: slot out of range");
    }
    const std::uint32_t inv_slot =
        slot < num_relations() ? slot + num_relations() : slot - num_relations();
    for (const auto& [s, d] : edges) {
      if (!RemoveDeltaEdge(slot, PackPair(s, d))) continue;
      if (mirrored) RemoveDeltaEdge(inv_slot, PackPair(d, s));
      if (slot < num_relations()) --num_edges_;
      ++removed;
    }
  }
  if (removed > 0) {
    RebuildOutAdjacency();
    ++version_;
  }
  return removed;
}

}  // namespace gar::core::graph
