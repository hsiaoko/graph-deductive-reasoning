#include "gar/miner/chain_miner.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <thread>

namespace gar {
namespace miner {

ChainMiner::ChainMiner(const TripleGraph& graph, MinerConfig config)
    : g_(graph), cfg_(std::move(config)) {}

std::unordered_set<std::uint64_t> ChainMiner::ChainPairs(
    const std::uint32_t* chain, std::uint32_t len, bool* capped) const {
  *capped = false;
  std::unordered_set<std::uint64_t> pairs;

  if (len == 1) {
    const auto& v = g_.RelationPairs(chain[0]);
    pairs.insert(v.begin(), v.end());
    return pairs;
  }

  // len == 2: join r1(s,m) with out-neighbors of m via r2, dedupe (s,w).
  std::uint64_t attempts = 0;
  for (std::uint64_t p : g_.RelationPairs(chain[0])) {
    const auto m = static_cast<std::uint32_t>(p);
    const auto s = static_cast<std::uint32_t>(p >> 32);
    const auto [nb, cnt] = g_.OutNeighbors(m, chain[1]);
    attempts += cnt;
    if (attempts > cfg_.join_attempt_cap) {
      *capped = true;
      return pairs;
    }
    for (std::size_t i = 0; i < cnt; ++i) {
      pairs.insert(PackPair(s, nb[i]));
      if (pairs.size() >= cfg_.pair_cap) {
        *capped = true;
        return pairs;
      }
    }
  }
  return pairs;
}

ChainMiner::TypedPairSets ChainMiner::TypedChainPairs(
    const std::uint32_t* chain, std::uint32_t len, bool* capped) const {
  *capped = false;
  TypedPairSets buckets;
  std::size_t total = 0;

  if (len == 1) {
    for (std::uint64_t p : g_.RelationPairs(chain[0])) {
      const auto d = static_cast<std::uint32_t>(p);
      const auto s = static_cast<std::uint32_t>(p >> 32);
      auto& set = buckets[{g_.VertexLabel(s), g_.VertexLabel(d), 0}];
      if (set.insert(p).second) ++total;
      if (total >= cfg_.pair_cap) {
        *capped = true;
        break;
      }
    }
    return buckets;
  }

  // len == 2: join r1(s,m) with out-neighbors of m via r2, dedupe (s,w) per
  // signature bucket (label(s), label(m), label(w)).
  std::uint64_t attempts = 0;
  for (std::uint64_t p : g_.RelationPairs(chain[0])) {
    const auto m = static_cast<std::uint32_t>(p);
    const auto s = static_cast<std::uint32_t>(p >> 32);
    const auto [nb, cnt] = g_.OutNeighbors(m, chain[1]);
    attempts += cnt;
    if (attempts > cfg_.join_attempt_cap) {
      *capped = true;
      return buckets;
    }
    const TypeSig prefix{g_.VertexLabel(s), g_.VertexLabel(m), 0};
    for (std::size_t i = 0; i < cnt; ++i) {
      TypeSig sig = prefix;
      sig[2] = g_.VertexLabel(nb[i]);
      auto& set = buckets[sig];
      if (set.insert(PackPair(s, nb[i])).second) ++total;
      if (total >= cfg_.pair_cap) {
        *capped = true;
        return buckets;
      }
    }
  }
  return buckets;
}

void ChainMiner::EmitRulesForChain(
    const std::vector<std::uint32_t>& chain,
    const std::unordered_set<std::uint64_t>& body_pairs, const TypeSig* sig,
    std::vector<GARule>* out) {
  const auto body_size = body_pairs.size();
  if (body_size < cfg_.min_support) return;  // supp(rule) <= supp(body)

  // Head support for every relation slot in a single pass over body pairs.
  std::unordered_map<std::uint32_t, std::uint64_t> head_counts;
  g_.CountHeads(body_pairs, &head_counts);

  // Heads are restricted to base relations: r^-1(x0,xk) == r(xk,x0).
  for (const auto& [rh, supp] : head_counts) {
    if (rh >= g_.num_relations()) continue;  // inverse heads are redundant
    if (chain.size() == 1 && chain[0] == rh) continue;  // trivial identity
    // supp(rule) <= |pairs(rh)|: skip heads that can never reach min_support.
    if (g_.RelationPairs(rh).size() < cfg_.min_support) continue;

    const double conf = static_cast<double>(supp) / body_size;
    if (supp < cfg_.min_support || conf < cfg_.min_confidence) continue;

    const auto k = static_cast<VertexID>(chain.size());

    // Variable tag: "x0" plain, or "x0:drug" when a label signature applies.
    auto tag = [&](VertexID i) {
      std::string t = "x" + std::to_string(i);
      if (sig) t += ":" + g_.LabelName((*sig)[i]);
      return t;
    };

    GARule rule;
    rule.name = "";
    for (std::size_t i = 0; i < chain.size(); ++i) {
      if (i) rule.name += " ∧ ";
      rule.name += g_.SlotName(chain[i]) + "(" + tag(static_cast<VertexID>(i)) +
                   "," + tag(static_cast<VertexID>(i + 1)) + ")";
    }
    rule.name += " → " + g_.SlotName(rh) + "(" + tag(0) + "," + tag(k) + ")";

    // Pattern: chain x0 -> x1 -> ... -> xk with slot labels.
    std::vector<EdgeIndex> offsets(k + 2);
    std::vector<VertexID> indices(k);
    std::vector<LabelID> labels(k);
    for (VertexID i = 0; i <= k + 1; ++i) offsets[i] = std::min(i, k);
    for (VertexID i = 0; i < k; ++i) {
      indices[i] = i + 1;
      labels[i] = chain[i];
    }
    rule.pattern = CSRTopology(std::move(offsets), std::move(indices),
                               std::move(labels));
    rule.node_labels.assign(k + 1, kWildcardLabel);
    rule.node_label_names.assign(k + 1, "");
    if (sig) {
      for (VertexID i = 0; i <= k; ++i) {
        rule.node_labels[i] = (*sig)[i];
        rule.node_label_names[i] = g_.LabelName((*sig)[i]);
      }
    }
    rule.pivot = {0, k};

    // Label dictionary for the relations used by this rule, plus the vertex
    // label names of the type signature (node label names resolve through
    // the same dictionary on load).
    for (VertexID i = 0; i < k; ++i) {
      rule.labels.emplace(g_.SlotName(chain[i]), chain[i]);
    }
    rule.labels.emplace(g_.SlotName(rh), rh);
    if (sig) {
      for (VertexID i = 0; i <= k; ++i) {
        rule.labels.emplace(g_.LabelName((*sig)[i]), (*sig)[i]);
      }
    }

    Predicate head_pred;
    head_pred.kind = PredKind::kEdge;
    head_pred.src = 0;
    head_pred.dst = k;
    head_pred.label = rh;
    head_pred.label_name = g_.SlotName(rh);
    rule.consequent = head_pred;

    rule.quality.support = supp;
    rule.quality.confidence = conf;
    rule.quality.dataset = cfg_.dataset_root;
    rule.quality.miner = "chain-miner-v1";

    out->push_back(std::move(rule));
  }
}

std::vector<GARule> ChainMiner::Mine() {
  std::vector<GARule> rules;
  const std::uint32_t body_slots =
      cfg_.with_inverses ? g_.num_slots() : g_.num_relations();
  const bool typed = cfg_.use_vertex_labels && g_.has_vertex_labels();

  // Frequency pre-filter (exact): a chain's body pair count is bounded by
  // the smallest slot on it, so any slot with fewer pairs than min_support
  // can never reach the threshold — neither in a body (skip the chain, no
  // join work at all) nor in a head (skipped at emission).
  std::vector<char> slot_ok(body_slots, 0);
  std::vector<std::uint32_t> freq_slots;
  for (std::uint32_t r = 0; r < body_slots; ++r) {
    if (g_.RelationPairs(r).size() >= cfg_.min_support) {
      slot_ok[r] = 1;
      freq_slots.push_back(r);
    }
  }

  // Chains are independent: evaluate them in parallel with an atomic
  // work-stealing counter, merging per-thread rules and stats at the end.
  std::mutex merge_mu;
  auto run_parallel = [&](std::uint64_t n_items, auto&& body_fn) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned n_threads =
        static_cast<unsigned>(std::min<std::uint64_t>(
            hw, (n_items + 63) / 64 > 0 ? (n_items + 63) / 64 : 1));
    std::atomic<std::uint64_t> next{0};
    auto worker = [&]() {
      std::vector<GARule> local_rules;
      MinerStats local_stats;
      for (;;) {
        const std::uint64_t begin = next.fetch_add(64);
        if (begin >= n_items) break;
        const std::uint64_t end =
            std::min(begin + 64, static_cast<std::uint64_t>(n_items));
        for (std::uint64_t i = begin; i < end; ++i) {
          body_fn(i, &local_rules, &local_stats);
        }
      }
      std::lock_guard<std::mutex> lk(merge_mu);
      std::move(local_rules.begin(), local_rules.end(),
                std::back_inserter(rules));
      stats_.chains_evaluated += local_stats.chains_evaluated;
      stats_.chains_capped += local_stats.chains_capped;
      stats_.rules_emitted += local_stats.rules_emitted;
    };
    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (unsigned t = 0; t < n_threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
  };

  // ---- body length 1 ----
  if (cfg_.min_body_len <= 1) {
    run_parallel(freq_slots.size(), [&](std::uint64_t i,
                                        std::vector<GARule>* out,
                                        MinerStats* st) {
      ++st->chains_evaluated;
      const auto slot = freq_slots[i];
      if (typed) {
        bool capped = false;
        auto buckets = TypedChainPairs(&slot, 1, &capped);
        if (capped) ++st->chains_capped;
        for (const auto& [sig, set] : buckets) {
          EmitRulesForChain({slot}, set, &sig, out);
        }
      } else {
        EmitRulesForChain({slot}, g_.RelationPairSet(slot), nullptr, out);
      }
    });
  }

  if (cfg_.max_body_len >= 2) {
    // Candidate (r1,r2) chains: relation slots that co-occur as in/out at
    // some vertex. One pass over all slot pairs; infrequent slots are
    // excluded up front (they can never reach min_support).
    std::vector<std::vector<std::uint32_t>> in_slots(g_.num_entities());
    std::vector<std::vector<std::uint32_t>> out_slots(g_.num_entities());
    for (std::uint32_t r : freq_slots) {
      for (std::uint64_t p : g_.RelationPairs(r)) {
        out_slots[static_cast<std::uint32_t>(p >> 32)].push_back(r);
        in_slots[static_cast<std::uint32_t>(p)].push_back(r);
      }
    }
    std::unordered_set<std::uint64_t> candidates;
    {
      // A vertex appears once per incident pair, so its slot lists carry
      // duplicates; dedupe before the quadratic in×out enumeration, and
      // split the vertices across threads (local sets merged at the end).
      const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
      std::mutex cand_mu;
      auto gen = [&](std::uint32_t begin, std::uint32_t end) {
        std::unordered_set<std::uint64_t> local;
        for (std::uint32_t v = begin; v < end; ++v) {
          auto& in = in_slots[v];
          auto& out = out_slots[v];
          if (in.empty() || out.empty()) continue;
          std::sort(in.begin(), in.end());
          in.erase(std::unique(in.begin(), in.end()), in.end());
          std::sort(out.begin(), out.end());
          out.erase(std::unique(out.begin(), out.end()), out.end());
          for (std::uint32_t r1 : in) {
            for (std::uint32_t r2 : out) {
              local.insert(PackPair(r1, r2));
            }
          }
        }
        std::lock_guard<std::mutex> lk(cand_mu);
        candidates.insert(local.begin(), local.end());
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
    in_slots.clear();
    out_slots.clear();
    std::vector<std::uint64_t> cands(candidates.begin(), candidates.end());
    candidates.clear();

    run_parallel(cands.size(), [&](std::uint64_t i, std::vector<GARule>* out,
                                   MinerStats* st) {
      const std::uint64_t cand = cands[i];
      const auto r1 = static_cast<std::uint32_t>(cand >> 32);
      const auto r2 = static_cast<std::uint32_t>(cand);
      bool capped = false;
      const std::uint32_t chain[2] = {r1, r2};
      ++st->chains_evaluated;
      if (typed) {
        auto buckets = TypedChainPairs(chain, 2, &capped);
        if (capped) ++st->chains_capped;
        for (const auto& [sig, set] : buckets) {
          EmitRulesForChain({r1, r2}, set, &sig, out);
        }
      } else {
        auto pairs = ChainPairs(chain, 2, &capped);
        if (capped) ++st->chains_capped;
        EmitRulesForChain({r1, r2}, pairs, nullptr, out);
      }
    });
  }
  stats_.rules_emitted = rules.size();
  return rules;
}

}  // namespace miner
}  // namespace gar
