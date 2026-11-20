#include "gar/core/agent/scorer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/agent/state.h"
#include "gar/core/agent/witness.h"
#include "gar/core/hdc/evidence.h"

namespace gar::core::engine {
namespace agent {

namespace {

double RelFam(const GARule& rule, const ScoringContext& ctx, double gamma) {
  if (!rule.consequent) return 0.0;
  const std::uint32_t id = ctx.predicates->Find(
      gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
  if (id == gar::core::analysis::kInvalidPredicate ||
      (*ctx.distances)[id] == gar::core::analysis::kUnreachable) {
    return 0.0;
  }
  return std::pow(gamma, (*ctx.distances)[id]);
}

// d(p0): distance of the consequent's predicate to the goal.
std::uint32_t HeadDistance(const GARule& rule, const ScoringContext& ctx) {
  if (!rule.consequent) return gar::core::analysis::kUnreachable;
  const std::uint32_t id =
      ctx.predicates->Find(gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
  if (id == gar::core::analysis::kInvalidPredicate) return gar::core::analysis::kUnreachable;
  return (*ctx.distances)[id];
}

// price + admissible, shared by both scorers.
void FillPriceAdmissible(const GARule& rule, const Motif& mu,
                         const ScoringContext& ctx, const CostModel& cost,
                         ScoredMotif* out) {
  const std::uint32_t dp0 = HeadDistance(rule, ctx);
  const double w0 = cost.RuleLowerBound(rule);
  out->price = dp0 == gar::core::analysis::kUnreachable
                   ? std::numeric_limits<double>::infinity()
                   : w0 + dp0;
  const RuleHead head = ResolveRuleHead(rule);
  out->admissible =
      head.valid && dp0 != gar::core::analysis::kUnreachable &&
      !ctx.state->ContainsEdge(head.slot, mu.embedding[head.pu],
                               mu.embedding[head.pv]);
}

// Residual cost: sum over premises (1 - s_p) * T(p). Pattern-edge premises
// are edge lookups (CostModel::EdgeTestCost); attr/ml premises go by kind
// (unit model: 1; a logged model would refine per predicate — anchor).
double ResidualCost(const std::vector<Fact>& premises,
                    const std::vector<char>& hits, const CostModel& cost) {
  double dcost = 0.0;
  for (std::size_t i = 0; i < premises.size(); ++i) {
    if (hits[i]) continue;
    dcost += premises[i].kind == FactKind::kEdge ? cost.EdgeTestCost() : 1.0;
  }
  return dcost;
}

std::vector<Fact> DistinctPremises(const GARule& rule, const Motif& mu,
                                   const ScoringContext& ctx) {
  std::vector<Fact> input;
  GroundPremises(rule, mu, ctx.state->graph(), *ctx.predicates, &input);
  std::unordered_set<FactKey, FactKeyHash> seen;
  std::vector<Fact> output;
  for (const Fact& fact : input) {
    if (seen.insert(KeyOf(fact)).second) output.push_back(fact);
  }
  return output;
}

double Total(const HandcraftedConfig& cfg, double confidence,
             ScoredMotif* sm) {
  sm->rel = sm->rel_fam +
            cfg.nu2 * (cfg.use_confidence ? confidence : 0.0);
  return cfg.alpha * sm->rel + cfg.beta * sm->rel_evd -
         cfg.cost_weight * sm->dcost;
}

// Rule-level static score contains only embedding-independent terms. HDC
// evidence remains action-level.
void FillScoredRule(const HandcraftedConfig& cfg, const CostModel& cost,
                    const GARule& rule, const ScoringContext& ctx,
                    ScoredRule* out) {
  out->rel_fam = RelFam(rule, ctx, cfg.gamma);
  out->confidence = cfg.use_confidence ? rule.quality.confidence : 0.0;
  const std::uint32_t dp0 = HeadDistance(rule, ctx);
  out->price = dp0 == gar::core::analysis::kUnreachable
                   ? std::numeric_limits<double>::infinity()
                   : cost.RuleLowerBound(rule) + dp0;
  out->priority = cfg.alpha * (out->rel_fam + cfg.nu2 * out->confidence);
}

}  // namespace

void NullScorer::Score(const GARule& rule, const Motif& mu,
                       const ScoringContext& ctx, ScoredMotif* out) const {
  out->motif = mu;
  const std::uint32_t dp0 = HeadDistance(rule, ctx);
  const RuleHead head = ResolveRuleHead(rule);
  out->admissible =
      head.valid && dp0 != gar::core::analysis::kUnreachable &&
      !ctx.state->ContainsEdge(head.slot, mu.embedding[head.pu],
                               mu.embedding[head.pv]);
}

void ExactScorer::ScoreRule(const GARule& rule, const ScoringContext& ctx,
                            ScoredRule* out) const {
  FillScoredRule(cfg_, *cost_, rule, ctx, out);
}

void HdcScorer::ScoreRule(const GARule& rule, const ScoringContext& ctx,
                          ScoredRule* out) const {
  FillScoredRule(cfg_, *cost_, rule, ctx, out);
}

void ExactScorer::Score(const GARule& rule, const Motif& mu,
                        const ScoringContext& ctx, ScoredMotif* out) const {
  out->motif = mu;
  out->rel_fam = RelFam(rule, ctx, cfg_.gamma);
  FillPriceAdmissible(rule, mu, ctx, *cost_, out);

  const std::vector<Fact> premises = DistinctPremises(rule, mu, ctx);
  std::vector<char> hits(premises.size());
  std::uint64_t n_hits = 0;
  for (std::size_t i = 0; i < premises.size(); ++i) {
    hits[i] = ctx.witness->Contains(premises[i]);
    n_hits += hits[i];
  }
  out->premises_total = static_cast<std::uint32_t>(premises.size());
  out->premises_witnessed = static_cast<std::uint32_t>(n_hits);
  out->rel_evd =
      premises.empty() ? 1.0
                       : static_cast<double>(n_hits) / premises.size();
  out->dcost = ResidualCost(premises, hits, *cost_);
  out->total = Total(cfg_, rule.quality.confidence, out);
}

void HdcScorer::Score(const GARule& rule, const Motif& mu,
                      const ScoringContext& ctx, ScoredMotif* out) const {
  out->motif = mu;
  out->rel_fam = RelFam(rule, ctx, cfg_.gamma);
  FillPriceAdmissible(rule, mu, ctx, *cost_, out);

  const std::vector<Fact> premises = DistinctPremises(rule, mu, ctx);
  std::vector<const gar::core::hdc::DenseVector*> pvecs;
  pvecs.reserve(premises.size());
  for (const auto& p : premises) {
    const FactVecKey key{
        FactPredicateSymbol(p, ctx.state->graph(), *ctx.predicates),
        p.args[0], p.args[1], p.arity};
    const auto it = fact_cache_.find(key);
    if (it != fact_cache_.end()) {
      pvecs.push_back(&it->second);
    } else {
      if (fact_cache_.size() >= (1u << 20)) fact_cache_.clear();
      const std::uint64_t args[2] = {p.args[0], p.args[1]};
      pvecs.push_back(&fact_cache_
                           .emplace(key, enc_.EncodeFactDense(
                                               key.pred, args, key.arity))
                           .first->second);
    }
  }

  if (premises.empty()) {
    out->rel_evd = 1.0;
  } else if (ctx.witness->Count() == 0) {
    out->rel_evd = 0.0;
  } else {
    gar::core::hdc::EvidenceVector v_mu(enc_.dim());
    for (const gar::core::hdc::DenseVector* v : pvecs) v_mu.Witness(*v);
    out->rel_evd = std::clamp(v_mu.Dot(ctx.witness->hdc()) /
                                  static_cast<double>(premises.size()),
                              0.0, 1.0);
  }

  // Approximate HDC evidence guides ranking only. Residual work and witness
  // counts use exact set membership.
  std::vector<char> hits(premises.size());
  std::uint32_t n_hits = 0;
  for (std::size_t i = 0; i < premises.size(); ++i) {
    hits[i] = ctx.witness->Contains(premises[i]);
    n_hits += hits[i];
  }
  out->premises_total = static_cast<std::uint32_t>(premises.size());
  out->premises_witnessed = n_hits;
  out->dcost = ResidualCost(premises, hits, *cost_);
  out->total = Total(cfg_, rule.quality.confidence, out);
}

}  // namespace agent
}  // namespace gar::core::engine
