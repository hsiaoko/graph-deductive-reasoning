#include "gar/reason/chase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#if defined(GAR_REASON_HAS_TBB)
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#endif

namespace gar {
namespace reason {

namespace {

bool EvalCmp(CmpOp op, double lhs, double rhs) {
  switch (op) {
    case CmpOp::kEq: return lhs == rhs;
    case CmpOp::kNe: return lhs != rhs;
    case CmpOp::kLt: return lhs < rhs;
    case CmpOp::kLe: return lhs <= rhs;
    case CmpOp::kGt: return lhs > rhs;
    case CmpOp::kGe: return lhs >= rhs;
  }
  return false;
}

double AttrValueAsDouble(const AttrValue& v, bool* ok) {
  *ok = true;
  if (const auto* i = std::get_if<std::int64_t>(&v)) {
    return static_cast<double>(*i);
  }
  if (const auto* d = std::get_if<double>(&v)) return *d;
  if (const auto* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
  *ok = false;  // string constants can't compare against numeric columns
  return 0.0;
}

bool HasEdge(const gar::miner::TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  const auto [b, e] = g.OutEdgeRange(s);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  auto lo = std::lower_bound(dsts + b, dsts + e, d);
  for (auto it = lo; it != dsts + e && *it == d; ++it) {
    if (g.OutEdgeSlots()[it - dsts] == slot) return true;
  }
  return false;
}

}  // namespace

bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttrs* attrs,
                    const gar::miner::TripleGraph& g) {
  for (const auto& pred : antecedent) {
    switch (pred.kind) {
      case PredKind::kConst: {
        if (attrs == nullptr) return false;
        const AttrColumn* col = attrs->Find(pred.attr);
        if (col == nullptr) return false;
        const auto row = emb[pred.node];
        if (!col->Valid(row)) return false;
        bool ok = false;
        const double rhs = AttrValueAsDouble(pred.value, &ok);
        if (!(ok && EvalCmp(pred.op, col->Value(row), rhs))) return false;
        break;
      }
      case PredKind::kVar: {
        if (attrs == nullptr) return false;
        const AttrColumn* lhs = attrs->Find(pred.attr);
        const AttrColumn* rhs = attrs->Find(pred.rhs_attr);
        if (lhs == nullptr || rhs == nullptr) return false;
        const auto ri = emb[pred.node], rj = emb[pred.rhs_node];
        if (!lhs->Valid(ri) || !rhs->Valid(rj)) return false;
        if (!EvalCmp(pred.op, lhs->Value(ri), rhs->Value(rj))) return false;
        break;
      }
      case PredKind::kExists: {
        if (attrs == nullptr) return false;
        const AttrColumn* col = attrs->Find(pred.attr);
        if (col == nullptr || !col->Valid(emb[pred.node])) return false;
        break;
      }
      case PredKind::kEdge:
        if (!HasEdge(g, emb[pred.src], emb[pred.dst], pred.label)) {
          return false;
        }
        break;
      case PredKind::kMl:
        return false;  // unsupported on CPU
    }
  }
  return true;
}

ChaseResult ChaseEngine::Run(const std::vector<GARule>& rules,
                             const ChaseConfig& cfg) {
  const gar::miner::PatternMatcher matcher(g_);
  ChaseResult result;
  result.rules.resize(rules.size());
  result.matches.resize(rules.size());

  // Pre-validate rules and expand their patterns once.
  struct Prepared {
    gar::miner::PatternSpec spec;
    std::uint32_t pu = 0, pv = 0;  // pivot (unused for counting here)
    std::uint32_t head_slot = 0;
    bool has_head = false;
    bool skip = false;
  };
  std::vector<Prepared> pp(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    Prepared& p = pp[i];
    const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
    if (n < 2 || r.pattern.num_edges() < 1 || !r.pattern.has_edge_labels()) {
      result.rules[i].skipped = true;
      result.rules[i].note = "pattern missing or unlabeled";
      p.skip = true;
      continue;
    }
    p.spec.num_vertices = n;
    for (std::uint32_t s = 0; s < n; ++s) {
      for (EdgeIndex e = r.pattern.offsets[s]; e < r.pattern.offsets[s + 1];
           ++e) {
        p.spec.edges.push_back({s, r.pattern.indices[e],
                                r.pattern.edge_labels[e]});
      }
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
    return EvalAntecedent(r.antecedent, emb, attrs_, g_);
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
    std::vector<std::vector<DerivedEdge>> contribs(rules.size());

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
          contribs[i].push_back(DerivedEdge{emb[pp[i].pu], emb[pp[i].pv],
                                            pp[i].head_slot,
                                            static_cast<std::uint32_t>(i),
                                            round});
        }
      }
    };

#if defined(GAR_REASON_HAS_TBB)
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

    const double enum_secs = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - round_t0)
                                 .count();

    const auto merge_t0 = std::chrono::steady_clock::now();
    const RoundMerge merge =
        MergeRoundFacts(contribs, cfg.max_derivations, g_, &result);
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

RoundMerge MergeRoundFacts(
    const std::vector<std::vector<DerivedEdge>>& contribs,
    std::uint64_t max_derivations, gar::miner::TripleGraph& g,
    ChaseResult* result) {
  // Ordered merge: dedupe (slot, src, dst) across rules; the lowest-index
  // rule producing a fact gets the credit.
  std::unordered_map<std::uint32_t,
                     std::vector<std::pair<std::uint32_t, std::uint32_t>>>
      pending;
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint64_t>>
      pending_sets;
  RoundMerge out;
  for (std::size_t i = 0; i < contribs.size() && !out.deriv_capped; ++i) {
    for (const auto& e : contribs[i]) {
      if (!pending_sets[e.slot]
               .insert(gar::miner::PackPair(e.src, e.dst))
               .second) {
        continue;
      }
      pending[e.slot].emplace_back(e.src, e.dst);
      ++out.new_facts;
      ++result->rules[i].derived;
      result->derived.push_back(e);
      if (result->derived.size() >= max_derivations) {
        out.deriv_capped = true;
        break;
      }
    }
  }
  if (out.new_facts > 0) {
    // Merge the round's facts into the graph (base + inverse mirror), one
    // out-adjacency rebuild for the whole batch.
    g.AddEdgesBatch(pending);
  }
  return out;
}

void MarkAbsentEdges(const gar::miner::TripleGraph& g,
                     const std::uint32_t* src, const std::uint32_t* dst,
                     std::uint32_t slot, std::size_t n, char* keep) {
#if defined(GAR_REASON_HAS_TBB)
  tbb::parallel_for(
      tbb::blocked_range<std::size_t>(0, n),
      [&](const tbb::blocked_range<std::size_t>& r) {
        for (std::size_t t = r.begin(); t != r.end(); ++t) {
          keep[t] = !HasEdge(g, src[t], dst[t], slot) ? 1 : 0;
        }
      });
#else
  for (std::size_t t = 0; t < n; ++t) {
    keep[t] = !HasEdge(g, src[t], dst[t], slot) ? 1 : 0;
  }
#endif
}

}  // namespace reason
}  // namespace gar
