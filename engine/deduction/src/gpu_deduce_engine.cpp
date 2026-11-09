// Host-only translation unit: gar_deduce.cuh declares __host__ APIs and the
// kernels live inside matrixgraph_core, so plain g++ suffices; the only CUDA
// residue is DeviceOwnedBuffer's inline cudaMalloc/cudaFree (hence the
// CUDA::cudart link in CMake). Compiled as .cpp on purpose — nvcc + GCC 12
// cannot compile the AVX512 intrinsics headers reachable from here.

#include "gar/engine/deduction/gpu_deduce_engine.h"

#include <thread>  // must precede MatrixGraph headers (format_converter.cuh)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/data_structures/gar_deduce_arrays.h"
#include "core/data_structures/gar_graph_arrays.h"
#include "core/data_structures/gar_pattern_arrays.h"
#include "core/task/gpu_task/gar_deduce.cuh"
#include "gar/graph/fact.h"
#include "gar/graph/fact_merger.h"

namespace gar {
namespace engine {
namespace deduction {

namespace {

using sics::matrixgraph::core::data_structures::GARAttrColumnSet;
using sics::matrixgraph::core::data_structures::GARDeduceArrays;
using sics::matrixgraph::core::data_structures::GARGraphArrays;
using sics::matrixgraph::core::data_structures::GARPatternArrays;
using sics::matrixgraph::core::data_structures::GARPredicate;

bool HasEdge(const graph::TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  const auto [b, e] = g.OutEdgeRange(s);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  auto lo = std::lower_bound(dsts + b, dsts + e, d);
  for (auto it = lo; it != dsts + e && *it == d; ++it) {
    if (g.OutEdgeSlots()[it - dsts] == slot) return true;
  }
  return false;
}

// CPU-chase AttrValueAsDouble: string constants cannot compare against
// numeric columns.
bool PredValueAsDouble(const gar::AttrValue& v, double* out) {
  if (const auto* i = std::get_if<std::int64_t>(&v)) {
    *out = static_cast<double>(*i);
    return true;
  }
  if (const auto* d = std::get_if<double>(&v)) {
    *out = *d;
    return true;
  }
  if (const auto* b = std::get_if<bool>(&v)) {
    *out = *b ? 1.0 : 0.0;
    return true;
  }
  return false;
}

// Per-rule device-ready form (folded pattern, predicates, attr columns).
struct PreparedRule {
  GARPatternArrays pattern{};
  std::vector<std::int32_t> node_label;
  std::vector<std::int32_t> e_src, e_dst, e_label;
  std::vector<GARPredicate> preds;
  GARAttrColumnSet attr_set{};
  // Stable column storage backing attr_set pointers (indices into the
  // app-wide normalized column arrays).
  bool has_head = false;
  std::uint32_t head_slot = 0;
  std::uint32_t pu = 0, pv = 0;  // head positions (pivot override applied)
  std::uint32_t n_nodes = 0;
  bool x_always_false = false;  // unresolvable attr reference: x never holds
};

}  // namespace

ChaseResult GpuDeduceEngine::Run(const std::vector<GARule>& rules,
                                 const ChaseConfig& cfg) {
  const graph::VertexAttrs empty_attrs;
  const graph::VertexAttrs& attrs = attrs_ != nullptr ? *attrs_ : empty_attrs;
  const std::uint32_t n = g_.num_entities();
  const std::uint32_t num_rel = g_.num_relations();

  // ---- vertex attributes: plain columns normalized to double values +
  // optional valid masks for the device ----
  const std::size_t n_cols = attrs.columns.size();
  std::vector<std::vector<double>> col_vals(n_cols);
  std::vector<std::vector<std::uint8_t>> col_valid(n_cols);
  std::unordered_map<std::string, std::int32_t> col_by_name;
  for (std::size_t c = 0; c < n_cols; ++c) {
    const auto& col = attrs.columns[c];
    col_by_name.emplace(col.name, static_cast<std::int32_t>(c));
    col_vals[c].reserve(n);
    for (std::uint32_t v = 0; v < n; ++v) col_vals[c].push_back(col.Value(v));
    if (!col.valid.empty()) col_valid[c] = col.valid;
  }

  // ---- per-rule preparation (mirrors CpuChaseEngine::Run prep) ----
  ChaseResult result;
  result.rules.resize(rules.size());
  result.matches.resize(rules.size());
  std::vector<PreparedRule> pp(rules.size());
  std::vector<bool> skipped(rules.size(), false);
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const gar::GARule& r = rules[i];
    PreparedRule& q = pp[i];
    auto& st = result.rules[i];
    const auto pn = static_cast<std::uint32_t>(r.pattern.num_vertices());
    auto skip = [&](const char* note) {
      st.skipped = true;
      st.note = note;
      skipped[i] = true;
    };
    if (pn < 2 || r.pattern.num_edges() < 1 ||
        !r.pattern.has_edge_labels()) {
      skip("pattern missing or unlabeled");
      continue;
    }
    if (pn > 16) {
      skip("pattern too large for GPU deduce (>16 nodes)");
      continue;
    }
    for (const auto& pred : r.antecedent) {
      if (pred.kind == gar::PredKind::kMl) {
        skip("ml predicate unsupported on GPU chase");
        break;
      }
      if (pred.kind == gar::PredKind::kEdge &&
          pred.label == gar::kWildcardLabel) {
        skip("unresolved edge predicate label");
        break;
      }
    }
    if (skipped[i]) continue;
    if (r.antecedent.size() > 16) {
      skip("too many X predicates for GPU deduce (>16)");
      continue;
    }

    q.n_nodes = pn;
    // Folded pattern: node labels (wildcard -> -1), edges with inverse
    // slots folded to base relations with swapped endpoints.
    q.node_label.assign(pn, -1);
    for (std::uint32_t u = 0; u < pn; ++u) {
      if (u < r.node_labels.size() &&
          r.node_labels[u] != gar::kWildcardLabel) {
        q.node_label[u] = static_cast<std::int32_t>(r.node_labels[u]);
      }
    }
    bool bad_edge = false;
    for (std::uint32_t s = 0; s < pn && !bad_edge; ++s) {
      for (gar::EdgeIndex e = r.pattern.offsets[s];
           e < r.pattern.offsets[s + 1]; ++e) {
        const gar::LabelID l = r.pattern.edge_labels[e];
        if (l == gar::kWildcardLabel) {
          bad_edge = true;
          break;
        }
        const auto d = static_cast<std::int32_t>(r.pattern.indices[e]);
        if (l >= num_rel) {  // inverse slot: r^-1(s,d) == r(d,s)
          q.e_src.push_back(d);
          q.e_dst.push_back(static_cast<std::int32_t>(s));
          q.e_label.push_back(static_cast<std::int32_t>(l - num_rel));
        } else {
          q.e_src.push_back(static_cast<std::int32_t>(s));
          q.e_dst.push_back(d);
          q.e_label.push_back(static_cast<std::int32_t>(l));
        }
      }
    }
    if (bad_edge) {
      skip("wildcard pattern edge label");
      continue;
    }
    q.pattern = GARPatternArrays{q.node_label.data(),
                                 static_cast<int>(pn),
                                 q.e_src.data(),
                                 q.e_dst.data(),
                                 q.e_label.data(),
                                 static_cast<int>(q.e_src.size())};

    // X predicates; attribute columns are gathered per rule (positions in
    // q.attr_set), with a rule-level x-always-false fallback exactly where
    // the CPU evaluation would return false for every embedding.
    bool x_false = false;
    auto find_col = [&](const std::string& name) -> std::int32_t {
      if (n_cols == 0) return -1;
      const auto it = col_by_name.find(name);
      return it == col_by_name.end() ? -1 : it->second;
    };
    std::unordered_map<std::int32_t, std::int32_t> local_col;
    auto local_col_of = [&](std::int32_t cid) -> std::int32_t {
      const auto it = local_col.find(cid);
      if (it != local_col.end()) return it->second;
      if (q.attr_set.n_cols >= GARAttrColumnSet::kMaxCols) return -1;
      const auto pos = static_cast<std::int32_t>(q.attr_set.n_cols++);
      q.attr_set.col_ids[pos] = cid;
      q.attr_set.values[pos] = col_vals[cid].data();
      q.attr_set.valid[pos] =
          col_valid[cid].empty() ? nullptr : col_valid[cid].data();
      local_col.emplace(cid, pos);
      return pos;
    };
    for (const auto& pred : r.antecedent) {
      GARPredicate dp{};
      switch (pred.kind) {
        case gar::PredKind::kConst: {
          const std::int32_t cid = find_col(pred.attr);
          double rhs = 0.0;
          if (cid < 0 || !PredValueAsDouble(pred.value, &rhs)) {
            x_false = true;
            break;
          }
          const std::int32_t lc = local_col_of(cid);
          if (lc < 0) {
            x_false = true;
            break;
          }
          dp.kind = 0;
          dp.node = static_cast<std::int32_t>(pred.node);
          dp.col = lc;
          dp.op = static_cast<std::int32_t>(pred.op);
          dp.cval = rhs;
          q.preds.push_back(dp);
          break;
        }
        case gar::PredKind::kVar: {
          const std::int32_t ca = find_col(pred.attr);
          const std::int32_t cb = find_col(pred.rhs_attr);
          if (ca < 0 || cb < 0) {
            x_false = true;
            break;
          }
          const std::int32_t la = local_col_of(ca);
          const std::int32_t lb = local_col_of(cb);
          if (la < 0 || lb < 0) {
            x_false = true;
            break;
          }
          dp.kind = 1;
          dp.node = static_cast<std::int32_t>(pred.node);
          dp.col = la;
          dp.op = static_cast<std::int32_t>(pred.op);
          dp.rhs_node = static_cast<std::int32_t>(pred.rhs_node);
          dp.rhs_col = lb;
          q.preds.push_back(dp);
          break;
        }
        case gar::PredKind::kExists: {
          const std::int32_t cid = find_col(pred.attr);
          if (cid < 0) {
            x_false = true;
            break;
          }
          const std::int32_t lc = local_col_of(cid);
          if (lc < 0) {
            x_false = true;
            break;
          }
          dp.kind = 3;
          dp.node = static_cast<std::int32_t>(pred.node);
          dp.col = lc;
          q.preds.push_back(dp);
          break;
        }
        case gar::PredKind::kEdge: {
          dp.kind = 2;
          if (pred.label >= num_rel) {  // fold inverse: swap endpoints
            dp.node = static_cast<std::int32_t>(pred.dst);
            dp.rhs_node = static_cast<std::int32_t>(pred.src);
            dp.edge_label = static_cast<std::int32_t>(pred.label - num_rel);
          } else {
            dp.node = static_cast<std::int32_t>(pred.src);
            dp.rhs_node = static_cast<std::int32_t>(pred.dst);
            dp.edge_label = static_cast<std::int32_t>(pred.label);
          }
          q.preds.push_back(dp);
          break;
        }
        case gar::PredKind::kMl:
          break;  // pre-filtered above
      }
    }
    q.x_always_false = x_false;

    if (r.consequent && r.consequent->kind == gar::PredKind::kEdge &&
        r.consequent->label != gar::kWildcardLabel) {
      q.has_head = true;
      q.head_slot = r.consequent->label;
      q.pu = r.consequent->src;
      q.pv = r.consequent->dst;
    }
    if (r.pivot.size() == 2) {
      q.pu = r.pivot[0];
      q.pv = r.pivot[1];
    }
  }

  // ---- static host arrays for GARGraphArrays (rebuilt per round) ----
  std::vector<std::uint32_t> v_id(n);
  std::iota(v_id.begin(), v_id.end(), 0);
  std::vector<std::int32_t> v_label(n, 0);
  if (g_.has_vertex_labels()) {
    for (std::uint32_t v = 0; v < n; ++v) {
      v_label[v] = static_cast<std::int32_t>(g_.VertexLabel(v));
    }
  }

  // The device pre-allocates the whole embedding frontier (16M rows);
  // eval_cap mirrors the CPU chase's per-rule match cap (counting /
  // X-evaluation / head emission run over the first eval_cap embeddings).
  constexpr std::uint64_t kGpuFrontierCap = 16'777'216;
  const int eval_cap = static_cast<int>(std::min<std::uint64_t>(
      std::min(cfg.max_matches_per_rule, kGpuFrontierCap),
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
  constexpr int frontier_cap = static_cast<int>(kGpuFrontierCap);
  if (cfg.max_matches_per_rule > kGpuFrontierCap) {
    std::cerr << "[deduce_gpu] note: --max-matches clamped to "
              << kGpuFrontierCap << " (device frontier bound)\n";
  }
  std::vector<std::uint32_t> head_src(static_cast<std::size_t>(eval_cap));
  std::vector<std::uint32_t> head_dst(static_cast<std::size_t>(eval_cap));
  constexpr std::uint32_t kMaxNodes = 16;
  std::vector<std::int32_t> emb_dump;
  std::vector<std::uint8_t> xok_dump;
  if (cfg.keep_matches) {
    emb_dump.resize(static_cast<std::size_t>(eval_cap) * kMaxNodes);
    xok_dump.resize(static_cast<std::size_t>(eval_cap));
  }

  sics::matrixgraph::core::task::GARDeduce task;
  bool capped_out = false;
  for (std::uint32_t round = 1; round <= cfg.max_rounds; ++round) {
    const auto round_t0 = std::chrono::steady_clock::now();

    // Base-relation edge arrays of the current graph, sorted by (src,
    // dst) with duplicate (dst, label) pairs removed — the CPU matcher
    // enumerates over the deduplicated rel-pair arrays, so the device
    // candidates must be duplicate-free too (out-adjacency is per-vertex
    // dst-sorted; labels within a dst-run are sorted and uniqued here).
    std::vector<std::uint32_t> e_src, e_dst;
    std::vector<std::int32_t> e_label;
    {
      e_src.reserve(g_.num_edges());
      e_dst.reserve(g_.num_edges());
      e_label.reserve(g_.num_edges());
      std::vector<std::uint32_t> run_labels;
      for (std::uint32_t v = 0; v < n; ++v) {
        const auto [b, e] = g_.OutEdgeRange(v);
        std::size_t k = b;
        while (k < e) {
          const std::uint32_t d = g_.OutEdgeDsts()[k];
          run_labels.clear();
          std::size_t k2 = k;
          for (; k2 < e && g_.OutEdgeDsts()[k2] == d; ++k2) {
            const std::uint32_t slot = g_.OutEdgeSlots()[k2];
            if (slot < num_rel) run_labels.push_back(slot);
          }
          std::sort(run_labels.begin(), run_labels.end());
          run_labels.erase(
              std::unique(run_labels.begin(), run_labels.end()),
              run_labels.end());
          for (const std::uint32_t slot : run_labels) {
            e_src.push_back(v);
            e_dst.push_back(d);
            e_label.push_back(static_cast<std::int32_t>(slot));
          }
          k = k2;
        }
      }
    }
    GARGraphArrays gg{v_id.data(),
                      v_label.data(),
                      static_cast<int>(n),
                      e_src.data(),
                      e_dst.data(),
                      nullptr,
                      e_label.data(),
                      static_cast<int>(e_src.size())};
    task.SetGraph(gg);

    std::vector<std::vector<graph::DerivedEdge>> contribs(
        rules.size());
    for (std::size_t i = 0; i < rules.size(); ++i) {
      if (skipped[i]) continue;
      PreparedRule& q = pp[i];
      auto& st = result.rules[i];

      std::uint64_t n_emb = 0, n_x = 0;
      int match_capped = 0, head_size = 0, emb_rows = 0;
      GARDeduceArrays out{};
      out.num_embeddings = &n_emb;
      out.num_x_satisfied = &n_x;
      out.match_capped = &match_capped;
      // x-always-false rules: topology is still counted, but X never
      // holds and nothing is derived (CPU semantics), so head emission
      // and predicate evaluation are suppressed.
      const bool emit_head = q.has_head && !q.x_always_false;
      if (emit_head) {
        out.head_src = head_src.data();
        out.head_dst = head_dst.data();
        out.head_capacity = eval_cap;
        out.head_size = &head_size;
      }
      const bool dump = cfg.keep_matches;
      if (dump) {
        out.embeddings = emb_dump.data();
        out.emb_x_ok = xok_dump.data();
        out.emb_capacity = eval_cap;
        out.emb_rows = &emb_rows;
      }
      const int rc = task.Run(q.pattern, q.preds.data(),
                              static_cast<int>(q.preds.size()), q.attr_set,
                              emit_head ? static_cast<int>(q.pu) : -1,
                              emit_head ? static_cast<int>(q.pv) : -1,
                              frontier_cap, eval_cap, &out);
      if (rc == 3) {
        st.skipped = true;
        st.note = "disconnected pattern";
        skipped[i] = true;
        continue;
      }
      if (rc != 0) {
        std::cerr << "error: GARDeduce failed on rule " << i
                  << " with status " << rc << "\n";
        throw std::runtime_error("GARDeduce failed");
      }
      st.embeddings = n_emb;
      st.x_satisfied = q.x_always_false ? 0 : n_x;
      if (match_capped) {
        st.match_capped = true;
        capped_out = true;
      }
      if (emit_head) {
        const int n_heads = std::min(head_size, eval_cap);
        // Same pre-filter as the CPU chase: only heads not already in
        // the (pre-merge) graph are candidates; within-round dedup and
        // credit happen in MergeRoundFacts. The filter runs in parallel
        // inside the graph library (capped rules emit 1M heads/round).
        std::vector<char> keep(static_cast<std::size_t>(n_heads));
        graph::MarkAbsentEdges(g_, head_src.data(), head_dst.data(),
                               q.head_slot,
                               static_cast<std::size_t>(n_heads),
                               keep.data());
        contribs[i].reserve(static_cast<std::size_t>(n_heads));
        for (int t = 0; t < n_heads; ++t) {
          if (keep[static_cast<std::size_t>(t)]) {
            contribs[i].push_back(graph::DerivedEdge{
                head_src[t], head_dst[t], q.head_slot,
                static_cast<std::uint32_t>(i), round});
          }
        }
      }
      if (dump) {
        auto& rm = result.matches[i];
        rm.n = q.n_nodes;
        rm.capped = match_capped != 0;
        rm.embs.assign(emb_dump.begin(),
                       emb_dump.begin() + static_cast<std::size_t>(emb_rows) *
                                              q.n_nodes);
        rm.x_ok.assign(xok_dump.begin(), xok_dump.begin() + emb_rows);
        if (q.x_always_false) {
          std::fill(rm.x_ok.begin(), rm.x_ok.end(), 0);
        }
        rm.head_hit.resize(static_cast<std::size_t>(emb_rows));
        for (int t = 0; t < emb_rows; ++t) {
          const auto* row =
              rm.embs.data() + static_cast<std::size_t>(t) * q.n_nodes;
          rm.head_hit[t] =
              q.has_head && HasEdge(g_, row[q.pu], row[q.pv], q.head_slot)
                  ? 1
                  : 0;
        }
      }
    }
    for (std::size_t i = 0; i < rules.size(); ++i) {
      if (skipped[i]) continue;
      result.embeddings_total += result.rules[i].embeddings;
      result.x_satisfied_total += result.rules[i].x_satisfied;
      result.derivation_candidates_total += contribs[i].size();
    }
    const double enum_secs = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - round_t0)
                                 .count();

    const auto merge_t0 = std::chrono::steady_clock::now();
    std::vector<std::uint64_t> rule_derived(rules.size(), 0);
    const graph::RoundMerge merge = graph::MergeRoundFacts(
        contribs, cfg.max_derivations, g_, &result.derived,
        &rule_derived);
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
                   "[chase-gpu] round %u: enumerate %.1fs, +0 facts "
                   "(fixpoint)\n",
                   round, enum_secs);
      result.fixpoint = !capped_out;
      break;
    }
    std::fprintf(stderr,
                 "[chase-gpu] round %u: enumerate %.1fs, merge %.1fs, "
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
