#include "deduce_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <utility>

#include "gar/core/agent/learning/goal_logistic_model.h"
#include "gar/core/agent/learning/relevance_model.h"
#include "gar/core/agent/value_model.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/predicate_evaluator.h"
#include "rule_support.h"

namespace gar::core::engine {
namespace agent {

bool DeduceSession::ActionIndex::Insert(std::uint32_t rule,
                                        const std::vector<std::uint32_t>& e) {
  return Insert(rule, std::vector<std::uint32_t>(e));
}

bool DeduceSession::ActionIndex::Insert(std::uint32_t rule,
                                        std::vector<std::uint32_t>&& emb) {
  auto& bucket =
      m_[MotifHash(rule, emb.data(), static_cast<std::uint32_t>(emb.size()))];
  for (const auto& [r, e] : bucket) {
    if (r == rule && e == emb) return false;
  }
  bucket.emplace_back(rule, std::move(emb));
  return true;
}

void DeduceSession::Trace(std::string ev) {
  res_.stats->trace.push_back(std::move(ev));
}

void DeduceSession::RefreshPeaks() {
  auto& fx = *res_.stats;
  fx.ready_peak = std::max<std::uint64_t>(fx.ready_peak, ws_.ready().size());
  fx.suspended_peak =
      std::max<std::uint64_t>(fx.suspended_peak, ws_.suspended().size());
}

bool DeduceSession::WallExceeded() const {
  return cfg_.wall_limit_sec > 0.0 &&
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_)
                 .count() >= cfg_.wall_limit_sec;
}

bool DeduceSession::BudgetHit() const {
  return res_.stats->search_cost >= static_cast<double>(cfg_.budget);
}

void DeduceSession::BuildPlans() {
  if (kit_.paper_guide != nullptr) kit_.paper_guide->Prepare();
  const auto& rules = state_->rules();
  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    deduce::RulePlan plan;
    if (deduce::BuildRulePlan(rules[i], i, ctx_, g_, &plan)) {
      plans_.push_back(std::move(plan));
    }
  }
  // Learned star order resolves to input or goal per goal via a goal-level
  // logistic model; the rest of the plan build is identical to the fixed order.
  StarOrder effective_order = cfg_.star_order;
  if (effective_order == StarOrder::kLearned) {
    if (cfg_.star_order_model_path.empty())
      throw std::invalid_argument(
          "star-order learned requires --star-order-model");
    const auto model = learning::GoalLogisticModel::Load(
        cfg_.star_order_model_path, "gar_star_order_v1");
    const auto cone = gar::core::analysis::GoalConeRuleIndices(
        rules, g_.RelationName(goal_.slot));
    const double degree = static_cast<double>(g_.RelationPairs(goal_.slot).size());
    const double p_input =
        model.Probability(static_cast<double>(cone.size()), degree);
    effective_order =
        p_input >= model.threshold ? StarOrder::kInput : StarOrder::kGoal;
  }
  if (effective_order == StarOrder::kGoal) {
    auto head_distance = [&](const deduce::RulePlan& plan) {
      const GARule& rule = rules[plan.rule];
      const std::uint32_t predicate = ctx_.table->Find(
          gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
      return predicate == gar::core::analysis::kInvalidPredicate
                 ? gar::core::analysis::kUnreachable
                 : ctx_.distances[predicate];
    };
    std::stable_sort(plans_.begin(), plans_.end(),
                     [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
                       return head_distance(a) < head_distance(b);
                     });
  }
  if (kit_.proof_star_model != nullptr) {
    for (const auto& plan : plans_) {
      res_.stats->proof_star_estimated_cost +=
          EstimateStarCost(rules[plan.rule], g_);
    }
    res_.stats->proof_star_active =
        res_.stats->proof_star_estimated_cost >= cfg_.proof_star_min_cost;
  }
  if (kit_.proof_star_model != nullptr && cfg_.proof_star_weight > 0.0 &&
      res_.stats->proof_star_active && plans_.size() > 1) {
    std::vector<double> efficiency(rules.size(), 0.0);
    std::vector<std::uint32_t> distance(rules.size(),
                                        gar::core::analysis::kUnreachable);
    for (const auto& plan : plans_) {
      const GARule& rule = rules[plan.rule];
      ScoredRule score;
      kit_.scorer->ScoreRule(rule, sctx_, &score);
      const std::array<double, 11> features{
          score.price,
          static_cast<double>(rule.num_edges()),
          std::log1p(rule.quality.support),
          score.rel_fam,
          rule.quality.confidence,
          score.price - kit_.cost_model->RuleLowerBound(rule),
          0.0,
          0.0,
          0.0,
          0.0,
          0.0};
      efficiency[plan.rule] = kit_.proof_star_model->PredictRemaining(features);
      const std::uint32_t predicate = ctx_.table->Find(
          gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
      distance[plan.rule] = predicate == gar::core::analysis::kInvalidPredicate
                                ? gar::core::analysis::kUnreachable
                                : ctx_.distances[predicate];
    }
    std::stable_sort(plans_.begin(), plans_.end(),
                     [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
                       return distance[a.rule] < distance[b.rule];
                     });
    std::vector<double> order_score(rules.size(), 0.0);
    for (std::size_t begin = 0; begin < plans_.size();) {
      std::size_t end = begin + 1;
      while (end < plans_.size() &&
             distance[plans_[end].rule] == distance[plans_[begin].rule]) {
        ++end;
      }
      const auto [min_it, max_it] = std::minmax_element(
          plans_.begin() + begin, plans_.begin() + end,
          [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
            return efficiency[a.rule] < efficiency[b.rule];
          });
      const double minimum = efficiency[min_it->rule];
      const double span = efficiency[max_it->rule] - minimum;
      if (distance[plans_[begin].rule] == 0) {
        std::stable_sort(
            plans_.begin() + begin, plans_.begin() + end,
            [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
              return efficiency[a.rule] > efficiency[b.rule];
            });
        begin = end;
        continue;
      }
      for (std::size_t j = begin + 1; j < end; ++j) {
        const double baseline = end - begin == 1
                                    ? 0.0
                                    : static_cast<double>(end - 1 - j) /
                                          static_cast<double>(end - begin - 1);
        const double proof =
            span > 0.0 ? (efficiency[plans_[j].rule] - minimum) / span : 0.0;
        order_score[plans_[j].rule] = baseline + cfg_.proof_star_weight * proof;
      }
      std::stable_sort(
          plans_.begin() + begin + 1, plans_.begin() + end,
          [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
            return order_score[a.rule] > order_score[b.rule];
          });
      begin = end;
    }
  }
  if (kit_.star_guide != nullptr && cfg_.guide_strength > 0.0 &&
      plans_.size() > 1) {
    std::vector<std::uint32_t> eligible;
    eligible.reserve(plans_.size());
    for (const auto& plan : plans_) eligible.push_back(plan.rule);
    const auto& preferences = kit_.star_guide->Preferences(eligible);
    std::vector<double> order_score(rules.size(), 0.0);
    for (std::size_t j = 0; j < plans_.size(); ++j) {
      const double baseline = static_cast<double>(plans_.size() - 1 - j) /
                              static_cast<double>(plans_.size() - 1);
      order_score[plans_[j].rule] =
          baseline + cfg_.guide_strength * preferences[plans_[j].rule];
    }
    std::stable_sort(plans_.begin(), plans_.end(),
                     [&](const deduce::RulePlan& a, const deduce::RulePlan& b) {
                       return order_score[a.rule] > order_score[b.rule];
                     });
  }
  for (std::uint32_t pi = 0; pi < plans_.size(); ++pi) {
    for (const auto& edge : plans_[pi].spec.edges) {
      complete_slot_users_[edge[2]].push_back(pi);
    }
    for (const auto& de : plans_[pi].defers) {
      for (const auto& edge : de.sub_spec.edges) {
        partial_slot_users_[edge[2]].push_back(pi);
      }
    }
  }
  prod_.emplace(plans_, rules, ctx_.table);
  seen_partials_ = std::vector<ActionIndex>(plans_.size());
}

std::unordered_map<std::uint32_t, std::uint32_t> DeduceSession::GoalPins(
    const deduce::RulePlan& pl) const {
  std::unordered_map<std::uint32_t, std::uint32_t> pins;
  if (pl.head.slot == goal_.slot) {
    const GARule& rule = state_->rules()[pl.rule];
    const std::uint32_t predicate = ctx_.table->Find(
        gar::core::analysis::PredicateIdOf(*rule.consequent, rule));
    // A recursive goal may need same-predicate intermediate facts before the
    // final constants can be derived. Pinning every complete match to the
    // final head would exclude those necessary actions.
    if (predicate != gar::core::analysis::kInvalidPredicate &&
        ctx_.dg->RecursiveFamilyOf(predicate) >= 0) {
      return pins;
    }
    if (goal_.args[0]) pins[pl.head.pu] = *goal_.args[0];
    if (goal_.args[1]) pins[pl.head.pv] = *goal_.args[1];
  }
  return pins;
}

std::unordered_map<std::uint32_t, std::uint32_t> DeduceSession::GoalSubPins(
    const deduce::RulePlan& pl, const deduce::DeferrableEdge& de) const {
  // Goal pins only when the head endpoint survives in the sub-spec.
  std::unordered_map<std::uint32_t, std::uint32_t> sub_pins;
  for (const auto& [ov, val] : GoalPins(pl)) {
    for (std::uint32_t sv = 0; sv < de.to_orig.size(); ++sv) {
      if (de.to_orig[sv] == ov) {
        sub_pins[sv] = val;
        break;
      }
    }
  }
  return sub_pins;
}

bool DeduceSession::IncrementalScores() const {
  return kit_.scorer->SupportsIncrementalScoring() &&
         kit_.policy->SupportsScoreView();
}

std::array<double, 11> DeduceSession::ActionFeatures(
    const deduce::ActionRecord& rec) const {
  const GARule& rule = state_->rules()[rec.motif.rule];
  return {rec.score.price,
          static_cast<double>(rule.num_edges()),
          std::log1p(rule.quality.support),
          rec.score.rel_fam,
          rec.score.rel_evd,
          rule.quality.confidence,
          rec.score.dcost,
          rec.score.total,
          rec.score.price - kit_.cost_model->RuleLowerBound(rule),
          static_cast<double>(rec.score.premises_witnessed),
          static_cast<double>(rec.score.premises_total)};
}

void DeduceSession::ScoreReady(deduce::ActionRecord* rec) {
  auto& fx = *res_.stats;
  const auto started = std::chrono::steady_clock::now();
  kit_.scorer->Score(state_->rules()[rec->motif.rule], rec->motif, sctx_,
                     &rec->score);
  fx.score_wall_ms += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  ++fx.score_calls;
  ++fx.scored_candidates;
  rec->selection_priority =
      kit_.policy->SelectionPriorityForCandidate(rec->id, rec->score);
  if (kit_.policy->PrefersHighestScore()) {
    ws_.TouchReady(rec->id, kit_.policy->GuidePriority(rec->score));
  }
}

deduce::ActionRecord& DeduceSession::EnqueueReady(Motif mu, RuleHead head) {
  ++res_.stats->ready_insertions;
  auto& rec = ws_.AddReady(std::move(mu), head);
  if (!IncrementalScores()) return rec;
  ScoreReady(&rec);
  std::vector<Fact> premises;
  GroundPremises(state_->rules()[rec.motif.rule], rec.motif, g_, *ctx_.table,
                 &premises);
  rec.premise_keys.reserve(premises.size());
  for (const Fact& fact : premises) rec.premise_keys.push_back(KeyOf(fact));
  std::sort(rec.premise_keys.begin(), rec.premise_keys.end(),
            [](const FactKey& a, const FactKey& b) {
              return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
            });
  rec.premise_keys.erase(
      std::unique(rec.premise_keys.begin(), rec.premise_keys.end()),
      rec.premise_keys.end());
  for (const FactKey& key : rec.premise_keys) {
    premise_actions_[key].insert(rec.id);
  }
  return rec;
}

void DeduceSession::UntrackReady(const deduce::ActionRecord& rec) {
  if (!IncrementalScores()) return;
  kit_.policy->ReleaseSelectionPriority(rec.id);
  for (const FactKey& key : rec.premise_keys) {
    const auto it = premise_actions_.find(key);
    if (it == premise_actions_.end()) continue;
    it->second.erase(rec.id);
    if (it->second.empty()) premise_actions_.erase(it);
  }
}

void DeduceSession::RecordWitness(const Fact& fact) {
  if (!witness_->WitnessFact(fact, ctx_.encoder) || !IncrementalScores()) {
    return;
  }
  const auto it = premise_actions_.find(KeyOf(fact));
  if (it == premise_actions_.end()) return;
  for (const std::uint64_t id : it->second) {
    if (auto* rec = ws_.FindReady(id)) ScoreReady(rec);
  }
}

void DeduceSession::AddComplete(std::uint32_t pi,
                                const std::vector<std::uint32_t>& emb) {
  const deduce::RulePlan& pl = plans_[pi];
  const GARule& r = state_->rules()[pl.rule];
  if (!VertexLabelsOk(r, g_, emb.data())) return;
  ++res_.stats->match_embeddings;
  if (state_->ContainsEdge(pl.head.slot, emb[pl.head.pu], emb[pl.head.pv])) {
    ++res_.stats->match_head_known;
    return;  // conclusion already in Γ: nothing new
  }
  if (!seen_actions_.Insert(pl.rule, emb)) {
    ++res_.stats->match_duplicate;
    return;
  }
  ++res_.stats->match_new;
  auto& rec = EnqueueReady(Motif{pl.rule, emb}, pl.head);
  Trace("gen\trule=" + r.name + "\taction=" + std::to_string(rec.id));
  prod_->SetPending(pi, true);
}

void DeduceSession::AddPartialMatches(std::uint32_t pi,
                                      const deduce::DeferrableEdge& de,
                                      const std::vector<std::uint32_t>& embs) {
  const deduce::RulePlan& pl = plans_[pi];
  const GARule& r = state_->rules()[pl.rule];
  const auto ns = de.sub_spec.num_vertices;
  if (ns == 0) return;
  const std::uint64_t n_emb = embs.size() / ns;
  for (std::uint64_t t = 0; t < n_emb; ++t) {
    const std::uint32_t* se = embs.data() + t * ns;
    std::vector<std::uint32_t> full = deduce::ExpandPartial(
        de, se, static_cast<std::uint32_t>(r.num_vertices()));
    if (!VertexLabelsOk(r, g_, full.data())) continue;
    // Deferred edge satisfaction on current Γ.
    const std::uint32_t eu = full[de.pu], ev = full[de.pv];
    const bool u_bound = eu != UINT32_MAX, v_bound = ev != UINT32_MAX;
    bool present = false;
    if (u_bound && v_bound) {
      present = g_.RelationPairSet(de.slot).count(
                    gar::core::graph::PackPair(eu, ev)) != 0;
    } else if (u_bound) {
      const auto& pairs = g_.RelationPairs(de.slot);
      auto lo = std::lower_bound(pairs.begin(), pairs.end(),
                                 gar::core::graph::PackPair(eu, 0));
      present = lo != pairs.end() && (*lo >> 32) == eu;
    } else if (v_bound) {
      // dst-side presence: the mirror slot's pairs are (dst, src).
      const std::uint32_t mirror = g_.IsInverse(de.slot)
                                       ? g_.BaseRelation(de.slot)
                                       : de.slot + g_.num_relations();
      const auto& pairs = g_.RelationPairs(mirror);
      auto lo = std::lower_bound(pairs.begin(), pairs.end(),
                                 gar::core::graph::PackPair(ev, 0));
      present = lo != pairs.end() && (*lo >> 32) == ev;
    }
    if (present) continue;  // really complete; complete enum covers it
    std::vector<std::uint32_t> key = full;
    key.push_back(static_cast<std::uint32_t>(de.spec_edge));
    if (!seen_partials_[pi].Insert(pl.rule, std::move(key))) continue;
    deduce::SuspendedAction sw;
    sw.rule = pl.rule;
    sw.is_partial = true;
    sw.defer_idx = static_cast<std::size_t>(&de - &pl.defers[0]);
    sw.partial = std::move(full);
    sw.wait_since_round = 0;
    if (u_bound && v_bound) {
      sw.wait.ground = KeyOf(MakeEdgeFact(de.slot, eu, ev));  // slot kept as-is
    } else {
      sw.wait.open_side = true;
      sw.wait.slot = de.slot;
      sw.wait.bound_is_src = u_bound;
      sw.wait.bound_value = u_bound ? eu : ev;
    }
    auto& ref = ws_.AddSuspended(std::move(sw));
    Trace("suspend-partial\trule=" + r.name + "\twait_slot=" +
          g_.SlotName(de.slot) + "\taction=" + std::to_string(ref.id));
    prod_->SetPending(pi, true);
    ++res_.stats->suspend_events;
  }
}

void DeduceSession::VerifyGeneration() {
  // GAR_VERIFY_DEDUCE=1 (development only, small graphs): after every
  // generation/delta wave, EVERY complete match with a still-new conclusion
  // must have been generated (be in seen_actions_) — catches any delta path
  // that misses a match the full enumeration would find.
  if (!verify_) return;
  // Only EXPANDED stars are checked: an unexpanded star generates on its
  // turn and sees the full current G∪Γ, so its matches need no coverage.
  std::unique_ptr<MatchBackend> cpu_backend =
      MakeMatchBackend(MatchEngine::kCpu, g_);
  for (std::uint32_t pi = 0; pi < next_star_; ++pi) {
    const deduce::RulePlan& pl = plans_[pi];
    const GARule& r = state_->rules()[pl.rule];
    const std::vector<MatchResult> matches = cpu_backend->RunBatch(
        {MatchRequest{pl.spec, UINT64_MAX, GoalPins(pl)}});
    const std::vector<std::uint32_t>& embs = matches.front().embeddings;
    const auto n = pl.spec.num_vertices;
    const std::uint64_t n_emb = embs.size() / n;
    for (std::uint64_t t = 0; t < n_emb; ++t) {
      std::vector<std::uint32_t> emb(embs.data() + t * n,
                                     embs.data() + (t + 1) * n);
      if (!VertexLabelsOk(r, g_, emb.data())) continue;
      if (state_->ContainsEdge(pl.head.slot, emb[pl.head.pu],
                               emb[pl.head.pv])) {
        continue;  // conclusion present: no action needed
      }
      // The embedding must already be known (Insert returns true only when
      // it was MISSING).
      if (seen_actions_.Insert(pl.rule, emb)) {
        std::fprintf(stderr, "[verify-fixpoint] MISSING rule=%s emb#%llu",
                     r.name.substr(0, 80).c_str(),
                     static_cast<unsigned long long>(t));
        for (const std::uint32_t v : emb) {
          std::fprintf(stderr, " %s", g_.EntityName(v).c_str());
        }
        std::fprintf(stderr, "\n");
        std::abort();
      }
    }
  }
}

void DeduceSession::GenerateStar(std::uint32_t pi) {
  const deduce::RulePlan& pl = plans_[pi];
  std::vector<MatchRequest> requests;
  requests.push_back(
      MatchRequest{pl.spec, cfg_.match_cap_per_rule, GoalPins(pl)});
  for (const auto& de : pl.defers) {
    requests.push_back(MatchRequest{de.sub_spec, cfg_.match_cap_per_rule,
                                    GoalSubPins(pl, de)});
  }
  const auto gen_t0 = std::chrono::steady_clock::now();
  const std::vector<MatchResult> results = match_backend_->RunBatch(requests);
  const auto gen_t1 = std::chrono::steady_clock::now();
  const auto n = pl.spec.num_vertices;
  const std::vector<std::uint32_t>& embs = results.front().embeddings;
  if (results.front().capped) ++res_.stats->candidate_caps;
  for (std::uint64_t t = 0; t < embs.size() / n; ++t) {
    AddComplete(pi, std::vector<std::uint32_t>(embs.data() + t * n,
                                               embs.data() + (t + 1) * n));
  }
  for (std::size_t i = 0; i < pl.defers.size(); ++i) {
    const MatchResult& result = results[i + 1];
    if (result.capped) ++res_.stats->candidate_caps;
    AddPartialMatches(pi, pl.defers[i], result.embeddings);
  }
  const auto gen_t2 = std::chrono::steady_clock::now();
  res_.stats->gen_match_ms +=
      std::chrono::duration<double, std::milli>(gen_t1 - gen_t0).count();
  res_.stats->gen_add_ms +=
      std::chrono::duration<double, std::milli>(gen_t2 - gen_t1).count();
  prod_->MarkGenerated(pi);
  RefreshPeaks();
  VerifyGeneration();
}

void DeduceSession::CompleteDeltas(
    const std::vector<gar::core::graph::DerivedEdge>& derived,
    std::vector<MatchRequest>* requests, std::vector<DeltaMatchJob>* jobs) {
  for (const auto& d : derived) {
    const std::uint32_t mirror = g_.IsInverse(d.slot)
                                     ? g_.BaseRelation(d.slot)
                                     : d.slot + g_.num_relations();
    std::vector<std::uint32_t> affected;
    for (const std::uint32_t slot : {d.slot, mirror}) {
      const auto it = complete_slot_users_.find(slot);
      if (it != complete_slot_users_.end()) {
        affected.insert(affected.end(), it->second.begin(), it->second.end());
      }
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()),
                   affected.end());
    for (const std::uint32_t pi : affected) {
      if (pi >= next_star_) continue;
      const deduce::RulePlan& pl = plans_[pi];
      for (const std::uint32_t slot : {d.slot, mirror}) {
        const bool flipped = (slot == mirror);
        for (const auto& e : pl.spec.edges) {
          if (e[2] != slot) continue;
          auto pins = GoalPins(pl);
          const std::uint32_t ps = flipped ? d.dst : d.src;
          const std::uint32_t pd = flipped ? d.src : d.dst;
          auto pit = pins.find(e[0]);
          if (pit != pins.end() && pit->second != ps) continue;
          auto qit = pins.find(e[1]);
          if (qit != pins.end() && qit->second != pd) continue;
          pins[e[0]] = ps;
          pins[e[1]] = pd;
          const std::uint32_t edge =
              static_cast<std::uint32_t>(&e - pl.spec.edges.data());
          requests->push_back(MatchRequest{pl.spec, cfg_.match_cap_per_rule,
                                           std::move(pins),
                                           EdgeSeed{e[2], ps, pd, edge}, pi});
          jobs->push_back(DeltaMatchJob{pi, 0, false});
        }
      }
    }
  }
}

void DeduceSession::CompletePartials(const gar::core::graph::DerivedEdge& d,
                                     std::uint32_t mirror) {
  std::vector<std::uint64_t> done;
  for (const std::uint64_t id : ws_.MatchingPartials(d, mirror)) {
    auto* swp = ws_.FindSuspended(id);
    if (swp == nullptr) continue;
    auto& sw = *swp;
    std::uint32_t pi = prod_->PlanOfRule(sw.rule);
    if (pi == UINT32_MAX) continue;
    const deduce::DeferrableEdge& de = plans_[pi].defers[sw.defer_idx];
    std::uint32_t fs, fd;  // the new fact expressed in de.slot
    if (de.slot == d.slot) {
      fs = d.src;
      fd = d.dst;
    } else {
      fs = d.dst;
      fd = d.src;
    }
    bool complete_now = false;
    if (!sw.wait.open_side) {
      complete_now = (sw.wait.ground == KeyOf(MakeEdgeFact(de.slot, fs, fd)));
    } else if (sw.wait.bound_is_src && fs == sw.wait.bound_value) {
      sw.partial[de.pv] = fd;
      complete_now = true;
    } else if (!sw.wait.bound_is_src && fd == sw.wait.bound_value) {
      sw.partial[de.pu] = fs;
      complete_now = true;
    }
    if (!complete_now) continue;
    bool any_unbound = false;
    for (std::uint32_t v : sw.partial) {
      if (v == UINT32_MAX) {
        any_unbound = true;
        break;
      }
    }
    if (any_unbound) continue;  // unreachable with a single deferred edge
    Trace("revive-partial\trule=" + state_->rules()[sw.rule].name +
          "\taction=" + std::to_string(sw.id));
    ++res_.stats->revive_events;
    AddComplete(pi, sw.partial);
    done.push_back(sw.id);
  }
  for (std::uint64_t id : done) ws_.RemoveSuspended(id);
}

void DeduceSession::GrowPartials(
    const std::vector<gar::core::graph::DerivedEdge>& derived,
    std::vector<MatchRequest>* requests, std::vector<DeltaMatchJob>* jobs) {
  for (const auto& d : derived) {
    const std::uint32_t mirror = g_.IsInverse(d.slot)
                                     ? g_.BaseRelation(d.slot)
                                     : d.slot + g_.num_relations();
    // New partials from sub-spec growth; a goal/delta pin conflict makes the
    // intersection empty by construction.
    std::vector<std::uint32_t> affected;
    for (const std::uint32_t slot : {d.slot, mirror}) {
      const auto it = partial_slot_users_.find(slot);
      if (it != partial_slot_users_.end()) {
        affected.insert(affected.end(), it->second.begin(), it->second.end());
      }
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()),
                   affected.end());
    for (const std::uint32_t pi : affected) {
      if (pi >= next_star_) continue;
      for (std::size_t di = 0; di < plans_[pi].defers.size(); ++di) {
        const auto& de = plans_[pi].defers[di];
        for (const std::uint32_t slot : {d.slot, mirror}) {
          const bool flipped = (slot == mirror);
          for (const auto& e : de.sub_spec.edges) {
            if (e[2] != slot) continue;
            auto pins = GoalSubPins(plans_[pi], de);
            const std::uint32_t ps = flipped ? d.dst : d.src;
            const std::uint32_t pd = flipped ? d.src : d.dst;
            auto pit = pins.find(e[0]);
            if (pit != pins.end() && pit->second != ps) continue;
            auto qit = pins.find(e[1]);
            if (qit != pins.end() && qit->second != pd) continue;
            pins[e[0]] = ps;
            pins[e[1]] = pd;
            const std::uint32_t edge =
                static_cast<std::uint32_t>(&e - de.sub_spec.edges.data());
            requests->push_back(
                MatchRequest{de.sub_spec, cfg_.match_cap_per_rule,
                             std::move(pins), EdgeSeed{e[2], ps, pd, edge}, pi});
            jobs->push_back(DeltaMatchJob{pi, di, true});
          }
        }
      }
    }
  }
}

void DeduceSession::OnDerivations(
    const std::vector<gar::core::graph::DerivedEdge>& derived) {
  ++res_.stats->delta_invocations;
  res_.stats->delta_batch_edges += derived.size();
  std::vector<MatchRequest> requests;
  std::vector<DeltaMatchJob> jobs;
  auto started = std::chrono::steady_clock::now();
  CompleteDeltas(derived, &requests, &jobs);
  res_.stats->delta_complete_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  started = std::chrono::steady_clock::now();
  GrowPartials(derived, &requests, &jobs);
  res_.stats->partial_grow_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  res_.stats->delta_match_requests += requests.size();
  started = std::chrono::steady_clock::now();
  const std::vector<MatchResult> results = match_backend_->RunBatch(requests);
  res_.stats->delta_match_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();

  started = std::chrono::steady_clock::now();
  for (std::size_t j = 0; j < results.size(); ++j) {
    if (jobs[j].partial) continue;
    const std::uint32_t pi = jobs[j].pi;
    const auto n = plans_[pi].spec.num_vertices;
    const std::vector<std::uint32_t>& embs = results[j].embeddings;
    if (results[j].capped) ++res_.stats->candidate_caps;
    for (std::uint64_t t = 0; t < embs.size() / n; ++t) {
      AddComplete(pi, std::vector<std::uint32_t>(embs.data() + t * n,
                                                 embs.data() + (t + 1) * n));
    }
  }
  res_.stats->delta_complete_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  started = std::chrono::steady_clock::now();
  for (const auto& d : derived) {
    const std::uint32_t mirror = g_.IsInverse(d.slot)
                                     ? g_.BaseRelation(d.slot)
                                     : d.slot + g_.num_relations();
    CompletePartials(d, mirror);
  }
  res_.stats->partial_complete_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  started = std::chrono::steady_clock::now();
  for (std::size_t j = 0; j < results.size(); ++j) {
    if (!jobs[j].partial) continue;
    const DeltaMatchJob& job = jobs[j];
    if (results[j].capped) ++res_.stats->candidate_caps;
    AddPartialMatches(job.pi, plans_[job.pi].defers[job.defer],
                      results[j].embeddings);
  }
  res_.stats->partial_grow_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  RefreshPeaks();
  VerifyGeneration();
}

void DeduceSession::FlushDerivations() {
  if (pending_derivations_.empty()) return;
  OnDerivations(pending_derivations_);
  pending_derivations_.clear();
}

int DeduceSession::Evaluate(const deduce::ActionRecord& rec) {
  auto& fx = *res_.stats;
  const GARule& r = state_->rules()[rec.motif.rule];
  const std::uint32_t* emb = rec.motif.embedding.data();
  fx.search_cost += kit_.cost_model->RuleLowerBound(r);  // unit: exactly 1
  ++fx.evaluate_events;
  bool static_false = false;
  bool static_false_from_cache = false;
  std::vector<FactKey> missing;
  // Pattern edges are present by construction (the matcher matched them);
  // antecedent predicates are evaluated one by one (no short-circuit).
  for (const gar::Predicate& pred : r.antecedent) {
    const deduce::GroundPreconditionKey pkey =
        deduce::MakePreconditionKey(pred, emb);
    const bool dyn = IsDynamicPredicate(pred, r, ctx_);
    if (auto cached = H_.Lookup(pkey, !dyn, state_->version())) {
      if (*cached) continue;
      if (!dyn) {
        static_false = true;
        static_false_from_cache = true;  // H hit, no fresh evaluation
        continue;  // keep scanning: collect every missing/false
      }
      // Dynamic edge negatives may also wait in W (normalized FactKey).
      Fact ef;
      if (deduce::GroundEdgeFact(pred, emb, g_, &ef)) {
        missing.push_back(KeyOf(ef));
      }
      continue;
    }
    const deduce::PrecondState st = deduce::ClassifyPredicate(
        pred, r, emb, state_->attrs(), g_, ctx_, state_->ml_models());
    const bool ok = st == deduce::PrecondState::kStaticTrue ||
                    st == deduce::PrecondState::kDynamicPresent;
    H_.Record(pkey, ok, !dyn, state_->version());
    if (!ok && !dyn) static_false = true;
    if (!ok && dyn) {
      Fact ef;
      if (deduce::GroundEdgeFact(pred, emb, g_, &ef)) {
        missing.push_back(KeyOf(ef));
      }
    }
  }
  if (static_false) {
    // Permanent prune; shared: every R action whose grounded antecedent
    // contains a static-false precondition (same structured key) goes too.
    std::vector<std::uint64_t> victims;
    for (const deduce::ActionRecord& other : ws_.ready()) {
      const GARule& orule = state_->rules()[other.motif.rule];
      const std::uint32_t* oe = other.motif.embedding.data();
      for (const gar::Predicate& pred : orule.antecedent) {
        const auto okey = deduce::MakePreconditionKey(pred, oe);
        if (auto cached =
                H_.Lookup(okey, /*is_static=*/true, state_->version());
            cached && !*cached) {
          victims.push_back(other.id);
          break;
        }
      }
    }
    for (std::uint64_t id : victims) {
      if (const auto* action = ws_.FindReady(id)) UntrackReady(*action);
      ws_.RemoveReady(id);
      ++fx.prune_events;
      Trace("prune-shared\taction=" + std::to_string(id));
    }
    return static_false_from_cache ? 3 : 0;
  }
  if (!missing.empty()) {
    deduce::SuspendedAction sw;
    sw.rule = rec.motif.rule;
    sw.is_partial = false;
    sw.motif = rec.motif;
    sw.missing = std::move(missing);
    sw.wait_since_round = round_no_;
    auto& ref = ws_.AddSuspended(std::move(sw));
    ++fx.suspend_events;
    Trace("suspend\trule=" + r.name + "\taction=" + std::to_string(ref.id));
    return 1;
  }
  return 2;
}

gar::core::graph::DerivedEdge DeduceSession::Derive(
    const deduce::ActionRecord& rec) {
  auto& fx = *res_.stats;
  const GARule& r = state_->rules()[rec.motif.rule];
  const std::uint32_t* emb = rec.motif.embedding.data();
  const gar::core::graph::DerivedEdge d{
      emb[rec.head.pu], emb[rec.head.pv], rec.head.slot, rec.motif.rule,
      static_cast<std::uint32_t>(res_.chain.size() + 1)};
  const auto ms = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t0 = std::chrono::steady_clock::now();
  state_->ApplyDerivation(d);
  match_backend_->CommitDerived(d);
  const auto t1 = std::chrono::steady_clock::now();
  std::vector<Fact> premises;
  GroundPremises(r, rec.motif, g_, *ctx_.table, &premises);
  const auto t2 = std::chrono::steady_clock::now();
  for (const Fact& f : premises) RecordWitness(f);
  RecordWitness(MakeEdgeFact(d.slot, d.src, d.dst));
  const auto t3 = std::chrono::steady_clock::now();
  AgentStep step;
  step.motif = rec.motif;
  step.success = true;
  step.cost = kit_.cost_model->RuleLowerBound(r);
  step.derived = d;
  step.action_id = rec.id;
  step.handcrafted_score = rec.score.total;
  res_.chain.push_back(std::move(step));
  const auto t4 = std::chrono::steady_clock::now();
  ++fx.derive_events;
  Trace("derive\trule=" + r.name + "\tfact=" + g_.SlotName(d.slot) + "(" +
        g_.EntityName(d.src) + "," + g_.EntityName(d.dst) + ")");
  const auto t5 = std::chrono::steady_clock::now();
  fx.derive_apply_ms += ms(t0, t1);
  fx.derive_ground_ms += ms(t1, t2);
  fx.derive_witness_ms += ms(t2, t3);
  fx.derive_chain_ms += ms(t3, t4);
  fx.derive_trace_ms += ms(t4, t5);
  if (GoalSatisfied(goal_, d)) res_.goal_derived = true;
  return d;
}

double DeduceSession::GoalProofCost() const {
  if (!res_.goal_derived || res_.chain.empty()) return 0.0;
  std::vector<bool> queued(res_.chain.size(), false);
  std::vector<std::size_t> pending;
  for (std::size_t i = res_.chain.size(); i-- > 0;) {
    const auto& derived = res_.chain[i].derived;
    if (derived && GoalSatisfied(goal_, *derived)) {
      queued[i] = true;
      pending.push_back(i);
      break;
    }
  }
  auto derived_key = [](const gar::core::graph::DerivedEdge& edge) {
    return KeyOf(MakeEdgeFact(edge.slot, edge.src, edge.dst));
  };
  while (!pending.empty()) {
    const std::size_t index = pending.back();
    pending.pop_back();
    const AgentStep& step = res_.chain[index];
    std::vector<Fact> premises;
    GroundPremises(state_->rules()[step.motif.rule], step.motif, g_,
                   *ctx_.table, &premises);
    for (const Fact& fact : premises) {
      if (fact.kind != FactKind::kEdge) continue;
      const FactKey wanted = KeyOf(fact);
      for (std::size_t j = index; j-- > 0;) {
        const auto& candidate = res_.chain[j].derived;
        if (!candidate || derived_key(*candidate) != wanted) continue;
        if (!queued[j]) {
          queued[j] = true;
          pending.push_back(j);
        }
        break;
      }
    }
  }
  double cost = 0.0;
  for (std::size_t i = 0; i < res_.chain.size(); ++i) {
    if (queued[i]) cost += res_.chain[i].cost;
  }
  return cost;
}

double DeduceSession::MeasuredCostMs() const {
  const auto& fx = *res_.stats;
  if (std::getenv("GAR_DERIVE_PROFILE") != nullptr) {
    std::fprintf(stderr,
                 "[derive-profile] events=%llu derivation=%.1fms | apply=%.1f "
                 "ground=%.1f witness=%.1f chain=%.1f trace=%.1f | "
                 "prepare=%.1f generation=%.1f selection=%.1f score=%.1f "
                 "eval=%.1f delta=%.1f | gen_match=%.1f gen_add=%.1f\n",
                 (unsigned long long)fx.derive_events, fx.derivation_wall_ms,
                 fx.derive_apply_ms, fx.derive_ground_ms, fx.derive_witness_ms,
                 fx.derive_chain_ms, fx.derive_trace_ms, fx.prepare_wall_ms,
                 fx.generation_wall_ms, fx.selection_wall_ms, fx.score_wall_ms,
                 fx.evaluation_wall_ms, fx.delta_wall_ms, fx.gen_match_ms,
                 fx.gen_add_ms);
  }
  return fx.prepare_wall_ms + fx.generation_wall_ms + fx.selection_wall_ms +
         fx.ready_remove_wall_ms + fx.evaluation_wall_ms +
         fx.derivation_wall_ms + fx.delta_wall_ms + fx.revival_wall_ms;
}

bool DeduceSession::RevivalPass() {
  auto& fx = *res_.stats;
  ++round_no_;
  // Refresh producer pending flags (a rule is pending iff it owns entries
  // in R or W).
  for (std::uint32_t pi = 0; pi < plans_.size(); ++pi) {
    prod_->SetPending(pi, false);
  }
  for (const auto& sw : ws_.suspended()) {
    prod_->SetPending(prod_->PlanOfRule(sw.rule), true);
  }
  std::uint64_t progressed = 0;
  std::vector<std::uint64_t> drop;
  std::vector<deduce::ActionRecord> completed;
  for (auto& sw : ws_.suspended()) {
    bool fulfilled = true;
    bool any_alive_producer = false;
    if (!sw.is_partial) {
      for (const FactKey& k : sw.missing) {
        // W wait keys are normalized edge FactKeys: (kind|base slot, src,
        // dst) — check Γ directly (a present fact stays present).
        const std::uint32_t slot =
            static_cast<std::uint32_t>(k.hi & 0xffffffffu);
        const std::uint32_t s = static_cast<std::uint32_t>(k.lo >> 32);
        const std::uint32_t d = static_cast<std::uint32_t>(k.lo);
        if (g_.RelationPairSet(slot).count(gar::core::graph::PackPair(s, d)) ==
            0) {
          fulfilled = false;
          const std::uint32_t pid = ctx_.table->Find(g_.RelationName(
              slot < g_.num_relations() ? slot : slot - g_.num_relations()));
          if (pid != gar::core::analysis::kInvalidPredicate &&
              prod_->Alive(pid, state_->version())) {
            any_alive_producer = true;
          }
        }
      }
      if (fulfilled) {
        ++fx.revive_events;
        Trace("revive\taction=" + std::to_string(sw.id));
        const std::uint32_t pi = prod_->PlanOfRule(sw.rule);
        if (pi != UINT32_MAX) {
          deduce::ActionRecord rec;
          rec.motif = sw.motif;
          rec.head = plans_[pi].head;
          completed.push_back(std::move(rec));
        }
        drop.push_back(sw.id);
        ++progressed;
      } else if (!sw.missing.empty() && !any_alive_producer) {
        ++fx.prune_events;
        Trace("prune-dead\taction=" + std::to_string(sw.id));
        drop.push_back(sw.id);
        ++progressed;
      }
      continue;
    }
    // Partial waits: check current presence of the deferred edge.
    const std::uint32_t pii = prod_->PlanOfRule(sw.rule);
    if (pii == UINT32_MAX) continue;
    const deduce::DeferrableEdge& de = plans_[pii].defers[sw.defer_idx];
    bool present = false;
    if (!sw.wait.open_side) {
      const std::uint32_t s =
          static_cast<std::uint32_t>(sw.wait.ground.lo >> 32);
      const std::uint32_t dd = static_cast<std::uint32_t>(sw.wait.ground.lo);
      const std::uint32_t slot =
          static_cast<std::uint32_t>(sw.wait.ground.hi & 0xffffffffu);
      present = g_.RelationPairSet(slot).count(
                    gar::core::graph::PackPair(s, dd)) != 0;
    } else {
      // Open-side: any fact with the bound endpoint now?
      const auto& pairs = g_.RelationPairs(sw.wait.slot);
      const std::uint32_t bv = sw.wait.bound_value;
      if (sw.wait.bound_is_src) {
        auto lo = std::lower_bound(pairs.begin(), pairs.end(),
                                   gar::core::graph::PackPair(bv, 0));
        present = lo != pairs.end() && (*lo >> 32) == bv;
      } else {
        const std::uint32_t mirror = g_.IsInverse(sw.wait.slot)
                                         ? g_.BaseRelation(sw.wait.slot)
                                         : sw.wait.slot + g_.num_relations();
        const auto& mp = g_.RelationPairs(mirror);
        auto lo = std::lower_bound(mp.begin(), mp.end(),
                                   gar::core::graph::PackPair(bv, 0));
        present = lo != mp.end() && (*lo >> 32) == bv;
      }
    }
    if (!present) {
      const std::uint32_t pid = ctx_.table->Find(
          gar::core::analysis::LabelNameOf(de.slot, state_->rules()[sw.rule]));
      if (pid == gar::core::analysis::kInvalidPredicate ||
          !prod_->Alive(pid, state_->version())) {
        ++fx.prune_events;
        Trace("prune-dead\taction=" + std::to_string(sw.id));
        drop.push_back(sw.id);
        ++progressed;
      }
    }
    // Present-but-not-completed partials complete via the delta path
    // (OnDerivation), not here.
  }
  for (std::uint64_t id : drop) ws_.RemoveSuspended(id);
  for (const auto& rec : completed) {
    if (state_->ContainsEdge(rec.head.slot, rec.motif.embedding[rec.head.pu],
                             rec.motif.embedding[rec.head.pv])) {
      continue;
    }
    const auto derived = Derive(rec);
    OnDerivations({derived});
    if (res_.goal_derived) break;
  }
  return progressed > 0;
}

void DeduceSession::WritePolicyTrace() {
  if (!policy_trace_) return;
  policy_trace_->goal_derived = res_.goal_derived;
  if (res_.goal_derived) {
    std::unordered_map<FactKey, std::size_t, FactKeyHash> by_fact;
    for (std::size_t i = 0; i < res_.chain.size(); ++i) {
      if (res_.chain[i].derived) {
        const auto& derived = *res_.chain[i].derived;
        by_fact.emplace(
            KeyOf(MakeEdgeFact(derived.slot, derived.src, derived.dst)), i);
      }
    }
    std::vector<bool> proof(res_.chain.size(), false);
    std::vector<std::size_t> pending;
    for (std::size_t i = res_.chain.size(); i-- > 0;) {
      if (res_.chain[i].derived &&
          GoalSatisfied(goal_, *res_.chain[i].derived)) {
        proof[i] = true;
        pending.push_back(i);
        break;
      }
    }
    while (!pending.empty()) {
      const std::size_t index = pending.back();
      pending.pop_back();
      const AgentStep& step = res_.chain[index];
      std::vector<Fact> premises;
      GroundPremises(state_->rules()[step.motif.rule], step.motif, g_,
                     *ctx_.table, &premises);
      for (const Fact& premise : premises) {
        const auto it = by_fact.find(KeyOf(premise));
        if (it == by_fact.end() || proof[it->second]) continue;
        proof[it->second] = true;
        pending.push_back(it->second);
      }
    }
    auto append_candidate = [&](std::size_t index) {
      const AgentStep& step = res_.chain[index];
      const auto& derived = *step.derived;
      policy_trace_->candidates.push_back(learning::CandidateSnapshot{
          index, step.action_id,
          MakeEdgeFact(derived.slot, derived.src, derived.dst),
          step.handcrafted_score, true});
    };
    for (std::size_t i = 0; i < res_.chain.size(); ++i) {
      if (!proof[i]) continue;
      const AgentStep& step = res_.chain[i];
      std::vector<Fact> premises;
      GroundPremises(state_->rules()[step.motif.rule], step.motif, g_,
                     *ctx_.table, &premises);
      const auto& derived = *step.derived;
      policy_trace_->derivations.push_back(learning::DerivationRecord{
          step.action_id, MakeEdgeFact(derived.slot, derived.src, derived.dst),
          premises});
      append_candidate(i);
    }
    std::vector<std::size_t> negatives;
    for (std::size_t i = 0; i < res_.chain.size(); ++i) {
      if (res_.chain[i].derived && !proof[i]) negatives.push_back(i);
    }
    std::mt19937_64 rng(cfg_.seed);
    std::shuffle(negatives.begin(), negatives.end(), rng);
    negatives.resize(std::min<std::size_t>(cfg_.policy_trace_negative_count,
                                           negatives.size()));
    for (const std::size_t index : negatives) append_candidate(index);
  }
  learning::WritePolicyTrace(cfg_.policy_trace_path, *policy_trace_);
}

void DeduceSession::WriteDecisionTrace(const deduce::ActionRecord& chosen) {
  if (!decision_trace_.is_open()) return;
  auto write = [&](const deduce::ActionRecord& rec, bool is_chosen) {
    decision_trace_ << decision_index_ << '\t' << is_chosen << '\t'
                    << res_.stats->evaluate_events << '\t' << rec.id << '\t'
                    << rec.motif.rule << '\t';
    for (std::size_t i = 0; i < rec.motif.embedding.size(); ++i) {
      if (i != 0) decision_trace_ << ',';
      decision_trace_ << rec.motif.embedding[i];
    }
    for (const double value : ActionFeatures(rec)) {
      decision_trace_ << '\t' << value;
    }
    decision_trace_ << '\n';
  };
  write(chosen, true);
  if (cfg_.decision_sample_stride != 0 &&
      decision_index_ % cfg_.decision_sample_stride == 0) {
    std::vector<const deduce::ActionRecord*> alternatives;
    for (const auto& rec : ws_.ready()) {
      if (rec.id == chosen.id || !rec.score.admissible) continue;
      const std::uint32_t src = rec.motif.embedding[rec.head.pu];
      const std::uint32_t dst = rec.motif.embedding[rec.head.pv];
      if (state_->ContainsEdge(rec.head.slot, src, dst)) continue;
      alternatives.push_back(&rec);
    }
    std::sort(alternatives.begin(), alternatives.end(),
              [](const deduce::ActionRecord* a, const deduce::ActionRecord* b) {
                if (a->selection_priority != b->selection_priority) {
                  return a->selection_priority > b->selection_priority;
                }
                return a->id < b->id;
              });
    const std::size_t count = std::min<std::size_t>(
        cfg_.decision_alternative_count, alternatives.size());
    for (std::size_t i = 0; i < count; ++i) write(*alternatives[i], false);
  }
  ++decision_index_;
}

deduce::ActionRecord* DeduceSession::SelectReady() {
  auto& fx = *res_.stats;
  if (const auto forced = kit_.policy->ForcedAction()) {
    auto* chosen = ws_.FindReady(*forced);
    if (chosen == nullptr) {
      throw std::runtime_error("forced replay action is not ready: " +
                               std::to_string(*forced));
    }
    ++fx.select_events;
    kit_.policy->OnSelection(chosen->id);
    WriteDecisionTrace(*chosen);
    Trace("select\taction=" + std::to_string(chosen->id));
    return chosen;
  }
  if (cfg_.fair_interval > 0 && selects_since_fair_ >= cfg_.fair_interval) {
    selects_since_fair_ = 0;
    auto* chosen = &*std::min_element(
        ws_.ready().begin(), ws_.ready().end(),
        [](const deduce::ActionRecord& a, const deduce::ActionRecord& b) {
          return a.seq < b.seq;
        });
    ++fx.fair_select_events;
    Trace("fair_select\taction=" + std::to_string(chosen->id));
    return chosen;
  }
  deduce::ActionRecord* chosen = nullptr;
  if (IncrementalScores() && kit_.policy->PrefersHighestScore()) {
    while ((chosen = ws_.HighestReady()) != nullptr) {
      const bool head_known = state_->ContainsEdge(
          chosen->head.slot, chosen->motif.embedding.data()[chosen->head.pu],
          chosen->motif.embedding.data()[chosen->head.pv]);
      const bool admissible = chosen->score.admissible && !head_known;
      if (admissible != chosen->score.admissible) {
        chosen->score.admissible = admissible;
        ws_.TouchReady(chosen->id);
        continue;
      }
      // A known head is removed by Run's zero-cost skip-known path. Filtering
      // it only from the heap would strand R nonempty and block later stars.
      if (kit_.policy->RequiresAdmissible() && !admissible && !head_known) {
        ws_.DiscardHighestReady();
        chosen = nullptr;
        continue;
      }
      break;
    }
  } else {
    std::optional<std::size_t> pick;
    if (IncrementalScores()) {
      std::vector<double> totals;
      std::vector<char> admissible;
      totals.reserve(ws_.ready().size());
      admissible.reserve(ws_.ready().size());
      for (auto& rec : ws_.ready()) {
        rec.score.admissible =
            rec.score.admissible &&
            !state_->ContainsEdge(rec.head.slot,
                                  rec.motif.embedding.data()[rec.head.pu],
                                  rec.motif.embedding.data()[rec.head.pv]);
        totals.push_back(rec.score.total);
        admissible.push_back(rec.score.admissible);
      }
      pick = kit_.policy->SelectScores(totals, admissible);
    } else {
      std::vector<Motif> mus;
      mus.reserve(ws_.ready().size());
      for (const auto& rec : ws_.ready()) mus.push_back(rec.motif);
      std::vector<ScoredMotif> scored;
      const auto score_started = std::chrono::steady_clock::now();
      kit_.scorer->ScoreBatch(state_->rules(), mus, sctx_, &scored);
      fx.score_wall_ms += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - score_started)
                              .count();
      ++fx.score_calls;
      fx.scored_candidates += mus.size();
      pick = kit_.policy->Select(scored);
    }
    if (pick) chosen = &ws_.ready()[*pick];
  }
  ++fx.select_events;
  if (chosen == nullptr) return nullptr;
  kit_.policy->OnSelection(chosen->id);
  WriteDecisionTrace(*chosen);
  Trace("select\taction=" + std::to_string(chosen->id));
  return chosen;
}

AgentResult DeduceSession::Run() {
  res_.stats.emplace();
  t0_ = std::chrono::steady_clock::now();
  verify_ = std::getenv("GAR_VERIFY_DEDUCE") != nullptr;
  if (!cfg_.policy_trace_path.empty()) {
    const auto* score_config = kit_.scorer->handcrafted_config();
    if (!IncrementalScores() || score_config == nullptr) {
      throw std::invalid_argument(
          "policy tracing requires exact incremental scoring");
    }
    policy_trace_.emplace();
    policy_trace_->goal = goal_;
    policy_trace_->hdc = {ctx_.encoder.dim(), ctx_.encoder.seed()};
    policy_trace_->scorer = *score_config;
    policy_trace_->graph_fingerprint = GraphFingerprint(g_);
  }
  if (!cfg_.decision_trace_path.empty()) {
    const std::filesystem::path path(cfg_.decision_trace_path);
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }
    decision_trace_.open(path);
    if (!decision_trace_) {
      throw std::runtime_error("cannot open decision trace: " +
                               cfg_.decision_trace_path);
    }
    decision_trace_ << std::setprecision(17)
                    << "# decision\tchosen\tevaluations_before\taction_id"
                    << "\trule\tembedding\tprice\tpattern_edges"
                    << "\tlog_support\trelfam\trelevd\tconf\tdcost\ttotal"
                    << "\tdp0\twit\tpremises\n";
  }

  if (goal_.args[0] && goal_.args[1] &&
      state_->ContainsEdge(goal_.slot, *goal_.args[0], *goal_.args[1])) {
    res_.goal_derived = true;
    res_.termination = "goal_already_in_graph";
    WritePolicyTrace();
    return res_;
  }

  const auto prepared_at = std::chrono::steady_clock::now();
  BuildPlans();
  const auto plans_at = std::chrono::steady_clock::now();
  match_backend_ = MakeMatchBackend(cfg_.engine, g_);
  const auto backend_at = std::chrono::steady_clock::now();
  res_.stats->prepare_wall_ms +=
      std::chrono::duration<double, std::milli>(backend_at - prepared_at)
          .count();
  if (std::getenv("GAR_DERIVE_PROFILE") != nullptr) {
    std::fprintf(stderr,
                 "[prepare-profile] BuildPlans=%.1fms MakeMatchBackend=%.1fms "
                 "rules=%zu\n",
                 std::chrono::duration<double, std::milli>(plans_at -
                                                           prepared_at)
                     .count(),
                 std::chrono::duration<double, std::milli>(backend_at -
                                                           plans_at)
                     .count(),
                 state_->rules().size());
  }
  res_.termination = "fixpoint";

  // Star discipline (approved algorithm): expand ONE star on the current
  // G∪Γ, drain R via policy, then the next star; deltas from derivations
  // target only already-expanded stars (unexpanded stars' first-turn full
  // enumeration covers any new facts). The W revival pass runs after all stars,
  // or earlier when the configured soft suspension threshold is reached.
  while (true) {
    if (WallExceeded()) {
      res_.termination = "timeout";
      break;
    }
    if (BudgetHit()) {
      res_.termination = "budget";
      break;
    }
    if (ws_.ReadyEmpty()) {
      FlushDerivations();
      if (!ws_.ReadyEmpty()) continue;
      if (next_star_ < plans_.size() && cfg_.revival_threshold > 0 &&
          ws_.suspended().size() >= cfg_.revival_threshold) {
        const auto started = std::chrono::steady_clock::now();
        const bool progressed = RevivalPass();
        ++res_.stats->revival_passes;
        res_.stats->revival_wall_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        if (res_.goal_derived) {
          res_.termination = "goal";
          break;
        }
        if (progressed && !ws_.ReadyEmpty()) continue;
      }
      if (next_star_ < plans_.size()) {
        const auto started = std::chrono::steady_clock::now();
        GenerateStar(next_star_++);
        ++res_.stats->generation_events;
        res_.stats->generation_wall_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        continue;  // drain this star's R before the next star
      }
      const auto started = std::chrono::steady_clock::now();
      const bool progressed = RevivalPass();
      ++res_.stats->revival_passes;
      res_.stats->revival_wall_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started)
              .count();
      if (res_.goal_derived) {
        res_.termination = "goal";
        break;
      }
      if (!progressed) {
        res_.termination =
            res_.stats->candidate_caps > 0 ? "candidate_cap" : "fixpoint";
        break;
      }
      continue;
    }

    std::vector<deduce::ActionRecord> batch;
    const auto selection_started = std::chrono::steady_clock::now();
    if (cfg_.batch_size == 1) {
      deduce::ActionRecord* chosen = SelectReady();
      if (chosen != nullptr) batch.push_back(*chosen);
    } else {
      batch = SelectReadyBatch();
    }
    res_.stats->selection_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - selection_started)
            .count();
    if (batch.empty()) {
      res_.termination = "no_candidates";
      break;
    }
    ++selects_since_fair_;

    // Selection uses one batch-start state. Remove every selected record before
    // any member can derive facts and trigger new scores or delta candidates.
    for (const auto& rec : batch) {
      const auto removal_started = std::chrono::steady_clock::now();
      UntrackReady(rec);
      ws_.RemoveReady(rec.id);
      res_.stats->ready_remove_wall_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - removal_started)
              .count();
    }
    if (batch.size() > 1) {
      const auto started = std::chrono::steady_clock::now();
      PrepareStaticBatch(batch);
      res_.stats->evaluation_wall_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started)
              .count();
    }
    for (const auto& rec : batch) {
      if (BudgetHit()) {
        res_.termination = "budget";
        break;
      }
      if (ProcessAction(rec)) {
        res_.termination = "goal";
        break;
      }
    }
    if (res_.goal_derived || res_.termination == "budget") break;
  }

  res_.logical_cost = res_.stats->search_cost;
  res_.execution_cost_ms = MeasuredCostMs();
  res_.stats->proof_cost = GoalProofCost();
  res_.budget_exhausted = res_.termination == "budget";
  res_.derived = state_->journal();
  if (kit_.relevance_model != nullptr) {
    const auto& model = *kit_.relevance_model;
    std::fprintf(
        stderr,
        "[ada-deduce] relevance-model: calls=%llu infer_ms=%.3f avg_us=%.3f\n",
        static_cast<unsigned long long>(model.infer_calls()),
        model.infer_ns() / 1.0e6,
        model.infer_calls() == 0
            ? 0.0
            : model.infer_ns() / 1.0e3 / model.infer_calls());
  }
  WritePolicyTrace();
  return res_;
}

}  // namespace agent
}  // namespace gar::core::engine
