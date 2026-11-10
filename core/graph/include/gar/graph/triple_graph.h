#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gar::core::graph {
class TripleGraph;
}  // namespace gar::core::graph

namespace gar::core::bridge {
// Defined in core/bridge/graph_loader.h; forward-declared here so that
// TripleGraph can friend it (the loader fills the vertex-label fields
// directly after building the graph).
gar::core::graph::TripleGraph LoadTripleGraph(
    const std::filesystem::path& immutable_csr_dir, bool with_inverses);
}  // namespace gar::core::bridge

namespace gar::core::graph {

// TripleGraph: an in-memory triple store (src, relation, dst) for rule
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
// module (gar/bridge/graph_loader.h), keeping this header free of any
// MatrixGraph layout knowledge.
inline std::uint64_t PackPair(std::uint32_t src, std::uint32_t dst) {
  return (static_cast<std::uint64_t>(src) << 32) | dst;
}

class TripleGraph {
 public:
  static TripleGraph Load(const std::filesystem::path& dir,
                          bool with_inverses = true);

  // Builds the index structures from dictionary-encoded triples
  // [src, rel, dst] * m. Shared by all loaders.
  static TripleGraph Build(std::vector<std::string> entity_names,
                           std::vector<std::string> relation_names,
                           std::vector<std::uint32_t> triples,
                           bool with_inverses);

  [[nodiscard]] std::uint32_t num_entities() const { return num_entities_; }
  [[nodiscard]] std::uint32_t num_relations() const {
    return num_relations_;  // excludes inverses
  }
  [[nodiscard]] std::uint32_t num_slots() const {
    return static_cast<std::uint32_t>(rel_pairs_.size());  // incl. inverses
  }
  [[nodiscard]] std::uint64_t num_edges() const { return num_edges_; }

  [[nodiscard]] bool IsInverse(std::uint32_t rel) const {
    return rel >= num_relations_;
  }
  [[nodiscard]] std::uint32_t BaseRelation(std::uint32_t rel) const {
    return IsInverse(rel) ? rel - num_relations_ : rel;
  }

  // Sorted unique packed (src,dst) pairs of a relation slot.
  [[nodiscard]] const std::vector<std::uint64_t>& RelationPairs(
      std::uint32_t rel) const {
    return rel_pairs_[rel];
  }
  // Hash view of the same pairs, for O(1) membership tests.
  [[nodiscard]] const std::unordered_set<std::uint64_t>& RelationPairSet(
      std::uint32_t rel) const {
    return rel_pair_sets_[rel];
  }

  // Out-neighbors of v via relation slot rel. `rel_pairs_[rel]` is sorted by
  // (src,dst) — the same src-major layout a CSR would give — so the slice is
  // a binary search away; no per-slot offsets array is materialized (that
  // would cost slots × (n+1) × 4B, which is quadratic in the relation
  // vocabulary and infeasible for high-|R| KGs).
  //
  // The destination ids are copied into a thread_local buffer: the returned
  // slice is invalidated by the next OutNeighbors call on the same thread.
  [[nodiscard]] std::pair<const std::uint32_t*, std::size_t> OutNeighbors(
      std::uint32_t v, std::uint32_t rel) const {
    const auto& pairs = rel_pairs_[rel];
    thread_local std::vector<std::uint32_t> buf;
    buf.clear();
    for (auto it = std::lower_bound(pairs.begin(), pairs.end(), PackPair(v, 0));
         it != pairs.end() && (*it >> 32) == v; ++it) {
      buf.push_back(static_cast<std::uint32_t>(*it));
    }
    return {buf.data(), buf.size()};
  }

  // All out-edges of v across every slot, as parallel (dst, slot) slices
  // sorted by dst — one binary search finds the slots between v and any w.
  // The unified adjacency is LAZY: single-edge AddEdges (the agent's
  // per-derivation path) only updates the per-slot pair arrays/hashes and
  // defers the rebuild to the first unified-adjacency read, so every read
  // here always sees the latest edges. AddEdgesBatch (the chase's per-round
  // bulk merge) rebuilds eagerly, exactly once per round as before.
  // Mutation-while-reading was never supported and stays unsupported:
  // writers run outside parallel read sections in every caller.
  [[nodiscard]] std::pair<std::size_t, std::size_t> OutEdgeRange(
      std::uint32_t v) const {
    EnsureOutAdjacency();
    return {all_out_offsets_[v], all_out_offsets_[v + 1]};
  }
  [[nodiscard]] const std::uint32_t* OutEdgeDsts() const {
    EnsureOutAdjacency();
    return all_out_dsts_.data();
  }
  [[nodiscard]] const std::uint32_t* OutEdgeSlots() const {
    EnsureOutAdjacency();
    return all_out_slots_.data();
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
      const std::uint32_t* dsts = all_out_dsts_.data();
      auto lo = std::lower_bound(dsts + b, dsts + e, d);
      for (auto it = lo; it != dsts + e && *it == d; ++it) {
        ++(*out)[all_out_slots_[(it - dsts)]];
      }
    }
  }

  // Adds edges (src, dst) to relation slot `slot`, mirroring each into the
  // inverse slot when the graph was built with inverses. Existing edges are
  // skipped. The per-slot pair arrays and hash sets are updated immediately;
  // the unified out-adjacency is only marked dirty and rebuilt lazily on the
  // next unified-adjacency read (OutEdgeRange/OutEdgeDsts/OutEdgeSlots/
  // CountHeads) — per-derivation full-graph rebuilds were the agent's
  // dominant index cost. Returns the number of genuinely new edges.
  std::uint64_t AddEdges(
      std::uint32_t slot,
      const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges);

  // Batch form: merges edges for several slots and rebuilds the unified
  // out-adjacency exactly once. Strongly preferred over repeated AddEdges
  // calls (one chase round typically touches hundreds of slots).
  std::uint64_t AddEdgesBatch(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);

  // Batch edge removal (the hold-out operation of workload construction,
  // paper §7: "we held out X triples as target facts; a task takes one of
  // them as g and the rest of the graph as Γ0"): removes each (src, dst)
  // from the slot, mirroring the inverse slot when built with inverses;
  // missing edges are skipped; the unified out-adjacency is rebuilt once
  // when anything changed. Returns the number of edges actually removed.
  std::uint64_t RemoveEdgesBatch(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);

  [[nodiscard]] const std::string& EntityName(std::uint32_t id) const {
    return entity_names_[id];
  }
  [[nodiscard]] const std::string& RelationName(std::uint32_t rel) const {
    return relation_names_[rel];
  }
  // Display name; inverse relations are suffixed with "^-1".
  [[nodiscard]] std::string SlotName(std::uint32_t rel) const {
    return IsInverse(rel) ? relation_names_[rel - num_relations_] + "^-1"
                          : relation_names_[rel];
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

 private:
  // The bridge-module ImmutableCSR loader fills the vertex-label fields
  // after Build().
  friend TripleGraph gar::core::bridge::LoadTripleGraph(
      const std::filesystem::path&, bool);

  // Rebuilds all_out_* from rel_pairs_ (after bulk edge insertion).
  void RebuildOutAdjacency();
  // Shared merge core of AddEdges/AddEdgesBatch: updates rel_pairs_ and
  // rel_pair_sets_ (both slots' mirrors), returns the new-edge count.
  // Does NOT touch the unified out-adjacency (caller's contract).
  std::uint64_t MergeEdges(
      const std::unordered_map<
          std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>&
          by_slot);
  // Lazily rebuilds the unified out-adjacency when a single-slot AddEdges
  // marked it dirty. Called by every unified-adjacency accessor.
  void EnsureOutAdjacency() const {
    if (out_dirty_) {
      const_cast<TripleGraph*>(this)->RebuildOutAdjacency();
      out_dirty_ = false;
    }
  }

  std::uint32_t num_entities_ = 0;
  std::uint32_t num_relations_ = 0;  // base relations, excluding inverses
  std::uint64_t num_edges_ = 0;      // base edges, excluding inverses

  std::vector<std::string> entity_names_;
  std::vector<std::string> relation_names_;

  // Indexed by relation slot (base + inverses).
  std::vector<std::vector<std::uint64_t>> rel_pairs_;
  std::vector<std::unordered_set<std::uint64_t>> rel_pair_sets_;

  // Unified out-adjacency across all slots, sorted by dst per vertex.
  // Lazily rebuilt: `out_dirty_` is set by single-slot AddEdges and cleared
  // by EnsureOutAdjacency/RebuildOutAdjacency.
  std::vector<std::uint32_t> all_out_offsets_;  // n+1
  std::vector<std::uint32_t> all_out_dsts_;
  std::vector<std::uint32_t> all_out_slots_;
  mutable bool out_dirty_ = false;

  // Optional vertex labels (size == num_entities when loaded).
  std::vector<std::uint32_t> vertex_labels_;
  std::vector<std::string> label_names_;
  std::uint32_t num_labels_ = 0;
  bool has_vertex_labels_ = false;
};

}  // namespace gar::core::graph
