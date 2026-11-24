#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/data_structures/gar_graph_arrays.h"

namespace gar::core::graph {
class Graph;
}  // namespace gar::core::graph

namespace gar::core::graph {
// Defined in core/bridge/graph_loader.h; forward-declared here so that
// Graph can friend it (the loader fills the vertex-label fields
// directly after building the graph).
gar::core::graph::Graph LoadGraph(
    const std::filesystem::path& immutable_csr_dir, bool with_inverses);
}  // namespace gar::core::graph

namespace gar::core::graph {

// Graph: an in-memory triple store (src, relation, dst) for rule
// mining and reasoning. Entities and relations are dictionary-encoded to
// uint32; inverse relations (id + num_relations) are materialized so chain
// rules can walk edges in both directions, following AMIE/AnyBURL practice.
//
// Input directory layout (produced by tools/prepare_finreflectkg.py):
//   edges.bin       uint32 triples, [src, rel, dst] * m
//   entities.tsv    id \t entity_name
//   relations.tsv   id \t relation_name
//
// Loading from a MatrixGraph ImmutableCSR directory lives in the bridge
// module (gar/graph/graph_loader.h), keeping this header free of any
// MatrixGraph layout knowledge.
inline std::uint64_t PackPair(std::uint32_t src, std::uint32_t dst) {
  return (static_cast<std::uint64_t>(src) << 32) | dst;
}

class Graph {
 private:
  struct Base {
    std::uint32_t num_entities = 0;
    std::uint32_t num_relations = 0;
    std::uint64_t num_edges = 0;
    std::vector<std::string> entity_names;
    std::vector<std::string> relation_names;
    std::vector<std::vector<std::uint64_t>> rel_pairs;
    std::vector<std::unordered_set<std::uint64_t>> rel_pair_sets;
    std::vector<std::uint32_t> all_out_offsets;
    std::vector<std::uint32_t> all_out_dsts;
    std::vector<std::uint32_t> all_out_slots;
    std::vector<std::uint32_t> array_src;
    std::vector<std::uint32_t> array_dst;
    std::vector<std::int32_t> array_labels;
    sics::matrixgraph::core::data_structures::GARGraphArrays arrays{};
  };

  struct SlotDelta {
    std::unordered_set<std::uint64_t> added;
    std::unordered_set<std::uint64_t> removed;
  };

 public:
  Graph() : base_(std::make_shared<Base>()) {}
  Graph(const Graph& other) { CopyFrom(other); }
  Graph(Graph&& other) noexcept = default;
  Graph& operator=(const Graph& other) {
    if (this != &other) CopyFrom(other);
    return *this;
  }
  Graph& operator=(Graph&& other) noexcept = default;

  static Graph Load(const std::filesystem::path& dir,
                    bool with_inverses = true);

  // Builds the index structures from dictionary-encoded triples
  // [src, rel, dst] * m. Shared by all loaders.
  static Graph Build(std::vector<std::string> entity_names,
                     std::vector<std::string> relation_names,
                     std::vector<std::uint32_t> triples,
                     bool with_inverses);

  [[nodiscard]] std::uint32_t num_entities() const {
    return base_->num_entities;
  }
  [[nodiscard]] std::uint32_t num_relations() const {
    return base_->num_relations;  // excludes inverses
  }
  [[nodiscard]] std::uint32_t num_slots() const {
    return static_cast<std::uint32_t>(base_->rel_pairs.size());
  }
  [[nodiscard]] std::uint64_t num_edges() const { return num_edges_; }

  [[nodiscard]] bool IsInverse(std::uint32_t rel) const {
    return rel >= num_relations();
  }
  [[nodiscard]] std::uint32_t BaseRelation(std::uint32_t rel) const {
    return IsInverse(rel) ? rel - num_relations() : rel;
  }

  // Sorted unique packed (src,dst) pairs of a relation slot. The view is the
  // shared base plus this Graph's additions, with its tombstones applied.
  [[nodiscard]] const std::vector<std::uint64_t>& RelationPairs(
      std::uint32_t rel) const {
    EnsureSlotView(rel);
    if (!slot_views_valid_[rel]) return base_->rel_pairs[rel];
    auto& pairs = rel_pairs_[rel];
    const std::size_t prefix = slot_sorted_len_[rel];
    if (prefix < pairs.size()) {
      std::sort(pairs.begin() + prefix, pairs.end());
      std::inplace_merge(pairs.begin(), pairs.begin() + prefix, pairs.end());
      slot_sorted_len_[rel] = pairs.size();
    }
    return pairs;
  }
  // Hash view of the same pairs, for O(1) membership tests.
  [[nodiscard]] const std::unordered_set<std::uint64_t>& RelationPairSet(
      std::uint32_t rel) const {
    EnsureSlotView(rel);
    return slot_views_valid_[rel] ? rel_pair_sets_[rel]
                                  : base_->rel_pair_sets[rel];
  }

  // Out-neighbors of v via relation slot rel. RelationPairs is sorted by
  // (src,dst), so the slice is a binary search away.
  //
  // The destination ids are copied into a thread_local buffer: the returned
  // slice is invalidated by the next OutNeighbors call on the same thread.
  [[nodiscard]] std::pair<const std::uint32_t*, std::size_t> OutNeighbors(
      std::uint32_t v, std::uint32_t rel) const {
    const auto& pairs = RelationPairs(rel);
    thread_local std::vector<std::uint32_t> buf;
    buf.clear();
    for (auto it = std::lower_bound(pairs.begin(), pairs.end(), PackPair(v, 0));
         it != pairs.end() && (*it >> 32) == v; ++it) {
      buf.push_back(static_cast<std::uint32_t>(*it));
    }
    return {buf.data(), buf.size()};
  }

  // All out-edges of v across every slot, as parallel (dst, slot) slices
  // sorted by dst. Unmodified graphs read the shared base adjacency directly.
  // A Graph with a delta builds its own adjacency lazily after AddEdges, or
  // eagerly after AddEdgesBatch and RemoveEdgesBatch.
  // Mutation-while-reading is unsupported, as before.
  [[nodiscard]] std::pair<std::size_t, std::size_t> OutEdgeRange(
      std::uint32_t v) const {
    EnsureOutAdjacency();
    const auto& offsets =
        has_delta_ ? all_out_offsets_ : base_->all_out_offsets;
    return {offsets[v], offsets[v + 1]};
  }
  [[nodiscard]] const std::uint32_t* OutEdgeDsts() const {
    EnsureOutAdjacency();
    return has_delta_ ? all_out_dsts_.data() : base_->all_out_dsts.data();
  }
  [[nodiscard]] const std::uint32_t* OutEdgeSlots() const {
    EnsureOutAdjacency();
    return has_delta_ ? all_out_slots_.data() : base_->all_out_slots.data();
  }

  // Counts, for each relation slot, how many of `pairs` are also linked by
  // that slot — i.e. head support for every candidate head in one pass.
  template <typename PairContainer>
  void CountHeads(const PairContainer& pairs,
                  std::unordered_map<std::uint32_t, std::uint64_t>* out) const {
    for (std::uint64_t p : pairs) {
      const auto s = static_cast<std::uint32_t>(p >> 32);
      const auto d = static_cast<std::uint32_t>(p);
      const auto [b, e] = OutEdgeRange(s);
      const std::uint32_t* dsts = OutEdgeDsts();
      const std::uint32_t* slots = OutEdgeSlots();
      auto lo = std::lower_bound(dsts + b, dsts + e, d);
      for (auto it = lo; it != dsts + e && *it == d; ++it) {
        ++(*out)[slots[it - dsts]];
      }
    }
  }

  // Adds edges (src, dst) to this Graph's delta, mirroring each into the
  // inverse slot when the graph was built with inverses. Existing edges are
  // skipped. Unified out-adjacency is rebuilt lazily on its next read.
  // Returns the number of genuinely new edges, including inverse mirrors.
  std::uint64_t AddEdges(
      std::uint32_t slot,
      const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges);

  // Batch form: merges edges for several slots and rebuilds this Graph's
  // unified out-adjacency exactly once.
  std::uint64_t AddEdgesBatch(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);

  // Batch edge removal records tombstones without modifying the shared base.
  // Missing edges are skipped; unified out-adjacency is rebuilt once when
  // anything changed. Returns the number of edges actually removed.
  std::uint64_t RemoveEdgesBatch(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);

  [[nodiscard]] const std::string& EntityName(std::uint32_t id) const {
    return base_->entity_names[id];
  }
  [[nodiscard]] const std::string& RelationName(std::uint32_t rel) const {
    return base_->relation_names[rel];
  }
  // Display name; inverse relations are suffixed with "^-1".
  [[nodiscard]] std::string SlotName(std::uint32_t rel) const {
    return IsInverse(rel) ? base_->relation_names[rel - num_relations()] + "^-1"
                          : base_->relation_names[rel];
  }

  // Vertex labels (e.g. entity types), loaded from label/0.bin when present.
  // Only "active" when at least two distinct labels exist — a uniformly
  // labeled graph behaves exactly like an unlabeled one.
  [[nodiscard]] bool has_vertex_labels() const { return has_vertex_labels_; }
  [[nodiscard]] std::uint32_t VertexLabel(std::uint32_t v) const {
    return vertex_labels_[v];
  }
  [[nodiscard]] std::uint32_t num_labels() const { return num_labels_; }
  // Label display name; falls back to the numeric id when no dictionary.
  [[nodiscard]] std::string LabelName(std::uint32_t l) const {
    if (l < label_names_.size() && !label_names_[l].empty()) {
      return label_names_[l];
    }
    return "l" + std::to_string(l);
  }

  [[nodiscard]] sics::matrixgraph::core::data_structures::GARGraphArrays
  Arrays() const {
    auto arrays = base_->arrays;
    arrays.v_label_idx = vertex_labels_.empty()
                             ? nullptr
                             : reinterpret_cast<const std::int32_t*>(
                                   vertex_labels_.data());
    return arrays;
  }
  // Precomputed base adjacency (sorted per-slot pairs + hash sets), shared so
  // matchers can reuse it instead of rebuilding from raw edges.
  [[nodiscard]] const std::vector<std::vector<std::uint64_t>>&
  BaseRelPairs() const {
    return base_->rel_pairs;
  }
  [[nodiscard]] const std::vector<std::unordered_set<std::uint64_t>>&
  BaseRelPairSets() const {
    return base_->rel_pair_sets;
  }
  [[nodiscard]] std::uint64_t version() const { return version_; }

 private:
  // The core ImmutableCSR loader fills the vertex-label fields after Build().
  friend Graph LoadGraph(const std::filesystem::path&, bool);

  void CopyFrom(const Graph& other) {
    base_ = other.base_;
    num_edges_ = other.num_edges_;
    deltas_ = other.deltas_;
    has_delta_ = other.has_delta_;
    version_ = other.version_;
    vertex_labels_ = other.vertex_labels_;
    label_names_ = other.label_names_;
    num_labels_ = other.num_labels_;
    has_vertex_labels_ = other.has_vertex_labels_;
    ResetViews();
  }

  void ResetViews() {
    rel_pairs_.clear();
    rel_pairs_.resize(num_slots());
    rel_pair_sets_.clear();
    rel_pair_sets_.resize(num_slots());
    slot_views_valid_.assign(num_slots(), 0);
    slot_sorted_len_.assign(num_slots(), 0);
    all_out_offsets_.clear();
    all_out_dsts_.clear();
    all_out_slots_.clear();
    out_dirty_ = has_delta_;
  }

  void RebuildOutAdjacency();
  void EnsureSlotView(std::uint32_t rel) const;
  bool AddDeltaEdge(std::uint32_t slot, std::uint64_t pair);
  bool RemoveDeltaEdge(std::uint32_t slot, std::uint64_t pair);
  std::uint64_t MergeEdges(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);
  void EnsureOutAdjacency() const {
    if (has_delta_ && out_dirty_) {
      const_cast<Graph*>(this)->RebuildOutAdjacency();
    }
  }

  // The base is immutable after loading. Copies share it but own their delta
  // and all mutable views. CopyFrom deliberately discards materialized views.
  std::shared_ptr<const Base> base_;
  std::uint64_t num_edges_ = 0;
  std::vector<SlotDelta> deltas_;
  bool has_delta_ = false;

  // Per-slot merged views are built only for slots with a local delta.
  mutable std::vector<std::vector<std::uint64_t>> rel_pairs_;
  mutable std::vector<std::unordered_set<std::uint64_t>> rel_pair_sets_;
  mutable std::vector<char> slot_views_valid_;
  // Length of the sorted prefix of rel_pairs_[slot]. Delta edges are appended
  // unsorted in O(1); ordered access sorts only the appended suffix and merges
  // it into the sorted prefix (O(n + k log k)), instead of an O(n) sorted-vector
  // shift per fact (O(n^2)) or a full O(n log n) re-sort per batch. The hash view
  // rel_pair_sets_ stays eagerly maintained for O(1) membership (ContainsEdge).
  mutable std::vector<std::size_t> slot_sorted_len_;

  // Local unified adjacency, used only when has_delta_ is true.
  mutable std::vector<std::uint32_t> all_out_offsets_;
  mutable std::vector<std::uint32_t> all_out_dsts_;
  mutable std::vector<std::uint32_t> all_out_slots_;
  mutable bool out_dirty_ = false;

  // Optional vertex labels (size == num_entities when loaded).
  std::vector<std::uint32_t> vertex_labels_;
  std::vector<std::string> label_names_;
  std::uint32_t num_labels_ = 0;
  bool has_vertex_labels_ = false;

  std::uint64_t version_ = 0;
};

}  // namespace gar::core::graph
