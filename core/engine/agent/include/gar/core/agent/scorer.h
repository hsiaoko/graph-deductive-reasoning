#pragma once

// Scorer: candidate-motif scoring (paper §4). The canonical score is
//   f(S,g,mu) = alpha * relevance + beta * evidence - cost_weight * dcost.
// Relevance is gamma^d(p0). Evidence is the distinct witnessed-premise share
// or the paper's set-based HDC estimate. Confidence is an optional experimental
// term and is disabled by default. Similarity never establishes predicate truth.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gar/core/analysis/predicate_table.h"
#include "gar/core/agent/cost_model.h"
#include "gar/core/agent/motif.h"
#include "gar/core/hdc/encoder.h"
#include "gar/core/rule.h"

namespace gar::core::engine {
namespace agent {

class AgentState;
class WitnessLog;

struct HandcraftedConfig {
  double alpha = 1.0;
  double beta = 1.0;         // evidence weight
  double gamma = 0.5;        // goal-distance decay
  double cost_weight = 1.0;  // residual evaluation-cost weight
  double nu2 = 0.5;          // experimental confidence weight
  bool use_confidence = false;
};

// Rebuilt once per goal: the goal's d(.) table plus the predicate table it
// indexes (the two travel together — distances are meaningless without the
// id mapping), the chain witness and the current state.
struct ScoringContext {
  const std::vector<std::uint32_t>* distances;
  const gar::core::analysis::PredicateTable* predicates;
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
  // Rule-level static score (see ScoredRule): the incremental greedy's
  // policy-first selection ranks rules before any matching. Exact and HDC
  // share the same symbolic static terms (the HDC readout is
  // embedding-level evidence only); the null scorer emits zero guidance.
  // out->rule is NOT set (the caller knows the index).
  virtual void ScoreRule(const GARule& rule, const ScoringContext& ctx,
                         ScoredRule* out) const = 0;
  // Whether the scorer reads the witness's HDC evidence vector (only the
  // HDC scorer does; exact/null skip the 8192-dim bundle maintenance).
  virtual bool NeedsHdcWitness() const { return false; }
  // Exact/null scores can be maintained through new exact-witness facts.
  // HDC currently uses the full-rescoring path after a new distinct witness.
  virtual bool SupportsIncrementalScoring() const { return false; }
};

class ExactScorer final : public Scorer {
 public:
  ExactScorer(HandcraftedConfig cfg, const CostModel* cost_model)
      : cfg_(cfg), cost_(cost_model) {}

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;

  void ScoreRule(const GARule& rule, const ScoringContext& ctx,
                 ScoredRule* out) const override;

  bool SupportsIncrementalScoring() const override { return true; }

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

  void ScoreRule(const GARule& rule, const ScoringContext& ctx,
                 ScoredRule* out) const override {
    (void)rule;
    (void)ctx;
    (void)out;  // all zeros: rule order falls back to rule index
  }

  bool SupportsIncrementalScoring() const override { return true; }
};

class HdcScorer final : public Scorer {
 public:
  HdcScorer(HandcraftedConfig cfg, const CostModel* cost_model,
            gar::core::hdc::HdcEncoder encoder)
      : cfg_(cfg), cost_(cost_model), enc_(encoder) {}

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;

  void ScoreRule(const GARule& rule, const ScoringContext& ctx,
                 ScoredRule* out) const override;

  bool NeedsHdcWitness() const override { return true; }

 private:
  // Fact-vector memo key: EncodeFactDense is a pure function of (pred symbol,
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
  gar::core::hdc::HdcEncoder enc_;
  mutable std::unordered_map<FactVecKey, gar::core::hdc::DenseVector,
                             FactVecKeyHash> fact_cache_;
};

}  // namespace agent
}  // namespace gar::core::engine
