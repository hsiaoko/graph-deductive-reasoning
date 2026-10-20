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

class HdcScorer final : public Scorer {
 public:
  HdcScorer(HandcraftedConfig cfg, const CostModel* cost_model,
            hdc::HdcEncoder encoder)
      : cfg_(cfg), cost_(cost_model), enc_(encoder) {}

  void Score(const GARule& rule, const Motif& mu, const ScoringContext& ctx,
             ScoredMotif* out) const override;

 private:
  HandcraftedConfig cfg_;
  const CostModel* cost_;
  hdc::HdcEncoder enc_;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
