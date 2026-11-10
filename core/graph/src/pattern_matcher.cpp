#include "gar/graph/pattern_matcher.h"

#include <algorithm>

namespace gar {
namespace graph {

namespace {

// Slot of the same data edges walked in the opposite direction.
std::uint32_t FlipSlot(std::uint32_t slot, std::uint32_t num_relations) {
  return slot < num_relations ? slot + num_relations : slot - num_relations;
}

}  // namespace

std::uint64_t PatternMatcher::Enumerate(const PatternSpec& p,
                                        std::uint64_t match_cap,
                                        std::vector<std::uint32_t>* out,
                                        bool* capped) const {
  return Enumerate(p, match_cap, out, capped, {});
}

namespace {

// nullptr when pattern vertex v is unpinned.
const std::uint32_t* PinOf(
    const std::unordered_map<std::uint32_t, std::uint32_t>& pins,
    std::uint32_t v) {
  const auto it = pins.find(v);
  return it == pins.end() ? nullptr : &it->second;
}

}  // namespace

std::uint64_t PatternMatcher::Enumerate(
    const PatternSpec& p, std::uint64_t match_cap,
    std::vector<std::uint32_t>* out, bool* capped,
    const std::unordered_map<std::uint32_t, std::uint32_t>& pins) const {
  *capped = false;
  const std::uint32_t n = p.num_vertices;
  const std::uint32_t m = static_cast<std::uint32_t>(p.edges.size());
  if (n == 0 || m == 0) return 0;
  const std::uint32_t R = g_.num_relations();

  // Undirected adjacency over pattern edges: (edge_idx, other, is_out, slot)
  // where is_out means the pattern edge leaves this vertex.
  struct Adj {
    std::uint32_t edge;
    std::uint32_t other;
    bool is_out;
    std::uint32_t slot;
  };
  std::vector<std::vector<Adj>> adj(n);
  for (std::uint32_t e = 0; e < m; ++e) {
    const auto [s, d, slot] = p.edges[e];
    adj[s].push_back({e, d, true, slot});
    adj[d].push_back({e, s, false, slot});
  }

  // Greedy matching order: seed with vertex 0, then repeatedly pick the
  // unbound vertex with the most edges into the bound set (ties: lower id).
  std::vector<std::uint32_t> order;
  order.reserve(n);
  {
    std::vector<char> bound(n, 0);
    bound[0] = 1;
    order.push_back(0);
    while (order.size() < n) {
      std::uint32_t best = n, best_back = 0;
      for (std::uint32_t v = 0; v < n; ++v) {
        if (bound[v]) continue;
        std::uint32_t back = 0;
        for (const auto& a : adj[v]) back += bound[a.other];
        if (back == 0) continue;  // keep the order connected
        if (back > best_back || (back == best_back && v < best)) {
          best = v;
          best_back = back;
        }
      }
      if (best == n) {  // disconnected remainder: bind by id
        for (std::uint32_t v = 0; v < n; ++v) {
          if (!bound[v]) {
            best = v;
            break;
          }
        }
      }
      bound[best] = 1;
      order.push_back(best);
    }
  }

  // Per ordered vertex (past the seed), the edges to earlier vertices; the
  // first one drives candidate generation, the rest are existence checks.
  std::vector<std::vector<Adj>> back_edges(n);
  {
    std::vector<char> earlier(n, 0);
    earlier[order[0]] = 1;
    for (std::size_t i = 1; i < n; ++i) {
      const std::uint32_t v = order[i];
      for (const auto& a : adj[v]) {
        if (earlier[a.other]) back_edges[i].push_back(a);
      }
      earlier[v] = 1;
    }
  }

  // Only connected patterns are supported: every ordered vertex past the
  // seed must have at least one edge to an earlier vertex.
  for (std::size_t i = 1; i < n; ++i) {
    if (back_edges[i].empty()) return 0;
  }

  // has_edge(src, dst, slot): O(1) point query on the slot's hash set
  // (always current — updated synchronously by AddEdges/AddEdgesBatch,
  // unlike the lazily rebuilt unified adjacency).
  auto has_edge = [&](std::uint32_t src, std::uint32_t dst,
                      std::uint32_t slot) {
    return g_.RelationPairSet(slot).count(PackPair(src, dst)) != 0;
  };

  std::vector<std::uint32_t> emb(n, 0);
  std::uint64_t count = 0;

  // Candidates for vertex `v` driven by its edge to a bound vertex `other`:
  // edge other -> v (is_out=false): scan the slot's pairs with src==other;
  // edge v -> other (is_out=true): scan the flipped slot's pairs, which are
  // packed reversed, again giving v's candidates as destinations.
  auto candidates = [&](const Adj& driver, std::vector<std::uint32_t>* out_c) {
    out_c->clear();
    const std::uint32_t slot =
        driver.is_out ? FlipSlot(driver.slot, R) : driver.slot;
    const std::uint32_t anchor = emb[driver.other];
    const auto& pairs = g_.RelationPairs(slot);
    for (auto it = std::lower_bound(pairs.begin(), pairs.end(),
                                    PackPair(anchor, 0));
         it != pairs.end() && (*it >> 32) == anchor; ++it) {
      out_c->push_back(static_cast<std::uint32_t>(*it));
    }
  };

  auto check_back_edges = [&](std::size_t pos) {
    const std::uint32_t v = order[pos];
    for (std::size_t i = 1; i < back_edges[pos].size(); ++i) {
      const auto& a = back_edges[pos][i];
      const std::uint32_t src = a.is_out ? emb[v] : emb[a.other];
      const std::uint32_t dst = a.is_out ? emb[a.other] : emb[v];
      if (!has_edge(src, dst, a.slot)) return false;
    }
    return true;
  };

  // Iterative backtracking over order positions 1..n-1. Position 0's vertex
  // is co-bound with position 1 through the seed edge (see below).
  if (n == 1) return 0;  // single-vertex patterns carry no edges; unsupported

  // Seed: the driver edge of order[1] connects it to order[0]; iterating the
  // (flipped, if needed) slot's pairs binds both endpoints at once. In both
  // directions the pair array is keyed by (emb[order[0]], emb[order[1]]):
  // edge order[0] -> order[1] uses the slot's own pairs; edge order[1] ->
  // order[0] uses the flipped slot's pairs (packed reversed).
  const Adj seed_adj = back_edges[1][0];  // connects order[1] to order[0]
  const std::uint32_t seed_slot =
      seed_adj.is_out ? FlipSlot(seed_adj.slot, R) : seed_adj.slot;
  const auto& seed_pairs = g_.RelationPairs(seed_slot);

  // Pin-aware seed scan: pairs are keyed (emb[order[0]], emb[order[1]]). A
  // pin on order[0] binary-searches the seed slot's sorted pairs; a pin on
  // order[1] alone switches to the flipped slot's mirrored pairs (sorted by
  // the same key after the swap).
  const std::uint32_t* pin0 = PinOf(pins, order[0]);
  const std::uint32_t* pin1 = PinOf(pins, order[1]);
  const std::vector<std::uint64_t>* scan = &seed_pairs;
  bool swapped = false;
  if (pin0 == nullptr && pin1 != nullptr) {
    scan = &g_.RelationPairs(FlipSlot(seed_slot, R));
    swapped = true;
  }
  std::size_t scan_lo = 0, scan_hi = scan->size();
  if (const std::uint32_t* pp = swapped ? pin1 : pin0) {
    const auto& arr = *scan;
    scan_lo = static_cast<std::size_t>(
        std::lower_bound(arr.begin(), arr.end(), PackPair(*pp, 0)) -
        arr.begin());
    scan_hi = static_cast<std::size_t>(
        std::lower_bound(arr.begin(), arr.end(), PackPair(*pp + 1, 0)) -
        arr.begin());
  }

  std::vector<std::vector<std::uint32_t>> cands(n);
  std::vector<std::size_t> cand_pos(n, 0);

  for (std::size_t si = scan_lo; si < scan_hi; ++si) {
    const std::uint64_t sp = (*scan)[si];
    const auto hi = static_cast<std::uint32_t>(sp >> 32);
    const auto lo = static_cast<std::uint32_t>(sp);
    const auto ps = swapped ? lo : hi;
    const auto pd = swapped ? hi : lo;
    if (pin0 != nullptr && ps != *pin0) continue;
    if (pin1 != nullptr && pd != *pin1) continue;
    emb[order[0]] = ps;
    emb[order[1]] = pd;
    // Remaining seed-side edges between order[0] and order[1] (multi-edges).
    bool ok = true;
    for (const auto& a : adj[order[1]]) {
      if (a.other == order[0] && a.edge != seed_adj.edge) {
        const std::uint32_t src = a.is_out ? emb[order[1]] : emb[order[0]];
        const std::uint32_t dst = a.is_out ? emb[order[0]] : emb[order[1]];
        if (!has_edge(src, dst, a.slot)) {
          ok = false;
          break;
        }
      }
    }
    if (!ok) continue;

    // Backtracking over positions 2..n-1 (positions 0 and 1 are bound by the
    // seed pair above). pos drops to 1 when the subtree is exhausted.
    std::size_t pos = 2;
    while (pos >= 2) {
      if (pos == n) {
        out->insert(out->end(), emb.begin(), emb.end());
        if (++count >= match_cap) {
          *capped = true;
          return count;
        }
        --pos;  // advance the last bound vertex
        continue;
      }
      if (cand_pos[pos] == 0) {
        if (const std::uint32_t* pp = PinOf(pins, order[pos])) {
          // Pinned vertex: the driver edge must actually exist between the
          // pin and the anchor (the driver is never re-checked by
          // check_back_edges, so validate it here).
          const Adj& driver = back_edges[pos][0];
          const std::uint32_t src = driver.is_out ? *pp : emb[driver.other];
          const std::uint32_t dst = driver.is_out ? emb[driver.other] : *pp;
          cands[pos].clear();
          if (has_edge(src, dst, driver.slot)) cands[pos].push_back(*pp);
        } else {
          candidates(back_edges[pos][0], &cands[pos]);
        }
      }
      bool advanced = false;
      while (cand_pos[pos] < cands[pos].size()) {
        emb[order[pos]] = cands[pos][cand_pos[pos]++];
        if (check_back_edges(pos)) {
          advanced = true;
          break;
        }
      }
      if (advanced) {
        ++pos;
        if (pos < n) cand_pos[pos] = 0;
      } else {
        cand_pos[pos] = 0;
        cands[pos].clear();
        --pos;
      }
    }
    if (count >= match_cap) {
      *capped = true;
      return count;
    }
  }
  return count;
}

}  // namespace graph
}  // namespace gar
