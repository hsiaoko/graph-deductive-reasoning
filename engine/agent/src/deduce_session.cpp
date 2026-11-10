#include "deduce_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "gar/engine/agent/value_model.h"
#include "gar/graph/predicate_evaluator.h"
#include "rule_support.h"

namespace gar {
namespace engine {
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
  const auto& rules = state_->rules();
  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    deduce::RulePlan plan;
    if (deduce::BuildRulePlan(rules[i], i, ctx_, g_, &plan)) {
      plans_.push_back(std::move(plan));
    }
  }
  if (cfg_.star_order == "goal") {
    auto head_distance = [&](const deduce::RulePlan& plan) {
      const GARule& rule = rules[plan.rule];
      const std::uint32_t predicate = ctx_.table->Find(
          analysis::PredicateIdOf(*rule.consequent, rule));
      return predicate == analysis::kInvalidPredicate
                 ? analysis::kUnreachable
                 : ctx_.distances[predicate];
    };
    std::stable_sort(plans_.begin(), plans_.end(),
                     [&](const deduce::RulePlan& a,
                         const deduce::RulePlan& b) {
                       return head_distance(a) < head_distance(b);
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
        analysis::PredicateIdOf(*rule.consequent, rule));
    // A recursive goal may need same-predicate intermediate facts before the
    // final constants can be derived. Pinning every complete match to the
    // final head would exclude those necessary actions.
    if (predicate != analysis::kInvalidPredicate &&
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
          static_cast<double>(rule.pattern.num_edges()),
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
  rec->selection_priority = kit_.policy->SelectionPriorityWithFeatures(
      rec->id, rec->score.total, ActionFeatures(*rec));
  if (kit_.policy->PrefersHighestScore()) ws_.TouchReady(rec->id);
}

void DeduceSession::WriteQTrace() const {
  if (cfg_.q_trace_path.empty()) return;
  const std::filesystem::path path(cfg_.q_trace_path);
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("cannot write Q trace: " +
                             cfg_.q_trace_path);
  }
  out << "# event\tgoal_derived\tremaining_evaluations\taction_id\trule"
      << "\tprice\tpattern_edges\tlog_support\trelfam\trelevd\tconf"
      << "\tdcost\ttotal\tdp0\twit\tpremises\n";
  out.precision(17);
  for (std::size_t i = 0; i < q_trace_rows_.size(); ++i) {
    const QTraceRow& row = q_trace_rows_[i];
    const std::uint64_t remaining =
        res_.goal_derived ? q_trace_rows_.size() - i : 0;
    out << i << '\t' << (res_.goal_derived ? 1 : 0) << '\t' << remaining
        << '\t' << row.action_id << '\t' << row.rule;
    for (const double value : row.features) out << '\t' << value;
    out << '\n';
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
    return;  // conclusion already in Γ: nothing new
  }
  if (!seen_actions_.Insert(pl.rule, emb)) return;
  auto& rec = EnqueueReady(Motif{pl.rule, emb}, pl.head);
  Trace("gen\trule=" + r.name + "\taction=" + std::to_string(rec.id));
  prod_->SetPending(pi, true);
}

void DeduceSession::AddPartials(
    std::uint32_t pi, const deduce::DeferrableEdge& de,
    const std::unordered_map<std::uint32_t, std::uint32_t>& sub_pins) {
  const deduce::RulePlan& pl = plans_[pi];
  const GARule& r = state_->rules()[pl.rule];
  std::vector<std::uint32_t> embs;
  bool capped = false;
  const std::uint64_t n_emb = matcher_.Enumerate(
      de.sub_spec, cfg_.match_cap_per_rule, &embs, &capped, sub_pins);
  if (capped) ++res_.stats->candidate_caps;
  const auto ns = de.sub_spec.num_vertices;
  for (std::uint64_t t = 0; t < n_emb; ++t) {
    const std::uint32_t* se = embs.data() + t * ns;
    std::vector<std::uint32_t> full =
        deduce::ExpandPartial(de, se, static_cast<std::uint32_t>(
                                            r.pattern.num_vertices()));
    if (!VertexLabelsOk(r, g_, full.data())) continue;
    // Deferred edge satisfaction on current Γ.
    const std::uint32_t eu = full[de.pu], ev = full[de.pv];
    const bool u_bound = eu != UINT32_MAX, v_bound = ev != UINT32_MAX;
    bool present = false;
    if (u_bound && v_bound) {
      present = g_.RelationPairSet(de.slot).count(graph::PackPair(eu, ev)) != 0;
    } else if (u_bound) {
      const auto& pairs = g_.RelationPairs(de.slot);
      auto lo =
          std::lower_bound(pairs.begin(), pairs.end(), graph::PackPair(eu, 0));
      present = lo != pairs.end() && (*lo >> 32) == eu;
    } else if (v_bound) {
      // dst-side presence: the mirror slot's pairs are (dst, src).
      const std::uint32_t mirror = g_.IsInverse(de.slot)
                                       ? g_.BaseRelation(de.slot)
                                       : de.slot + g_.num_relations();
      const auto& pairs = g_.RelationPairs(mirror);
      auto lo =
          std::lower_bound(pairs.begin(), pairs.end(), graph::PackPair(ev, 0));
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
    Trace("suspend-partial\trule=" + r.name +
          "\twait_slot=" + g_.SlotName(de.slot) +
          "\taction=" + std::to_string(ref.id));
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
  for (std::uint32_t pi = 0; pi < next_star_; ++pi) {
    const deduce::RulePlan& pl = plans_[pi];
    const GARule& r = state_->rules()[pl.rule];
    std::vector<std::uint32_t> embs;
    bool capped = false;
    const std::uint64_t n_emb = matcher_.Enumerate(pl.spec, UINT64_MAX, &embs,
                                                   &capped, GoalPins(pl));
    const auto n = pl.spec.num_vertices;
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
        std::fprintf(stderr, "[verify-fixpoint] MISSING rule=%s emb#%llu\n",
                     r.name.substr(0, 80).c_str(),
                     static_cast<unsigned long long>(t));
        std::abort();
      }
    }
  }
}

void DeduceSession::GenerateStar(std::uint32_t pi) {
  const deduce::RulePlan& pl = plans_[pi];
  std::vector<std::uint32_t> embs;
  bool capped = false;
  const std::uint64_t n_emb = matcher_.Enumerate(
      pl.spec, cfg_.match_cap_per_rule, &embs, &capped, GoalPins(pl));
  if (capped) ++res_.stats->candidate_caps;
  const auto n = pl.spec.num_vertices;
  for (std::uint64_t t = 0; t < n_emb; ++t) {
    AddComplete(pi, std::vector<std::uint32_t>(embs.data() + t * n,
                                               embs.data() + (t + 1) * n));
  }
  for (const auto& de : pl.defers) {
    AddPartials(pi, de, GoalSubPins(pl, de));
  }
  prod_->MarkGenerated(pi);
  RefreshPeaks();
  VerifyGeneration();
}

void DeduceSession::CompleteDeltas(const graph::DerivedEdge& d,
                                     std::uint32_t mirror) {
  std::vector<std::uint32_t> affected;
  for (const std::uint32_t slot : {d.slot, mirror}) {
    const auto it = complete_slot_users_.find(slot);
    if (it != complete_slot_users_.end()) {
      affected.insert(affected.end(), it->second.begin(), it->second.end());
    }
  }
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
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
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher_.Enumerate(
            pl.spec, cfg_.match_cap_per_rule, &embs, &capped, pins);
        if (capped) ++res_.stats->candidate_caps;
        const auto n = pl.spec.num_vertices;
        for (std::uint64_t t = 0; t < n_emb; ++t) {
          AddComplete(pi, std::vector<std::uint32_t>(embs.data() + t * n,
                                                     embs.data() +
                                                         (t + 1) * n));
        }
      }
    }
  }
}

void DeduceSession::CompletePartials(const graph::DerivedEdge& d,
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
      complete_now =
          (sw.wait.ground == KeyOf(MakeEdgeFact(de.slot, fs, fd)));
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

void DeduceSession::GrowPartials(const graph::DerivedEdge& d,
                                   std::uint32_t mirror) {
  // New partials from sub-spec growth (goal pins merged; a conflict between
  // a goal pin and the delta pin on the same vertex voids that edge's
  // delta — the intersection is empty by construction).
  std::vector<std::uint32_t> affected;
  for (const std::uint32_t slot : {d.slot, mirror}) {
    const auto it = partial_slot_users_.find(slot);
    if (it != partial_slot_users_.end()) {
      affected.insert(affected.end(), it->second.begin(), it->second.end());
    }
  }
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
  for (const std::uint32_t pi : affected) {
    if (pi >= next_star_) continue;
    for (const auto& de : plans_[pi].defers) {
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
          AddPartials(pi, de, pins);
        }
      }
    }
  }
}

void DeduceSession::OnDerivation(const graph::DerivedEdge& d) {
  const std::uint32_t mirror = g_.IsInverse(d.slot)
                                   ? g_.BaseRelation(d.slot)
                                   : d.slot + g_.num_relations();
  auto started = std::chrono::steady_clock::now();
  CompleteDeltas(d, mirror);
  res_.stats->delta_complete_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  started = std::chrono::steady_clock::now();
  CompletePartials(d, mirror);
  res_.stats->partial_complete_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  started = std::chrono::steady_clock::now();
  GrowPartials(d, mirror);
  res_.stats->partial_grow_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started)
          .count();
  RefreshPeaks();
  VerifyGeneration();
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
    const deduce::PrecondState st =
        deduce::ClassifyPredicate(pred, r, emb, state_->attrs(), g_, ctx_);
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

graph::DerivedEdge DeduceSession::Derive(const deduce::ActionRecord& rec) {
  auto& fx = *res_.stats;
  const GARule& r = state_->rules()[rec.motif.rule];
  const std::uint32_t* emb = rec.motif.embedding.data();
  const graph::DerivedEdge d{
      emb[rec.head.pu], emb[rec.head.pv], rec.head.slot, rec.motif.rule,
      static_cast<std::uint32_t>(res_.chain.size() + 1)};
  state_->ApplyDerivation(d);
  std::vector<Fact> premises;
  GroundPremises(r, rec.motif, g_, *ctx_.table, &premises);
  for (const Fact& f : premises) RecordWitness(f);
  RecordWitness(MakeEdgeFact(d.slot, d.src, d.dst));
  AgentStep step;
  step.motif = rec.motif;
  step.success = true;
  step.cost = kit_.cost_model->RuleLowerBound(r);
  step.derived = d;
  res_.chain.push_back(std::move(step));
  fx.proof_cost += step.cost;
  ++fx.derive_events;
  Trace("derive\trule=" + r.name + "\tfact=" + g_.SlotName(d.slot) + "(" +
        g_.EntityName(d.src) + "," + g_.EntityName(d.dst) + ")");
  if (GoalSatisfied(goal_, d)) res_.goal_derived = true;
  return d;
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
  for (auto& sw : ws_.suspended()) {
    bool fulfilled = true;
    bool any_alive_producer = false;
    if (!sw.is_partial) {
      for (const FactKey& k : sw.missing) {
        // W wait keys are normalized edge FactKeys: (kind|base slot, src,
        // dst) — check Γ directly (a present fact stays present).
        const std::uint32_t slot = static_cast<std::uint32_t>(k.hi & 0xffffffffu);
        const std::uint32_t s = static_cast<std::uint32_t>(k.lo >> 32);
        const std::uint32_t d = static_cast<std::uint32_t>(k.lo);
        if (g_.RelationPairSet(slot).count(graph::PackPair(s, d)) == 0) {
          fulfilled = false;
          const std::uint32_t pid = ctx_.table->Find(g_.RelationName(
              slot < g_.num_relations() ? slot : slot - g_.num_relations()));
          if (pid != analysis::kInvalidPredicate &&
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
          EnqueueReady(std::move(sw.motif), plans_[pi].head);
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
      present = g_.RelationPairSet(slot).count(graph::PackPair(s, dd)) != 0;
    } else {
      // Open-side: any fact with the bound endpoint now?
      const auto& pairs = g_.RelationPairs(sw.wait.slot);
      const std::uint32_t bv = sw.wait.bound_value;
      if (sw.wait.bound_is_src) {
        auto lo = std::lower_bound(pairs.begin(), pairs.end(),
                                   graph::PackPair(bv, 0));
        present = lo != pairs.end() && (*lo >> 32) == bv;
      } else {
        const std::uint32_t mirror =
            g_.IsInverse(sw.wait.slot)
                ? g_.BaseRelation(sw.wait.slot)
                : sw.wait.slot + g_.num_relations();
        const auto& mp = g_.RelationPairs(mirror);
        auto lo = std::lower_bound(mp.begin(), mp.end(),
                                   graph::PackPair(bv, 0));
        present = lo != mp.end() && (*lo >> 32) == bv;
      }
    }
    if (!present) {
      const std::uint32_t pid = ctx_.table->Find(
          analysis::LabelNameOf(de.slot, state_->rules()[sw.rule]));
      if (pid == analysis::kInvalidPredicate ||
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
  return progressed > 0;
}

deduce::ActionRecord* DeduceSession::SelectReady() {
  auto& fx = *res_.stats;
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
            !state_->ContainsEdge(
                rec.head.slot, rec.motif.embedding.data()[rec.head.pu],
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
  Trace("select\taction=" + std::to_string(chosen->id));
  return chosen;
}

AgentResult DeduceSession::Run() {
  res_.stats.emplace();
  t0_ = std::chrono::steady_clock::now();
  verify_ = std::getenv("GAR_VERIFY_DEDUCE") != nullptr;

  if (goal_.args[0] && goal_.args[1] &&
      state_->ContainsEdge(goal_.slot, *goal_.args[0], *goal_.args[1])) {
    res_.goal_derived = true;
    res_.termination = "goal_already_in_graph";
    return res_;
  }

  const auto prepared_at = std::chrono::steady_clock::now();
  BuildPlans();
  res_.stats->prepare_wall_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - prepared_at)
          .count();
  res_.termination = "fixpoint";

  // Star discipline (approved algorithm): expand ONE star on the current
  // G∪Γ, drain R via policy, then the next star; deltas from derivations
  // target only already-expanded stars (unexpanded stars' first-turn full
  // enumeration covers any new facts). The W revival pass runs
  // only once every star has been expanded and R is empty.
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
      if (!progressed) {
        res_.termination = res_.stats->candidate_caps > 0
                               ? "candidate_cap"
                               : "fixpoint";
        break;
      }
      continue;
    }

    const auto selection_started = std::chrono::steady_clock::now();
    deduce::ActionRecord* chosen = SelectReady();
    res_.stats->selection_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - selection_started)
            .count();
    if (chosen == nullptr) {
      // Policy declined the whole R: nothing else can progress here.
      res_.termination = "no_candidates";
      break;
    }
    ++selects_since_fair_;
    const deduce::ActionRecord rec = *chosen;  // copy; ws_ may reallocate
    const auto removal_started = std::chrono::steady_clock::now();
    UntrackReady(rec);
    ws_.RemoveReady(rec.id);
    res_.stats->ready_remove_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - removal_started)
            .count();
    // The conclusion may have been derived by another action since
    // generation; skip without charging (Γ only grows, so this is final).
    if (state_->ContainsEdge(rec.head.slot,
                             rec.motif.embedding.data()[rec.head.pu],
                             rec.motif.embedding.data()[rec.head.pv])) {
      Trace("skip-known\taction=" + std::to_string(rec.id));
      continue;
    }
    const auto evaluation_started = std::chrono::steady_clock::now();
    const int verdict = Evaluate(rec);
    res_.stats->evaluation_wall_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - evaluation_started)
            .count();
    if (!cfg_.q_trace_path.empty()) {
      q_trace_rows_.push_back(
          QTraceRow{ActionFeatures(rec), rec.id, rec.motif.rule});
    }
    if (verdict == 2) {
      const auto derivation_started = std::chrono::steady_clock::now();
      const graph::DerivedEdge d = Derive(rec);
      res_.stats->derivation_wall_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - derivation_started)
              .count();
      if (res_.goal_derived) {
        // Paper line 8: return immediately — no unrelated delta generation
        // after the goal is derived.
        res_.termination = "goal";
        break;
      }
      const auto delta_started = std::chrono::steady_clock::now();
      OnDerivation(d);
      res_.stats->delta_wall_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - delta_started)
              .count();
    } else if (verdict == 0 || verdict == 3) {
      auto& fx = *res_.stats;
      ++fx.prune_events;
      Trace(verdict == 3 ? "prune-cached\taction=" + std::to_string(rec.id)
                         : "prune\taction=" + std::to_string(rec.id));
    }
    // verdict == 1: suspended inside Evaluate.
  }

  res_.total_cost = res_.stats->search_cost;
  res_.budget_exhausted = res_.termination == "budget";
  res_.derived = state_->journal();
  WriteQTrace();
  if (kit_.value_model != nullptr) {
    const auto& model = *kit_.value_model;
    std::fprintf(stderr,
                 "[ada-deduce] value-model: calls=%llu infer_ms=%.3f avg_us=%.3f\n",
                 static_cast<unsigned long long>(model.infer_calls),
                 model.infer_ns / 1.0e6,
                 model.infer_calls == 0
                     ? 0.0
                     : model.infer_ns / 1.0e3 / model.infer_calls);
  }
  return res_;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
