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

  // has_edge(src, dst, slot) via the unified out-adjacency (sorted by dst).
  auto has_edge = [&](std::uint32_t src, std::uint32_t dst,
                      std::uint32_t slot) {
    const auto [b, e] = g_.OutEdgeRange(src);
    const std::uint32_t* dsts = g_.OutEdgeDsts();
    const std::uint32_t* slots = g_.OutEdgeSlots();
    auto lo = std::lower_bound(dsts + b, dsts + e, dst);
    for (auto it = lo; it != dsts + e && *it == dst; ++it) {
      if (slots[it - dsts] == slot) return true;
    }
    return false;
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

  std::vector<std::vector<std::uint32_t>> cands(n);
  std::vector<std::size_t> cand_pos(n, 0);

  for (std::uint64_t sp : seed_pairs) {
    const auto ps = static_cast<std::uint32_t>(sp >> 32);
    const auto pd = static_cast<std::uint32_t>(sp);
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
        candidates(back_edges[pos][0], &cands[pos]);
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
