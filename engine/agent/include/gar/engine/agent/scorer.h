#pragma once

// Scorer: candidate-motif scoring (paper §4.1). The handcrafted score is
//   f(S,g,mu) = alpha * rel(S,g,mu) - beta * dcost(mu,S)
// with rel = rel_fam + nu * rel_evd + nu2 * confidence (confidence term
// gated by use_confidence), where
//   rel_fam = gamma^{sum_q d(q)}  over the rule's distinct predicates
//             (pattern edge labels + antecedent + consequent); any
//             unreachable predicate zeroes it (ScoreF, App. H — the soft
//             filter);
//   rel_evd = witnessed-premise share (ExactScorer) or the HDC cosine
//             cos(v_mu, E) against the chain's evidence vector (HdcScorer);
//   dcost   = sum over premises (1 - s_p) * T(p): the residual evaluation
//             cost, s_p the witnessed-hit indicator (HdcScorer: cosine >=
//             tau), T from the CostModel;
//   price   = w0(phi) + d(p0): the completion-price lower bound (§3.4/5.4);
//   admissible = d(p0) < inf and the grounded conclusion not yet in Gamma.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gar/analysis/predicate_table.h"
#include "gar/engine/agent/cost_model.h"
#include "gar/engine/agent/motif.h"
#include "gar/hdc/encoder.h"
#include "gar/rule.h"

namespace gar {
namespace engine {
namespace agent {

class AgentState;
class WitnessLog;

struct HandcraftedConfig {
  double alpha = 1.0;
  double beta = 1.0;
  double gamma = 0.5;   // rel_fam decay
  double nu = 1.0;      // rel_evd weight
  double nu2 = 0.5;     // confidence weight
  bool use_confidence = true;
  // HdcScorer witness-hit threshold: fixed at 0.15, on the order of the
  // App I noise scale 1/sqrt(|E|) for chain evidence of tens-to-hundreds of
  // facts (1/sqrt(50) ≈ 0.14).
  double tau_hdc = 0.15;
};

// Rebuilt once per goal: the goal's d(.) table plus the predicate table it
// indexes (the two travel together — distances are meaningless without the
// id mapping), the chain witness and the current state.
struct ScoringContext {
  const std::vector<std::uint32_t>* distances;
  const analysis::PredicateTable* predicates;
  const WitnessLog* witness;
  const AgentState* state;
};

class Scorer {
 public:
  virtual ~Scorer() = default;
  virtual void Score(const GARule& rule, const Motif& mu,
                     const ScoringContext& ctx, ScoredMotif* out) const = 0;
  // Default: serial loop (deterministic by construction).
  virtual void ScoreBatch(const std::vector<GARule>& rules,
                          const std::vector<Motif>& mus,
                          const ScoringContext& ctx,
                          std::vector<ScoredMotif>* out) const {
    out->clear();
    out->reserve(mus.size());
    for (const auto& mu : mus) {
      out->emplace_back();
      Score(rules[mu.rule], mu, ctx, &out->back());
    }
  }
};

class ExactScorer final : public Scorer {
 public:
  ExactScorer(HandcraftedConfig cfg, const CostModel* cost_model)
      : cfg_(cfg), cost_(cost_model) {}

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;

 private:
  HandcraftedConfig cfg_;
  const CostModel* cost_;
};

// NullScorer: scoring ablation ("--scorer null"). Keeps the symbolic
// admissibility check (d(p0) < inf and conclusion not yet in Gamma — the
// agent's termination/correctness guard) but emits no guidance: rel_fam /
// rel_evd / rel / dcost / total / price are all zero. Greedy policies then
// degenerate to candidate-enumeration order (score ties break to lower
// index), and branch-and-bound degenerates to uniform-cost search.
class NullScorer final : public Scorer {
 public:
  NullScorer() = default;

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;
};

class HdcScorer final : public Scorer {
 public:
  HdcScorer(HandcraftedConfig cfg, const CostModel* cost_model,
            hdc::HdcEncoder encoder)
      : cfg_(cfg), cost_(cost_model), enc_(encoder) {}

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;

 private:
  // Fact-vector memo key: EncodeFact is a pure function of (pred symbol,
  // args, arity) under a fixed encoder; candidates repeat premise facts
  // heavily (same predicates, overlapping constants). Equality-compared, so
  // hash collisions never alias two facts. Capped — clearing only forces
  // recomputation, never changes a score.
  struct FactVecKey {
    std::uint64_t pred, a0, a1;
    std::uint32_t arity;
    bool operator==(const FactVecKey& o) const = default;
  };
  struct FactVecKeyHash {
    std::size_t operator()(const FactVecKey& k) const {
      std::uint64_t h = k.pred;
      h = (h ^ k.a0) * 0xBF58476D1CE4E5B9ull;
      h = (h ^ k.a1) * 0x94D049BB133111EBull;
      h = (h ^ k.arity) * 0xD6E8FEB86659FD93ull;
      return static_cast<std::size_t>(h ^ (h >> 31));
    }
  };

  HandcraftedConfig cfg_;
  const CostModel* cost_;
  hdc::HdcEncoder enc_;
  mutable std::unordered_map<FactVecKey, hdc::Hypervector, FactVecKeyHash>
      fact_cache_;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
