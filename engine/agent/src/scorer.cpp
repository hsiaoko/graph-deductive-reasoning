#include "gar/engine/agent/scorer.h"

#include <cmath>
#include <limits>
#include <unordered_set>

#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/state.h"
#include "gar/engine/agent/witness.h"
#include "gar/hdc/evidence.h"

namespace gar {
namespace engine {
namespace agent {

namespace {

// rel_fam = gamma^{sum_q d(q)} over the rule's DISTINCT predicates
// (pattern edge labels + antecedent predicates + consequent); any
// unreachable predicate zeroes the score (ScoreF's soft filter, App. H).
double RelFam(const GARule& rule, const ScoringContext& ctx, double gamma) {
  const auto& d = *ctx.distances;
  const auto& table = *ctx.predicates;
  std::unordered_set<std::string> seen;
  double sum = 0.0;
  auto add = [&](const std::string& name) {
    if (!seen.insert(name).second) return;
    const std::uint32_t id = table.Find(name);
    if (id == analysis::kInvalidPredicate || d[id] == analysis::kUnreachable) {
      sum = std::numeric_limits<double>::infinity();
      return;
    }
    if (sum != std::numeric_limits<double>::infinity()) sum += d[id];
  };
  for (std::uint32_t e = 0; e < rule.pattern.num_edges(); ++e) {
    add(analysis::LabelNameOf(rule.pattern.edge_labels[e], rule));
  }
  for (const auto& pred : rule.antecedent) {
    add(analysis::PredicateIdOf(pred, rule));
  }
  if (rule.consequent) add(analysis::PredicateIdOf(*rule.consequent, rule));
  if (sum == std::numeric_limits<double>::infinity()) return 0.0;
  return std::pow(gamma, sum);
}

// d(p0): distance of the consequent's predicate to the goal.
std::uint32_t HeadDistance(const GARule& rule, const ScoringContext& ctx) {
  if (!rule.consequent) return analysis::kUnreachable;
  const std::uint32_t id =
      ctx.predicates->Find(analysis::PredicateIdOf(*rule.consequent, rule));
  if (id == analysis::kInvalidPredicate) return analysis::kUnreachable;
  return (*ctx.distances)[id];
}

// price + admissible, shared by both scorers.
void FillPriceAdmissible(const GARule& rule, const Motif& mu,
                         const ScoringContext& ctx, const CostModel& cost,
                         ScoredMotif* out) {
  const std::uint32_t dp0 = HeadDistance(rule, ctx);
  const double w0 = cost.RuleLowerBound(rule);
  out->price = dp0 == analysis::kUnreachable
                   ? std::numeric_limits<double>::infinity()
                   : w0 + dp0;
  const RuleHead head = ResolveRuleHead(rule);
  out->admissible =
      head.valid && dp0 != analysis::kUnreachable &&
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

double Total(const HandcraftedConfig& cfg, double confidence,
             ScoredMotif* sm) {
  sm->rel = sm->rel_fam + cfg.nu * sm->rel_evd +
            cfg.nu2 * (cfg.use_confidence ? confidence : 0.0);
  return cfg.alpha * sm->rel - cfg.beta * sm->dcost;
}

}  // namespace

void ExactScorer::Score(const GARule& rule, const Motif& mu,
                        const ScoringContext& ctx, ScoredMotif* out) const {
  out->motif = mu;
  out->rel_fam = RelFam(rule, ctx, cfg_.gamma);
  FillPriceAdmissible(rule, mu, ctx, *cost_, out);

  std::vector<Fact> premises;
  GroundPremises(rule, mu, ctx.state->graph(), *ctx.predicates, &premises);
  std::vector<char> hits(premises.size());
  std::uint64_t n_hits = 0;
  for (std::size_t i = 0; i < premises.size(); ++i) {
    hits[i] = ctx.witness->Contains(premises[i]);
    n_hits += hits[i];
  }
  out->premises_total = static_cast<std::uint32_t>(premises.size());
  out->premises_witnessed = static_cast<std::uint32_t>(n_hits);
  out->rel_evd =
      premises.empty() ? 0.0
                       : static_cast<double>(n_hits) / premises.size();
  out->dcost = ResidualCost(premises, hits, *cost_);
  out->total = Total(cfg_, rule.quality.confidence, out);
}

void HdcScorer::Score(const GARule& rule, const Motif& mu,
                      const ScoringContext& ctx, ScoredMotif* out) const {
  out->motif = mu;
  out->rel_fam = RelFam(rule, ctx, cfg_.gamma);
  FillPriceAdmissible(rule, mu, ctx, *cost_, out);

  std::vector<Fact> premises;
  GroundPremises(rule, mu, ctx.state->graph(), *ctx.predicates, &premises);
  std::vector<char> hits(premises.size());

  // Encode each premise fact ONCE (memoized; identical vectors by
  // construction) and reuse for both the v_mu bundle and the per-premise
  // witness-hit probes.
  std::vector<const hdc::Hypervector*> pvecs;
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
                           .emplace(key, enc_.EncodeFact(key.pred, args,
                                                         key.arity))
                           .first->second);
    }
  }

  // Fresh chain convention (§4.1.1): E = 0 reads rel_evd = 0.
  if (ctx.witness->Count() == 0 || premises.empty()) {
    out->rel_evd = 0.0;
    for (std::size_t i = 0; i < hits.size(); ++i) hits[i] = 0;
  } else {
    // v_mu = bundle of the encoded premise facts; PackedHdc is the cached
    // sign-packed evidence vector (re-packed only on new witness events).
    const hdc::Hypervector& packed_e = ctx.witness->PackedHdc();
    hdc::EvidenceVector v_mu(enc_.dim());
    for (const hdc::Hypervector* v : pvecs) v_mu.Witness(*v);
    out->rel_evd = hdc::Hypervector::Cosine(packed_e, v_mu.Packed());
    for (std::size_t i = 0; i < premises.size(); ++i) {
      hits[i] = hdc::Hypervector::Cosine(packed_e, *pvecs[i]) >= cfg_.tau_hdc;
    }
  }
  std::uint32_t n_hits = 0;
  for (const char h : hits) n_hits += h;
  out->premises_total = static_cast<std::uint32_t>(premises.size());
  out->premises_witnessed = n_hits;
  out->dcost = ResidualCost(premises, hits, *cost_);
  out->total = Total(cfg_, rule.quality.confidence, out);
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
