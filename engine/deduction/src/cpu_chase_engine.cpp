#include "gar/engine/deduction/cpu_chase_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

#include "gar/graph/fact_merger.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"

#if defined(GAR_ENGINE_DEDUCTION_HAS_TBB)
#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#endif

namespace gar {
namespace engine {
namespace deduction {

namespace {

bool HasEdge(const graph::TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  // O(1) hash point query; always reflects the latest edges.
  return g.RelationPairSet(slot).count(graph::PackPair(s, d)) != 0;
}

}  // namespace

ChaseResult CpuChaseEngine::Run(const std::vector<GARule>& rules,
                                const ChaseConfig& cfg) {
#if defined(GAR_ENGINE_DEDUCTION_HAS_TBB)
  // ChaseConfig::threads caps TBB parallelism for this Run (0 = all cores).
  std::unique_ptr<tbb::global_control> tbb_limit;
  if (cfg.threads > 0) {
    tbb_limit = std::make_unique<tbb::global_control>(
        tbb::global_control::max_allowed_parallelism, cfg.threads);
  }
#endif
  const graph::PatternMatcher matcher(g_);
  ChaseResult result;
  result.rules.resize(rules.size());
  result.matches.resize(rules.size());

  // Pre-validate rules and expand their patterns once.
  struct Prepared {
    graph::PatternSpec spec;
    std::uint32_t pu = 0, pv = 0;  // pivot (unused for counting here)
    std::uint32_t head_slot = 0;
    bool has_head = false;
    bool skip = false;
  };
  std::vector<Prepared> pp(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    Prepared& p = pp[i];
    const auto n = static_cast<std::uint32_t>(r.num_vertices());
    if (n < 2 || r.num_edges() < 1 || !r.has_edge_labels()) {
      result.rules[i].skipped = true;
      result.rules[i].note = "pattern missing or unlabeled";
      p.skip = true;
      continue;
    }
    p.spec.num_vertices = n;
    for (std::size_t e = 0; e < r.edge_src.size(); ++e) {
      p.spec.edges.push_back(
          {static_cast<std::uint32_t>(r.edge_src[e]),
           static_cast<std::uint32_t>(r.edge_dst[e]),
           static_cast<LabelID>(r.edge_labels[e])});
    }
    for (const auto& pred : r.antecedent) {
      if (pred.kind == PredKind::kMl) {
        result.rules[i].skipped = true;
        result.rules[i].note = "ml predicate unsupported on CPU chase";
        p.skip = true;
      }
      if (pred.kind == PredKind::kEdge && pred.label == kWildcardLabel) {
        result.rules[i].skipped = true;
        result.rules[i].note = "unresolved edge predicate label";
        p.skip = true;
      }
    }
    if (p.skip) continue;
    if (r.consequent && r.consequent->kind == PredKind::kEdge &&
        r.consequent->label != kWildcardLabel) {
      p.has_head = true;
      p.head_slot = r.consequent->label;
      p.pu = r.consequent->src;
      p.pv = r.consequent->dst;
    }
    if (r.pivot.size() == 2) {
      p.pu = r.pivot[0];
      p.pv = r.pivot[1];
    }
  }

  // X evaluation against one embedding (shared with validate_rules).
  auto eval_x = [&](const GARule& r, const std::uint32_t* emb) {
    return graph::EvalAntecedent(r.antecedent, emb, attrs_, g_);
  };
  const bool typed = g_.has_vertex_labels();
  auto label_ok = [&](const GARule& r, std::uint32_t node,
                      std::uint32_t vid) {
    return !typed || node >= r.node_labels.size() ||
           r.node_labels[node] == kWildcardLabel ||
           g_.VertexLabel(vid) == r.node_labels[node];
  };

  // Naive chase rounds. Rules within a round are independent (matching is
  // read-only on the graph) and are evaluated in parallel (oneTBB); per-rule
  // derived-fact candidates are merged in rule order afterwards, so dedup
  // credit is deterministic regardless of thread scheduling.
  std::atomic<bool> capped_out{false};
  for (std::uint32_t round = 1; round <= cfg.max_rounds; ++round) {
    const auto round_t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<graph::DerivedEdge>> contribs(rules.size());

    auto process_rule = [&](std::size_t i) {
      if (pp[i].skip) return;
      const GARule& r = rules[i];
      const auto n = pp[i].spec.num_vertices;
      auto& st = result.rules[i];

      std::vector<std::uint32_t> embs;
      bool capped = false;
      const std::uint64_t n_emb =
          matcher.Enumerate(pp[i].spec, cfg.max_matches_per_rule, &embs,
                            &capped);
      if (capped) {
        st.match_capped = true;
        capped_out = true;
      }

      RuleMatches* rm = nullptr;
      if (cfg.keep_matches) {
        rm = &result.matches[i];
        rm->n = n;
        rm->embs.clear();
        rm->x_ok.clear();
        rm->head_hit.clear();
        rm->capped = capped;
      }

      st.embeddings = 0;
      st.x_satisfied = 0;
      for (std::uint64_t t = 0; t < n_emb; ++t) {
        const std::uint32_t* emb = embs.data() + t * n;
        bool ok = true;
        for (std::uint32_t u = 0; u < n; ++u) {
          if (!label_ok(r, u, emb[u])) {
            ok = false;
            break;
          }
        }
        if (!ok) continue;
        ++st.embeddings;
        const bool x_ok = eval_x(r, emb);
        if (x_ok) ++st.x_satisfied;

        const bool head_hit =
            pp[i].has_head && HasEdge(g_, emb[pp[i].pu], emb[pp[i].pv],
                                      pp[i].head_slot);
        if (rm != nullptr) {
          rm->embs.insert(rm->embs.end(), emb, emb + n);
          rm->x_ok.push_back(x_ok);
          rm->head_hit.push_back(head_hit);
        }

        if (x_ok && pp[i].has_head && !head_hit) {
          contribs[i].push_back(graph::DerivedEdge{emb[pp[i].pu], emb[pp[i].pv],
                                                   pp[i].head_slot,
                                                   static_cast<std::uint32_t>(i),
                                                   round});
        }
      }
    };

#if defined(GAR_ENGINE_DEDUCTION_HAS_TBB)
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, rules.size()),
        [&](const tbb::blocked_range<std::size_t>& range) {
          for (std::size_t i = range.begin(); i != range.end(); ++i) {
            process_rule(i);
          }
        });
#else
    for (std::size_t i = 0; i < rules.size(); ++i) process_rule(i);
#endif
    for (std::size_t i = 0; i < rules.size(); ++i) {
      if (pp[i].skip) continue;
      result.embeddings_total += result.rules[i].embeddings;
      result.x_satisfied_total += result.rules[i].x_satisfied;
      result.derivation_candidates_total += contribs[i].size();
    }

    const double enum_secs = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - round_t0)
                                 .count();

    const auto merge_t0 = std::chrono::steady_clock::now();
    std::vector<std::uint64_t> rule_derived(rules.size(), 0);
    const graph::RoundMerge merge =
        graph::MergeRoundFacts(contribs, cfg.max_derivations, g_,
                               &result.derived, &rule_derived);
    for (std::size_t i = 0; i < rules.size(); ++i) {
      result.rules[i].derived += rule_derived[i];
    }
    const double merge_secs = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - merge_t0)
                                  .count();
    if (merge.deriv_capped) capped_out = true;
    result.rounds = round;
    if (merge.new_facts == 0) {
      std::fprintf(stderr,
                   "[chase] round %u: enumerate %.1fs, +0 facts (fixpoint)\n",
                   round, enum_secs);
      result.fixpoint = !capped_out;
      break;
    }
    std::fprintf(stderr,
                 "[chase] round %u: enumerate %.1fs, merge %.1fs, "
                 "+%llu facts\n",
                 round, enum_secs, merge_secs,
                 static_cast<unsigned long long>(merge.new_facts));
    if (merge.deriv_capped) {
      result.fixpoint = false;
      break;
    }
  }
  return result;
}

}  // namespace deduction
}  // namespace engine
}  // namespace gar
