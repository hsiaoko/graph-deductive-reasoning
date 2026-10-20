#include "gar/miner/pattern_miner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace gar {
namespace miner {

using graph::PackPair;
using graph::PatternMatcher;
using graph::PatternSpec;
using graph::TripleGraph;

namespace {

using Edge = std::array<std::uint32_t, 3>;  // (src, dst, slot)

// Slot of the same data edges walked in the opposite direction.
std::uint32_t FlipSlot(std::uint32_t slot, std::uint32_t num_relations) {
  return slot < num_relations ? slot + num_relations : slot - num_relations;
}

// Canonical form of a pattern: over all vertex permutations, map every edge
// (u,v,l) to (pi(u),pi(v),l), sort, and take the lexicographic minimum. The
// key is the minimum edge list serialized as raw uint32 triples.
struct Canonical {
  std::string key;
  std::vector<Edge> edges;  // canonically numbered, sorted
};

Canonical Canonicalize(std::uint32_t n, const std::vector<Edge>& edges) {
  std::vector<std::uint32_t> perm(n);
  std::iota(perm.begin(), perm.end(), 0);
  std::vector<Edge> best;
  bool first = true;
  do {
    std::vector<Edge> mapped;
    mapped.reserve(edges.size());
    for (const auto& e : edges) {
      mapped.push_back({perm[e[0]], perm[e[1]], e[2]});
    }
    std::sort(mapped.begin(), mapped.end());
    if (first || mapped < best) {
      best = mapped;
      first = false;
    }
  } while (std::next_permutation(perm.begin(), perm.end()));

  Canonical c;
  c.edges = std::move(best);
  c.key.reserve(c.edges.size() * sizeof(std::uint32_t) * 3);
  for (const auto& e : c.edges) {
    for (int i = 0; i < 3; ++i) {
      const char* p = reinterpret_cast<const char*>(&e[i]);
      c.key.append(p, sizeof(std::uint32_t));
    }
  }
  return c;
}

// Ordered vertex-pair key for the per-pattern pair sets.
std::uint32_t PairKey(std::uint32_t u, std::uint32_t v) { return u * 8 + v; }
// Head-set key: pair key in the high bits, base relation in the low bits.
std::uint64_t HeadKey(std::uint32_t u, std::uint32_t v, std::uint32_t rh) {
  return (static_cast<std::uint64_t>(PairKey(u, v)) << 32) | rh;
}

// Per-pattern counting result: distinct (emb[u],emb[v]) pivot projections per
// ordered vertex pair, plus the same projection restricted to pairs also
// linked by a base relation rh (head support).
struct EvalResult {
  bool capped = false;
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint64_t>> bodies;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> heads;
};

void InsertCapped(std::unordered_set<std::uint64_t>* set, std::uint64_t value,
                  std::size_t cap, bool* capped) {
  if (set->size() >= cap) {
    *capped = true;
    return;
  }
  set->insert(value);
}

// Head slots (base relations) between src -> dst, via the unified
// out-adjacency. Appends each base slot found to *out.
void SlotsBetween(const TripleGraph& g, std::uint32_t src, std::uint32_t dst,
                  std::vector<std::uint32_t>* out) {
  out->clear();
  const auto [b, e] = g.OutEdgeRange(src);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  const std::uint32_t* slots = g.OutEdgeSlots();
  auto lo = std::lower_bound(dsts + b, dsts + e, dst);
  for (auto it = lo; it != dsts + e && *it == dst; ++it) {
    const std::uint32_t s = slots[it - dsts];
    if (s < g.num_relations()) out->push_back(s);
  }
}

// Undirected pattern adjacency: for each vertex, (other, walk_slot) where
// walk_slot is the slot id as traversed from this vertex (flipped when the
// pattern edge points back).
std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> PatternAdj(
    std::uint32_t n, const std::vector<Edge>& edges, std::uint32_t R) {
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> adj(n);
  for (const auto& e : edges) {
    adj[e[0]].emplace_back(e[1], e[2]);
    adj[e[1]].emplace_back(e[0], FlipSlot(e[2], R));
  }
  return adj;
}

// All-pairs BFS distances on the undirected pattern; returns the maximal
// distance and collects the ordered pairs attaining it.
std::uint32_t PivotPairs(std::uint32_t n, const std::vector<Edge>& edges,
                         std::uint32_t R,
                         std::vector<std::pair<std::uint32_t, std::uint32_t>>*
                             out_pairs) {
  const auto adj = PatternAdj(n, edges, R);
  std::uint32_t dmax = 0;
  std::vector<std::vector<std::uint32_t>> dist(
      n, std::vector<std::uint32_t>(n, UINT32_MAX));
  for (std::uint32_t s = 0; s < n; ++s) {
    dist[s][s] = 0;
    std::vector<std::uint32_t> queue{s};
    for (std::size_t i = 0; i < queue.size(); ++i) {
      const std::uint32_t x = queue[i];
      for (const auto& [y, w] : adj[x]) {
        if (dist[s][y] == UINT32_MAX) {
          dist[s][y] = dist[s][x] + 1;
          dmax = std::max(dmax, dist[s][y]);
          queue.push_back(y);
        }
      }
    }
  }
  for (std::uint32_t u = 0; u < n; ++u) {
    for (std::uint32_t v = 0; v < n; ++v) {
      if (u != v && dist[u][v] == dmax) out_pairs->emplace_back(u, v);
    }
  }
  return dmax;
}

// ---- evaluation: single edge (exact, mirrors ChainMiner len-1) ----
EvalResult EvalSingleEdge(const TripleGraph& g, const PatternMinerConfig& cfg,
                          std::uint32_t slot) {
  EvalResult res;
  auto& b01 = res.bodies[PairKey(0, 1)];
  auto& b10 = res.bodies[PairKey(1, 0)];
  b01 = g.RelationPairSet(slot);
  for (std::uint64_t p : g.RelationPairs(slot)) {
    b10.insert(PackPair(static_cast<std::uint32_t>(p),
                        static_cast<std::uint32_t>(p >> 32)));
  }
  std::vector<std::uint32_t> rh_slots;
  for (std::uint64_t p : b01) {
    const auto a = static_cast<std::uint32_t>(p >> 32);
    const auto b = static_cast<std::uint32_t>(p);
    SlotsBetween(g, a, b, &rh_slots);
    for (std::uint32_t rh : rh_slots) {
      InsertCapped(&res.heads[HeadKey(0, 1, rh)], p, cfg.pair_cap,
                   &res.capped);
    }
    SlotsBetween(g, b, a, &rh_slots);
    for (std::uint32_t rh : rh_slots) {
      InsertCapped(&res.heads[HeadKey(1, 0, rh)], PackPair(b, a), cfg.pair_cap,
                   &res.capped);
    }
  }
  return res;
}

// ---- evaluation: 2-edge path (exact, mirrors ChainMiner::ChainPairs) ----
//
// The walk slots (t0,t1) along the path reproduce the chain (t0,t1) of
// ChainMiner, including the iteration order and both caps, so capped counts
// agree bit-for-bit.
EvalResult EvalPath2(const TripleGraph& g, const PatternMinerConfig& cfg,
                     const std::vector<Edge>& edges) {
  const std::uint32_t R = g.num_relations();
  // Endpoints and walk slots from the lower-id endpoint.
  std::uint32_t deg[3] = {0, 0, 0};
  for (const auto& e : edges) {
    ++deg[e[0]];
    ++deg[e[1]];
  }
  std::uint32_t w0 = 0;
  for (std::uint32_t v = 0; v < 3; ++v) {
    if (deg[v] == 1) {
      w0 = v;
      break;
    }
  }
  std::uint32_t walk[3];
  std::uint32_t t[2];
  walk[0] = w0;
  {
    std::uint32_t cur = w0, prev = UINT32_MAX;
    for (int step = 0; step < 2; ++step) {
      for (const auto& e : edges) {
        std::uint32_t other = UINT32_MAX, slot = 0;
        if (e[0] == cur && e[1] != prev) {
          other = e[1];
          slot = e[2];
        } else if (e[1] == cur && e[0] != prev) {
          other = e[0];
          slot = FlipSlot(e[2], R);
        }
        if (other != UINT32_MAX) {
          t[step] = slot;
          prev = cur;
          cur = other;
          walk[step + 1] = cur;
          break;
        }
      }
    }
  }

  EvalResult res;
  auto& endpoint_pairs = res.bodies[PairKey(walk[0], walk[2])];
  auto& adj01 = res.bodies[PairKey(walk[0], walk[1])];
  auto& adj12 = res.bodies[PairKey(walk[1], walk[2])];

  // Same loop as ChainMiner::ChainPairs(len=2): join t0 pairs with
  // out-neighbors of the middle vertex via t1, dedupe (s,w).
  std::uint64_t attempts = 0;
  for (std::uint64_t p : g.RelationPairs(t[0])) {
    const auto m = static_cast<std::uint32_t>(p);
    const auto s = static_cast<std::uint32_t>(p >> 32);
    const auto [nb, cnt] = g.OutNeighbors(m, t[1]);
    attempts += cnt;
    if (attempts > cfg.join_attempt_cap) {
      res.capped = true;
      goto join_done;
    }
    for (std::size_t i = 0; i < cnt; ++i) {
      endpoint_pairs.insert(PackPair(s, nb[i]));
      InsertCapped(&adj01, PackPair(s, m), cfg.pair_cap, &res.capped);
      InsertCapped(&adj12, PackPair(m, nb[i]), cfg.pair_cap, &res.capped);
      if (endpoint_pairs.size() >= cfg.pair_cap) {
        res.capped = true;
        goto join_done;
      }
    }
  }
join_done:;

  // Flipped orientations of the three pair projections.
  for (std::uint32_t pk : {PairKey(walk[0], walk[2]), PairKey(walk[0], walk[1]),
                           PairKey(walk[1], walk[2])}) {
    const auto& src_set = res.bodies[pk];
    const std::uint32_t u = pk / 8, v = pk % 8;
    auto& dst_set = res.bodies[PairKey(v, u)];
    for (std::uint64_t p : src_set) {
      InsertCapped(&dst_set,
                   PackPair(static_cast<std::uint32_t>(p),
                            static_cast<std::uint32_t>(p >> 32)),
                   cfg.pair_cap, &res.capped);
    }
  }

  // Head support on the two endpoint orientations (the pivot pairs).
  std::vector<std::uint32_t> rh_slots;
  for (std::uint64_t p : endpoint_pairs) {
    const auto a = static_cast<std::uint32_t>(p >> 32);
    const auto b = static_cast<std::uint32_t>(p);
    SlotsBetween(g, a, b, &rh_slots);
    for (std::uint32_t rh : rh_slots) {
      InsertCapped(&res.heads[HeadKey(walk[0], walk[2], rh)], p, cfg.pair_cap,
                   &res.capped);
    }
    SlotsBetween(g, b, a, &rh_slots);
    for (std::uint32_t rh : rh_slots) {
      InsertCapped(&res.heads[HeadKey(walk[2], walk[0], rh)], PackPair(b, a),
                   cfg.pair_cap, &res.capped);
    }
  }
  return res;
}

// ---- evaluation: generic topology via PatternMatcher ----
EvalResult EvalGeneric(const TripleGraph& g, const PatternMinerConfig& cfg,
                       const PatternMatcher& matcher, std::uint32_t n,
                       const std::vector<Edge>& edges,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>&
                           pivot_pairs) {
  EvalResult res;
  PatternSpec spec;
  spec.num_vertices = n;
  spec.edges = edges;
  std::vector<std::uint32_t> embs;
  const std::uint64_t n_emb =
      matcher.Enumerate(spec, cfg.match_cap, &embs, &res.capped);

  for (std::uint64_t i = 0; i < n_emb; ++i) {
    const std::uint32_t* emb = embs.data() + i * n;
    for (std::uint32_t u = 0; u < n; ++u) {
      for (std::uint32_t v = 0; v < n; ++v) {
        if (u == v) continue;
        InsertCapped(&res.bodies[PairKey(u, v)], PackPair(emb[u], emb[v]),
                     cfg.pair_cap, &res.capped);
      }
    }
  }
  std::vector<std::uint32_t> rh_slots;
  for (std::uint64_t i = 0; i < n_emb; ++i) {
    const std::uint32_t* emb = embs.data() + i * n;
    for (const auto& [u, v] : pivot_pairs) {
      SlotsBetween(g, emb[u], emb[v], &rh_slots);
      for (std::uint32_t rh : rh_slots) {
        InsertCapped(&res.heads[HeadKey(u, v, rh)], PackPair(emb[u], emb[v]),
                     cfg.pair_cap, &res.capped);
      }
    }
  }
  return res;
}

}  // namespace

PatternMiner::PatternMiner(const TripleGraph& graph, PatternMinerConfig config)
    : g_(graph), cfg_(std::move(config)) {}

std::vector<GARule> PatternMiner::Mine() {
  const std::uint32_t R = g_.num_relations();
  const std::uint32_t slots = g_.num_slots();
  std::vector<GARule> rules;
  const PatternMatcher matcher(g_);

  // Frequency pre-filter (exact), same as ChainMiner: a body's pivot pair
  // count is bounded by its smallest slot, so rare slots never reach
  // min_support — neither in a body nor in a head.
  std::vector<std::uint32_t> freq_slots;
  for (std::uint32_t r = 0; r < slots; ++r) {
    if (g_.RelationPairs(r).size() >= cfg_.min_support) {
      freq_slots.push_back(r);
    }
  }

  // Slot co-occurrence index over frequent slots: for each vertex the
  // incident (slot, dir) keys (dir 1 = edge leaves v, 0 = edge enters v),
  // deduplicated, with every ordered pair registered. A new edge at pattern
  // vertex u must co-occur with every edge already incident to u (necessary
  // condition witnessed by any real match of u).
  std::unordered_set<std::uint64_t> cooc;
  {
    std::vector<std::vector<std::uint32_t>> inc(g_.num_entities());
    for (std::uint32_t r : freq_slots) {
      for (std::uint64_t p : g_.RelationPairs(r)) {
        inc[static_cast<std::uint32_t>(p >> 32)].push_back(r * 2 + 1);
        inc[static_cast<std::uint32_t>(p)].push_back(r * 2 + 0);
      }
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::mutex cooc_mu;
    auto gen = [&](std::uint32_t begin, std::uint32_t end) {
      std::unordered_set<std::uint64_t> local;
      for (std::uint32_t v = begin; v < end; ++v) {
        auto& keys = inc[v];
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        for (std::uint32_t a : keys) {
          for (std::uint32_t b : keys) {
            if (a != b) local.insert(PackPair(a, b));
          }
        }
      }
      std::lock_guard<std::mutex> lk(cooc_mu);
      cooc.insert(local.begin(), local.end());
    };
    std::vector<std::thread> threads;
    const std::uint32_t nv = g_.num_entities();
    const auto chunk = (nv + hw - 1) / hw;
    for (unsigned t = 0; t < hw; ++t) {
      const std::uint32_t begin = t * chunk;
      const std::uint32_t end = std::min(nv, begin + chunk);
      if (begin < end) threads.emplace_back(gen, begin, end);
    }
    for (auto& t : threads) t.join();
  }

  // Incident (slot, dir) keys of a pattern vertex, for co-occurrence checks.
  auto incident_keys = [&](const Pattern& p, std::uint32_t u) {
    std::vector<std::uint32_t> keys;
    for (const auto& e : p.edges) {
      if (e[0] == u) keys.push_back(e[2] * 2 + 1);
      if (e[1] == u) keys.push_back(e[2] * 2 + 0);
    }
    return keys;
  };
  auto cooc_ok = [&](const std::vector<std::uint32_t>& inc_keys,
                     std::uint32_t new_key) {
    for (std::uint32_t ik : inc_keys) {
      if (!cooc.count(PackPair(ik, new_key))) return false;
    }
    return true;
  };

  // Rule emission for one evaluated pattern; mirrors
  // ChainMiner::EmitRulesForChain (thresholds, tautology, head frequency).
  auto emit_rules = [&](const Pattern& p, const EvalResult& res,
                        const std::vector<std::pair<std::uint32_t, std::uint32_t>>&
                            pivot_pairs,
                        std::vector<GARule>* out) {
    for (const auto& [u, v] : pivot_pairs) {
      const auto bit = res.bodies.find(PairKey(u, v));
      if (bit == res.bodies.end()) continue;
      const auto body_size = bit->second.size();
      if (body_size < cfg_.min_support) continue;  // supp(rule) <= supp(body)

      for (const auto& [hk, hset] : res.heads) {
        if (static_cast<std::uint32_t>(hk >> 32) != PairKey(u, v)) continue;
        const auto rh = static_cast<std::uint32_t>(hk);
        // Tautology: the head restates an existing body edge — same
        // direction and slot, or the reverse edge with the inverse slot.
        bool tautology = false;
        for (const auto& e : p.edges) {
          if ((e[0] == u && e[1] == v && e[2] == rh) ||
              (e[0] == v && e[1] == u && e[2] == rh + R)) {
            tautology = true;
            break;
          }
        }
        if (tautology) continue;
        // supp(rule) <= |pairs(rh)|: skip heads that can never reach it.
        if (g_.RelationPairs(rh).size() < cfg_.min_support) continue;

        const std::uint64_t supp = hset.size();
        const double conf = static_cast<double>(supp) / body_size;
        if (supp < cfg_.min_support || conf < cfg_.min_confidence) continue;

        // Renumber vertices BFS-style from pivot u (walk order for chains,
        // reproducing ChainMiner's naming/CSR exactly on chain patterns).
        const auto adj = PatternAdj(p.n, p.edges, R);
        std::vector<std::uint32_t> new_id(p.n, UINT32_MAX);
        new_id[u] = 0;
        std::uint32_t nxt = 1;
        std::vector<std::uint32_t> queue{u};
        for (std::size_t i = 0; i < queue.size(); ++i) {
          const std::uint32_t x = queue[i];
          auto nbrs = adj[x];
          std::sort(nbrs.begin(), nbrs.end(),
                    [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                    });
          for (const auto& [y, w] : nbrs) {
            if (new_id[y] == UINT32_MAX) {
              new_id[y] = nxt++;
              queue.push_back(y);
            }
          }
        }
        // Normalize each edge to point from the lower to the higher new id,
        // flipping the slot when the edge is reversed.
        std::vector<Edge> norm;
        for (const auto& e : p.edges) {
          const std::uint32_t a = new_id[e[0]], b = new_id[e[1]];
          if (a < b) {
            norm.push_back({a, b, e[2]});
          } else {
            norm.push_back({b, a, FlipSlot(e[2], R)});
          }
        }
        std::sort(norm.begin(), norm.end());

        GARule rule;
        rule.name = "";
        for (std::size_t i = 0; i < norm.size(); ++i) {
          if (i) rule.name += " ∧ ";
          rule.name += g_.SlotName(norm[i][2]) + "(x" +
                       std::to_string(norm[i][0]) + ",x" +
                       std::to_string(norm[i][1]) + ")";
        }
        rule.name += " → " + g_.SlotName(rh) + "(x0,x" +
                     std::to_string(new_id[v]) + ")";

        std::vector<EdgeIndex> offsets(p.n + 1, 0);
        std::vector<VertexID> indices(norm.size());
        std::vector<LabelID> labels(norm.size());
        for (std::size_t i = 0; i < norm.size(); ++i) ++offsets[norm[i][0] + 1];
        for (std::uint32_t vtx = 1; vtx <= p.n; ++vtx) {
          offsets[vtx] += offsets[vtx - 1];
        }
        std::vector<EdgeIndex> cursor(offsets.begin(), offsets.end());
        for (std::size_t i = 0; i < norm.size(); ++i) {
          const auto pos = cursor[norm[i][0]]++;
          indices[pos] = norm[i][1];
          labels[pos] = norm[i][2];
        }
        rule.pattern = CSRTopology(std::move(offsets), std::move(indices),
                                   std::move(labels));
        rule.node_labels.assign(p.n, kWildcardLabel);
        rule.node_label_names.assign(p.n, "");
        rule.pivot = {0, new_id[v]};

        for (const auto& e : norm) rule.labels.emplace(g_.SlotName(e[2]), e[2]);
        rule.labels.emplace(g_.SlotName(rh), rh);

        Predicate head_pred;
        head_pred.kind = PredKind::kEdge;
        head_pred.src = 0;
        head_pred.dst = new_id[v];
        head_pred.label = rh;
        head_pred.label_name = g_.SlotName(rh);
        rule.consequent = head_pred;

        rule.quality.support = supp;
        rule.quality.confidence = conf;
        rule.quality.dataset = cfg_.dataset_root;
        rule.quality.miner = "pattern-miner-v1";

        out->push_back(std::move(rule));
      }
    }
  };

  // Candidate generation: extend every surviving pattern by one edge —
  // growth (to a fresh vertex, both directions) and closing (between two
  // existing non-adjacent vertices) — pruned by the co-occurrence index and
  // deduplicated by canonical form.
  auto gen_candidates = [&](const std::vector<Pattern>& survivors) {
    std::vector<Pattern> cands;
    std::unordered_set<std::string> seen;
    auto try_add = [&](std::uint32_t n, const std::vector<Edge>& edges) {
      auto canon = Canonicalize(n, edges);
      if (seen.insert(std::move(canon.key)).second) {
        cands.push_back(Pattern{n, std::move(canon.edges)});
      }
    };
    for (const auto& p : survivors) {
      const std::uint32_t n = p.n;
      // Growth.
      for (std::uint32_t u = 0; u < n; ++u) {
        const auto keys = incident_keys(p, u);
        for (std::uint32_t s : freq_slots) {
          if (cooc_ok(keys, s * 2 + 1)) {
            auto edges = p.edges;
            edges.push_back({u, n, s});
            try_add(n + 1, edges);
          }
          if (cooc_ok(keys, s * 2 + 0)) {
            auto edges = p.edges;
            edges.push_back({n, u, s});
            try_add(n + 1, edges);
          }
        }
      }
      // Closing (creates triangles etc. once k >= 2).
      if (p.edges.size() >= 2) {
        for (std::uint32_t u = 0; u < n; ++u) {
          for (std::uint32_t v = 0; v < n; ++v) {
            if (u == v) continue;
            bool adjacent = false;
            for (const auto& e : p.edges) {
              if ((e[0] == u && e[1] == v) || (e[0] == v && e[1] == u)) {
                adjacent = true;
                break;
              }
            }
            if (adjacent) continue;
            const auto keys_u = incident_keys(p, u);
            const auto keys_v = incident_keys(p, v);
            for (std::uint32_t s : freq_slots) {
              if (cooc_ok(keys_u, s * 2 + 1) && cooc_ok(keys_v, s * 2 + 0)) {
                auto edges = p.edges;
                edges.push_back({u, v, s});
                try_add(n, edges);
              }
            }
          }
        }
      }
    }
    return cands;
  };

  // Level-1 patterns: one per frequent slot.
  std::vector<Pattern> cur;
  for (std::uint32_t s : freq_slots) cur.push_back(Pattern{2, {{0, 1, s}}});

  std::mutex merge_mu;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint32_t k = 1; k <= cfg_.max_body_edges; ++k) {
    // Candidates are independent: evaluate them in parallel with an atomic
    // work-stealing counter, merging per-thread rules and stats at the end.
    std::vector<Pattern> survivors;
    std::atomic<std::uint64_t> next{0};
    const std::uint64_t n_items = cur.size();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned n_threads = static_cast<unsigned>(
        std::min<std::uint64_t>(hw, (n_items + 63) / 64 > 0
                                        ? (n_items + 63) / 64
                                        : 1));
    auto worker = [&]() {
      std::vector<GARule> local_rules;
      std::vector<Pattern> local_survivors;
      std::uint64_t local_capped = 0;
      for (;;) {
        const std::uint64_t begin = next.fetch_add(64);
        if (begin >= n_items) break;
        const std::uint64_t end = std::min(begin + 64, n_items);
        for (std::uint64_t i = begin; i < end; ++i) {
          const Pattern& p = cur[i];
          std::vector<std::pair<std::uint32_t, std::uint32_t>> pivots;
          PivotPairs(p.n, p.edges, R, &pivots);

          EvalResult res;
          if (p.edges.size() == 1) {
            res = EvalSingleEdge(g_, cfg_, p.edges[0][2]);
          } else if (p.edges.size() == 2) {
            res = EvalPath2(g_, cfg_, p.edges);
          } else {
            res = EvalGeneric(g_, cfg_, matcher, p.n, p.edges, pivots);
          }
          if (res.capped) ++local_capped;

          bool alive = false;
          for (const auto& [pk, set] : res.bodies) {
            if (set.size() >= cfg_.min_support) {
              alive = true;
              break;
            }
          }
          if (alive) local_survivors.push_back(p);
          if (p.edges.size() >= cfg_.min_body_edges) {
            emit_rules(p, res, pivots, &local_rules);
          }
        }
      }
      std::lock_guard<std::mutex> lk(merge_mu);
      std::move(local_rules.begin(), local_rules.end(),
                std::back_inserter(rules));
      std::move(local_survivors.begin(), local_survivors.end(),
                std::back_inserter(survivors));
      stats_.patterns_capped += local_capped;
    };
    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (unsigned t = 0; t < n_threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    stats_.patterns_evaluated += n_items;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    std::cerr << "[pattern_miner] level k=" << k << ": " << n_items
              << " patterns evaluated, " << survivors.size()
              << " survivors, " << rules.size() << " rules so far ("
              << elapsed << "s)\n";
    if (k == cfg_.max_body_edges) break;
    cur = gen_candidates(survivors);
    std::cerr << "[pattern_miner] level k=" << k + 1 << ": " << cur.size()
              << " candidates generated\n";
  }

  // The same oriented rule can be emitted from several (pattern, pivot)
  // combinations (e.g. a chain and its reverse walk are distinct canonical
  // patterns); the rule name fully determines body edges, pivot and head,
  // and duplicates carry identical counts, so dedupe by name.
  {
    std::unordered_set<std::string> seen;
    std::vector<GARule> deduped;
    deduped.reserve(rules.size());
    for (auto& r : rules) {
      if (seen.insert(r.name).second) deduped.push_back(std::move(r));
    }
    rules = std::move(deduped);
  }

  stats_.rules_emitted = rules.size();
  return rules;
}

}  // namespace miner
}  // namespace gar
