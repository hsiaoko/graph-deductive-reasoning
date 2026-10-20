#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gar/miner/triple_graph.h"
#include "gar/rule.h"

// ChainMiner: mines GAR chain rules from a triple graph, AMIE-style:
//
//   body:  r1(x0,x1) ∧ r2(x1,x2) ∧ ... ∧ rk(xk-1,xk)
//          (k = min_body_len..max_body_len)
//   head:  rh(x0, xk)
//
// * support    = |{(x0,xk) : body ∧ head}|      (distinct pivot pairs)
// * confidence = support / |{(x0,xk) : body}|
//
// Pair sets are deduplicated at every join step; counting is capped at
// `pair_cap` pairs (a capped support is reported as a lower bound), which
// keeps hub-heavy chains tractable.
namespace gar {
namespace miner {

struct MinerConfig {
  std::uint32_t max_body_len = 2;        // chain length k
  std::uint32_t min_body_len = 1;        // skip chains shorter than this
                                         // (2 = no single-edge r(x,y)->s(x,y)
                                         // relation-implication rules)
  std::uint64_t min_support = 50;        // on support (body ∧ head pairs)
  double min_confidence = 0.05;
  std::size_t pair_cap = 4'000'000;      // per-chain pair-count cap
  std::uint64_t join_attempt_cap = 50'000'000;  // per-chain join work cap
  bool with_inverses = true;             // include r^-1 slots in chains
  // Specialize patterns with vertex labels (entity types) when the graph
  // carries them: chain pairs are bucketed by the label signature of the
  // matched vertices, and each bucket becomes its own typed rule.
  bool use_vertex_labels = true;
  std::string dataset_root;              // dataset root path, recorded as provenance
};

struct MinerStats {
  std::uint64_t chains_evaluated = 0;
  std::uint64_t chains_capped = 0;
  std::uint64_t rules_emitted = 0;
};

class ChainMiner {
 public:
  // Label signature of a chain instance: vertex labels of (x0,...,xk),
  // zero-padded (k <= 2, so at most 3 entries are meaningful).
  using TypeSig = std::array<std::uint32_t, 3>;
  struct TypeSigHash {
    std::size_t operator()(const TypeSig& s) const {
      std::uint64_t h = s[0];
      h = h * 0x9E3779B97F4A7C15ull + s[1];
      h = h * 0x9E3779B97F4A7C15ull + s[2];
      return static_cast<std::size_t>(h);
    }
  };
  using TypedPairSets =
      std::unordered_map<TypeSig, std::unordered_set<std::uint64_t>,
                         TypeSigHash>;

  ChainMiner(const TripleGraph& graph, MinerConfig config);

  // Runs the mining pass and returns the rules (quality fields filled).
  std::vector<GARule> Mine();

  [[nodiscard]] const MinerStats& stats() const { return stats_; }

 private:
  // Counts chain pairs; may stop early at pair_cap (sets *capped).
  std::unordered_set<std::uint64_t> ChainPairs(const std::uint32_t* chain,
                                               std::uint32_t len,
                                               bool* capped) const;
  // Same, but bucketed by the vertex-label signature of each witness.
  TypedPairSets TypedChainPairs(const std::uint32_t* chain, std::uint32_t len,
                                bool* capped) const;
  void EmitRulesForChain(const std::vector<std::uint32_t>& chain,
                         const std::unordered_set<std::uint64_t>& body_pairs,
                         const TypeSig* sig, std::vector<GARule>* out);

  const TripleGraph& g_;
  MinerConfig cfg_;
  MinerStats stats_;
};

}  // namespace miner
}  // namespace gar
