#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

#include "gar/core/agent/executor.h"
#include "gar/core/graph/predicate_evaluator.h"
#include "rule_support.h"

namespace gar::core::engine::agent {
namespace {

class BackwardSession {
 public:
  BackwardSession(const Goal& goal, AgentState* state, WitnessLog* witness,
                  const AgentContext& ctx, const ExecConfig& cfg,
                  const ExecKit& kit)
      : goal_(goal), state_(state), witness_(witness), ctx_(ctx), cfg_(cfg),
        kit_(kit), g_(state->graph()),
        sctx_{&ctx.distances, ctx.table, witness, state} {
    res_.stats.emplace();
  }

  AgentResult Run() {
    if (!goal_.args[0] || !goal_.args[1]) {
      throw std::invalid_argument(
          "BackwardExecutor currently requires a fully bound goal");
    }
    const auto started = std::chrono::steady_clock::now();
    t0_ = started;
    BuildRuleIndex();
    const bool hit = Prove(goal_.slot, *goal_.args[0], *goal_.args[1], 0);
    res_.goal_derived = hit;
    res_.derived = state_->journal();
    res_.logical_cost = res_.stats->search_cost;
    res_.execution_cost_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (hit) {
      res_.termination = "goal";
    } else if (stopped_) {
      res_.termination = stop_reason_;
    } else {
      res_.termination = "no_proof_found";
    }
    return std::move(res_);
  }

 private:
  struct ActiveGuard {
    std::set<std::array<std::uint32_t, 3>>& active;
    std::array<std::uint32_t, 3> key;
    ~ActiveGuard() { active.erase(key); }
  };

  void Debug(const std::string& text) const {
    if (std::getenv("GAR_DEBUG_BACKWARD") != nullptr) {
      std::fprintf(stderr, "[backward] %s\n", text.c_str());
    }
  }

  bool Stopped() {
    if (calls_ >= cfg_.backward_max_calls) {
      stopped_ = true;
      stop_reason_ = "backward_call_limit";
      return true;
    }
    if (cfg_.wall_limit_sec > 0.0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_)
                .count() >= cfg_.wall_limit_sec) {
      stopped_ = true;
      stop_reason_ = "timeout";
      return true;
    }
    return false;
  }

  void BuildRuleIndex() {
    const auto& rules = state_->rules();
    head_index_.resize(g_.num_relations());
    for (std::uint32_t i = 0; i < rules.size(); ++i) {
      RuleHead head;
      if (!RuleSupported(rules[i], &head)) continue;
      head_index_[g_.BaseRelation(head.slot)].push_back(i);
    }
    for (auto& indices : head_index_) {
      std::stable_sort(indices.begin(), indices.end(), [&](std::uint32_t a,
                                                            std::uint32_t b) {
        ScoredRule sa, sb;
        kit_.scorer->ScoreRule(rules[a], sctx_, &sa);
        kit_.scorer->ScoreRule(rules[b], sctx_, &sb);
        if (sa.priority != sb.priority) return sa.priority > sb.priority;
        return a < b;
      });
    }
  }

  const std::vector<std::uint32_t>* RuleOrder(
      std::uint32_t slot, std::optional<std::uint32_t> src,
      std::optional<std::uint32_t> dst,
      std::vector<std::uint32_t>* learned_order) {
    if (kit_.relevance_model == nullptr) return &head_index_[slot];
    const auto& rules = state_->rules();
    std::vector<std::pair<double, std::uint32_t>> priorities;
    priorities.reserve(head_index_[slot].size());
    for (const std::uint32_t rule_index : head_index_[slot]) {
      const GARule& rule = rules[rule_index];
      const RuleHead head = ResolveRuleHead(rule);
      std::optional<std::uint32_t> head_src = src;
      std::optional<std::uint32_t> head_dst = dst;
      if (g_.IsInverse(head.slot)) std::swap(head_src, head_dst);
      ScoredRule score;
      kit_.scorer->ScoreRule(rule, sctx_, &score);
      ScoredMotif candidate;
      candidate.motif.rule = rule_index;
      candidate.motif.embedding.assign(rule.num_vertices(), UINT32_MAX);
      if (head_src) candidate.motif.embedding[head.pu] = *head_src;
      if (head_dst) candidate.motif.embedding[head.pv] = *head_dst;
      candidate.rel_fam = score.rel_fam;
      candidate.price = score.price;
      candidate.total = score.priority;
      const double priority =
          kit_.policy->SelectionPriorityForCandidate(rule_index, candidate);
      kit_.policy->ReleaseSelectionPriority(rule_index);
      priorities.emplace_back(priority, rule_index);
    }
    std::sort(priorities.begin(), priorities.end(), [](const auto& a,
                                                        const auto& b) {
      if (a.first != b.first) return a.first > b.first;
      return a.second < b.second;
    });
    learned_order->clear();
    learned_order->reserve(priorities.size());
    for (const auto& [priority, rule_index] : priorities) {
      (void)priority;
      learned_order->push_back(rule_index);
    }
    return learned_order;
  }

  bool Prove(std::uint32_t slot, std::uint32_t src, std::uint32_t dst,
             std::uint32_t depth) {
    if (g_.IsInverse(slot)) {
      slot = g_.BaseRelation(slot);
      std::swap(src, dst);
    }
    if (state_->ContainsEdge(slot, src, dst)) return true;
    if (Stopped() || depth > cfg_.backward_max_depth) return false;
    ++calls_;
    ++res_.stats->select_events;
    const std::array<std::uint32_t, 3> key{slot, src, dst};
    if (!active_.insert(key).second) return false;
    ActiveGuard guard{active_, key};
    Debug("prove slot=" + std::to_string(slot) + " src=" +
          std::to_string(src) + " dst=" + std::to_string(dst) +
          " depth=" + std::to_string(depth));
    if (std::getenv("GAR_DEBUG_BACKWARD") != nullptr) {
      for (const std::uint64_t pair : g_.RelationPairs(slot)) {
        std::fprintf(stderr, "[backward] slot %u has %u -> %u\n", slot,
                     static_cast<std::uint32_t>(pair >> 32),
                     static_cast<std::uint32_t>(pair));
      }
    }
    std::vector<std::uint32_t> learned_order;
    const std::vector<std::uint32_t>* rule_order =
        RuleOrder(slot, src, dst, &learned_order);
    for (const std::uint32_t rule_index : *rule_order) {
      if (Stopped()) return false;
      Motif motif;
      gar::core::graph::DerivedEdge derived;
      if (TryRule(rule_index, slot, src, dst, depth, &motif, &derived)) {
        state_->ApplyDerivation(derived);
        std::vector<Fact> premises;
        GroundPremises(state_->rules()[rule_index], motif, g_, *ctx_.table,
                       &premises);
        for (const Fact& fact : premises) {
          witness_->WitnessFact(fact, ctx_.encoder);
        }
        witness_->WitnessFact(MakeEdgeFact(derived.slot, derived.src, derived.dst),
                              ctx_.encoder);
        AgentStep step;
        step.motif = std::move(motif);
        step.success = true;
        step.cost = kit_.cost_model->RuleLowerBound(state_->rules()[rule_index]);
        step.derived = derived;
        res_.chain.push_back(std::move(step));
        res_.stats->proof_cost += step.cost;
        res_.stats->search_cost += step.cost;
        ++res_.stats->derive_events;
        Debug("proved with rule=" + std::to_string(rule_index));
        return state_->ContainsEdge(slot, src, dst);
      }
      Debug("rule failed=" + std::to_string(rule_index));
    }
    return false;
  }

  bool ProveOpen(std::uint32_t slot, std::optional<std::uint32_t> src,
                 std::optional<std::uint32_t> dst, std::uint32_t depth) {
    if (g_.IsInverse(slot)) {
      slot = g_.BaseRelation(slot);
      std::swap(src, dst);
    }
    if (Stopped() || depth > cfg_.backward_max_depth) return false;
    ++calls_;
    ++res_.stats->select_events;
    const std::array<std::uint32_t, 3> key{
        slot, src.value_or(UINT32_MAX), dst.value_or(UINT32_MAX)};
    if (!active_.insert(key).second) return false;
    ActiveGuard guard{active_, key};
    Debug("prove-open slot=" + std::to_string(slot));
    std::vector<std::uint32_t> learned_order;
    const std::vector<std::uint32_t>* rule_order =
        RuleOrder(slot, src, dst, &learned_order);
    for (const std::uint32_t rule_index : *rule_order) {
      if (Stopped()) return false;
      Motif motif;
      gar::core::graph::DerivedEdge derived;
      if (!TryRule(rule_index, slot, src, dst, depth, &motif, &derived,
                   /*require_new_head=*/true)) {
        continue;
      }
      Debug("open candidate slot=" + std::to_string(derived.slot) +
            " src=" + std::to_string(derived.src) + " dst=" +
            std::to_string(derived.dst));
      if (state_->ContainsEdge(derived.slot, derived.src, derived.dst)) {
        continue;
      }
      state_->ApplyDerivation(derived);
      std::vector<Fact> premises;
      GroundPremises(state_->rules()[rule_index], motif, g_, *ctx_.table,
                     &premises);
      for (const Fact& fact : premises) witness_->WitnessFact(fact, ctx_.encoder);
      witness_->WitnessFact(MakeEdgeFact(derived.slot, derived.src, derived.dst),
                            ctx_.encoder);
      AgentStep step;
      step.motif = std::move(motif);
      step.success = true;
      step.cost = kit_.cost_model->RuleLowerBound(state_->rules()[rule_index]);
      step.derived = derived;
      res_.chain.push_back(std::move(step));
      res_.stats->proof_cost += step.cost;
      res_.stats->search_cost += step.cost;
      ++res_.stats->derive_events;
      return true;
    }
    return false;
  }

  bool TryRule(std::uint32_t rule_index, std::uint32_t goal_slot,
               std::optional<std::uint32_t> src,
               std::optional<std::uint32_t> dst, std::uint32_t depth,
               Motif* motif, gar::core::graph::DerivedEdge* derived,
               bool require_new_head = false) {
    const GARule& rule = state_->rules()[rule_index];
    const RuleHead head = ResolveRuleHead(rule);
    const bool inverse = g_.IsInverse(head.slot);
    if (g_.BaseRelation(head.slot) != goal_slot) return false;
    if (inverse) std::swap(src, dst);
    Motif candidate;
    candidate.rule = rule_index;
    candidate.embedding.assign(rule.num_vertices(), UINT32_MAX);
    if (src) candidate.embedding[head.pu] = *src;
    if (dst) candidate.embedding[head.pv] = *dst;
    std::vector<char> done(rule.num_edges(), 0);
    if (!SolveEdges(rule, 0, depth, &candidate, &done, require_new_head)) {
      return false;
    }
    Debug("rule solved=" + std::to_string(rule_index));
    motif->rule = rule_index;
    motif->embedding = std::move(candidate.embedding);
    derived->src = motif->embedding[head.pu];
    derived->dst = motif->embedding[head.pv];
    derived->slot = head.slot;
    derived->rule = rule_index;
    derived->round = static_cast<std::uint32_t>(res_.chain.size() + 1);
    return true;
  }

  std::uint32_t ChooseEdge(const GARule& rule, const Motif& motif,
                           const std::vector<char>& done) const {
    std::uint32_t best = 0;
    int best_bound = -1;
    std::size_t best_size = 0;
    for (std::uint32_t e = 0; e < rule.num_edges(); ++e) {
      if (done[e]) continue;
      const int bound = (motif.embedding[rule.edge_src[e]] != UINT32_MAX) +
                        (motif.embedding[rule.edge_dst[e]] != UINT32_MAX);
      const std::size_t size = g_.RelationPairs(rule.edge_labels[e]).size();
      if (bound > best_bound ||
          (bound == best_bound && size < best_size)) {
        best = e;
        best_bound = bound;
        best_size = size;
      }
    }
    return best;
  }

  bool SolveEdges(const GARule& rule, std::uint32_t done_count,
                  std::uint32_t depth, Motif* motif, std::vector<char>* done,
                  bool require_new_head = false) {
    if (Stopped()) return false;
    if (done_count == rule.num_edges()) {
      if (!VertexLabelsOk(rule, g_, motif->embedding.data()) ||
          !gar::core::graph::EvalAntecedent(
              rule.antecedent, motif->embedding.data(), state_->attrs(), g_,
              state_->ml_models())) {
        return false;
      }
      if (require_new_head) {
        const RuleHead head = ResolveRuleHead(rule);
        return !state_->ContainsEdge(head.slot, motif->embedding[head.pu],
                                     motif->embedding[head.pv]);
      }
      return true;
    }
    const std::uint32_t edge = ChooseEdge(rule, *motif, *done);
    const std::uint32_t src_vertex = rule.edge_src[edge];
    const std::uint32_t dst_vertex = rule.edge_dst[edge];
    const std::uint32_t slot = rule.edge_labels[edge];
    const std::uint32_t src = motif->embedding[src_vertex];
    const std::uint32_t dst = motif->embedding[dst_vertex];
    const bool src_bound = src != UINT32_MAX;
    const bool dst_bound = dst != UINT32_MAX;

    std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
    if (src_bound && dst_bound) {
      candidates.emplace_back(src, dst);
    } else if (src_bound) {
      const auto [values, count] = g_.OutNeighbors(src, slot);
      for (std::size_t i = 0; i < count; ++i) {
        candidates.emplace_back(src, values[i]);
      }
    } else if (dst_bound) {
      const std::uint32_t flipped = g_.IsInverse(slot)
                                        ? slot - g_.num_relations()
                                        : slot + g_.num_relations();
      const auto [values, count] = g_.OutNeighbors(dst, flipped);
      for (std::size_t i = 0; i < count; ++i) {
        candidates.emplace_back(values[i], dst);
      }
    } else {
      const auto& pairs = g_.RelationPairs(slot);
      candidates.reserve(pairs.size());
      for (const std::uint64_t pair : pairs) {
        candidates.emplace_back(static_cast<std::uint32_t>(pair >> 32),
                                static_cast<std::uint32_t>(pair));
      }
    }
    Debug("edge slot=" + std::to_string(slot) + " candidates=" +
          std::to_string(candidates.size()));

    std::size_t attempted = 0;
    for (const auto [candidate_src, candidate_dst] : candidates) {
      if (Stopped()) return false;
      if (cfg_.match_cap_per_rule != 0 &&
          attempted >= cfg_.match_cap_per_rule) {
        ++res_.stats->candidate_caps;
        break;
      }
      ++attempted;
      ++res_.stats->evaluate_events;
      motif->embedding[src_vertex] = candidate_src;
      motif->embedding[dst_vertex] = candidate_dst;
      if (!VertexLabelsOk(rule, g_, motif->embedding.data())) continue;
      if (!state_->ContainsEdge(slot, candidate_src, candidate_dst) &&
          !Prove(slot, candidate_src, candidate_dst, depth + 1)) {
        continue;
      }
      (*done)[edge] = 1;
      if (SolveEdges(rule, done_count + 1, depth, motif, done,
                     require_new_head)) {
        return true;
      }
      (*done)[edge] = 0;
      if (!src_bound) motif->embedding[src_vertex] = UINT32_MAX;
      if (!dst_bound) motif->embedding[dst_vertex] = UINT32_MAX;
    }
    if (src_bound != dst_bound &&
        ProveOpen(slot, src_bound ? std::optional<std::uint32_t>(src)
                                  : std::nullopt,
                  dst_bound ? std::optional<std::uint32_t>(dst)
                            : std::nullopt,
                  depth + 1)) {
      return SolveEdges(rule, done_count, depth, motif, done,
                        require_new_head);
    }
    return false;
  }

  const Goal& goal_;
  AgentState* state_;
  WitnessLog* witness_;
  const AgentContext& ctx_;
  const ExecConfig& cfg_;
  const ExecKit& kit_;
  const gar::core::graph::Graph& g_;
  const ScoringContext sctx_;
  AgentResult res_;
  std::vector<std::vector<std::uint32_t>> head_index_;
  std::set<std::array<std::uint32_t, 3>> active_;
  std::chrono::steady_clock::time_point t0_;
  std::uint64_t calls_ = 0;
  bool stopped_ = false;
  std::string stop_reason_;
};

}  // namespace

AgentResult BackwardExecutor::Run(const Goal& goal, AgentState* state,
                                  WitnessLog* witness,
                                  const AgentContext& ctx,
                                  const ExecConfig& cfg, const ExecKit& kit) {
  return BackwardSession(goal, state, witness, ctx, cfg, kit).Run();
}

}  // namespace gar::core::engine::agent
