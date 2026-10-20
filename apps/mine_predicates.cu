// mine_predicates: stage-3 GAR mining — enrich mined rules (chain or
// generic-topology bodies) with attribute predicates (X) over real vertex
// attributes.
//
// For every rule directory produced by mine_rules / mine_graph_rules, the
// body matches are re-enumerated on the graph (CPU, PatternMatcher with the
// same homomorphism semantics as the miners), the vertex attribute columns
// are joined onto the matches, and const/var predicate candidates are
// evaluated with pivot-pair support/confidence:
//
//   const:  x.A = c      (attribute vs. constant)
//   var:    x.A = y.A    (same attribute on two pattern variables)
//
// A greedy pass adds predicates to X while confidence strictly improves and
// pivot support stays >= --min-supp. Enriched rules (X filled, quality
// recomputed under X) are written to --out in the one-rule-per-directory
// layout. Rules without qualifying predicates are skipped.
//
// Usage:
//   mine_predicates --csr <immutable_csr_dir> --rules <rules_root>
//                   --out <out_root> [--min-supp 500] [--min-conf 0.3]
//                   [--max-matches 2000000] [--max-preds 2] [--print 20]
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/io/attributes_io.cuh"
#include "gar/miner/pattern_matcher.h"
#include "gar/miner/triple_graph.h"
#include "gar/rule_loader.h"
#include "gar/rule_store.h"

namespace {

namespace mg_attr = sics::matrixgraph::core::data_structures;
using sics::matrixgraph::core::io::AttributesIO;
using gar::miner::PackPair;

struct Args {
  std::string csr_dir;
  std::string rules_root;
  std::string out_root;
  std::uint64_t min_supp = 500;
  double min_conf = 0.3;
  std::uint64_t max_matches = 2'000'000;
  std::uint32_t max_preds = 2;
  std::size_t print_n = 20;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << k << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--csr") a.csr_dir = next();
    else if (k == "--rules") a.rules_root = next();
    else if (k == "--out") a.out_root = next();
    else if (k == "--min-supp") a.min_supp = std::stoull(next());
    else if (k == "--min-conf") a.min_conf = std::stod(next());
    else if (k == "--max-matches") a.max_matches = std::stoull(next());
    else if (k == "--max-preds")
      a.max_preds = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--print") a.print_n = std::stoull(next());
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.csr_dir.empty() || a.rules_root.empty() || a.out_root.empty()) {
    std::cerr << "usage: mine_predicates --csr <csr_dir> --rules <rules_root> "
                 "--out <out_root> [--min-supp N] [--min-conf F] "
                 "[--max-matches N] [--max-preds K] [--print N]\n";
    std::exit(2);
  }
  return a;
}

// One body match: one data vertex per pattern vertex + head flag.
struct Match {
  std::vector<std::uint32_t> v;
  bool head;
};

// Attribute column view uniform over Int / Float64.
struct Column {
  std::string name;
  bool is_int = true;
  const mg_attr::Attribute* attr = nullptr;

  bool Valid(std::uint32_t row) const {
    return row < attr->n_rows && (attr->valid == nullptr || attr->valid[row]);
  }
  double Value(std::uint32_t row) const {
    return is_int ? static_cast<double>(mg_attr::GetInt(*attr, row))
                  : mg_attr::GetFloat64(*attr, row);
  }
  gar::AttrValue AttrVal(std::uint32_t row) const {
    if (is_int) return gar::AttrValue(mg_attr::GetInt(*attr, row));
    return gar::AttrValue(mg_attr::GetFloat64(*attr, row));
  }
};

struct Candidate {
  gar::Predicate pred;
  std::uint64_t supp = 0;   // pivot pairs with body ∧ X ∧ head
  double conf = 0.0;        // supp / pivot pairs with body ∧ X
  std::uint64_t body = 0;   // pivot pairs with body ∧ X
};

struct RuleContext {
  const gar::miner::TripleGraph* g;
  const std::vector<Column>* cols;
  std::uint32_t n = 0;            // pattern vertices
  std::uint32_t pu = 0, pv = 0;   // pivot variables
  std::vector<Match> matches;
};

// Pivot pair of a match: (x_pu, x_pv).
std::uint64_t PivotOf(const Match& m, const RuleContext& ctx) {
  return PackPair(m.v[ctx.pu], m.v[ctx.pv]);
}

// Counts distinct pivot pairs over `idx` (optionally only head matches).
std::pair<std::uint64_t, std::uint64_t> PivotStats(
    const RuleContext& ctx, const std::vector<std::uint32_t>& idx) {
  std::unordered_set<std::uint64_t> all, heads;
  for (std::uint32_t t : idx) {
    const auto pp = PivotOf(ctx.matches[t], ctx);
    all.insert(pp);
    if (ctx.matches[t].head) heads.insert(pp);
  }
  return {all.size(), heads.size()};
}

// Collects const/var candidates over the active match subset.
std::vector<Candidate> GenerateCandidates(
    const RuleContext& ctx, const std::vector<std::uint32_t>& active,
    std::uint64_t min_supp, double conf_floor) {
  std::vector<Candidate> out;
  const auto nv = ctx.n;

  for (std::uint32_t i = 0; i < nv; ++i) {
    for (std::size_t ci = 0; ci < ctx.cols->size(); ++ci) {
      const Column& c = (*ctx.cols)[ci];
      // Sort active matches by the column value at variable i.
      std::vector<std::pair<double, std::uint32_t>> keyed;
      keyed.reserve(active.size());
      for (std::uint32_t t : active) {
        const auto row = ctx.matches[t].v[i];
        if (c.Valid(row)) keyed.emplace_back(c.Value(row), t);
      }
      if (keyed.size() < min_supp) continue;
      std::sort(keyed.begin(), keyed.end());

      // const candidate per distinct value group.
      std::size_t p = 0;
      while (p < keyed.size()) {
        std::size_t q = p + 1;
        while (q < keyed.size() && keyed[q].first == keyed[p].first) ++q;
        std::vector<std::uint32_t> group;
        group.reserve(q - p);
        // A value shared by a single entity only fingerprints that entity
        // ("populationTotal = 66736000" pins x2 to France), making conf=1
        // trivially — require at least two distinct entities.
        std::unordered_set<std::uint32_t> ents;
        for (std::size_t t = p; t < q; ++t) {
          group.push_back(keyed[t].second);
          ents.insert(ctx.matches[keyed[t].second].v[i]);
        }
        if (ents.size() < 2) {
          p = q;
          continue;
        }
        const auto [all, heads] = PivotStats(ctx, group);
        if (heads >= min_supp) {
          const double conf = static_cast<double>(heads) / all;
          if (conf >= conf_floor) {
            Candidate cand;
            cand.pred.kind = gar::PredKind::kConst;
            cand.pred.node = static_cast<gar::VertexID>(i);
            cand.pred.attr = c.name;
            cand.pred.op = gar::CmpOp::kEq;
            cand.pred.value = c.AttrVal(ctx.matches[group[0]].v[i]);
            cand.supp = heads;
            cand.body = all;
            cand.conf = conf;
            out.push_back(std::move(cand));
          }
        }
        p = q;
      }

      // var candidates: x_i.c = x_j.c for j > i (same column). Same
      // anti-fingerprint guard: the equality group must span at least two
      // distinct entities on both sides.
      for (std::uint32_t j = i + 1; j < nv; ++j) {
        std::vector<std::uint32_t> group;
        std::unordered_set<std::uint32_t> ents_i, ents_j;
        for (std::uint32_t t : active) {
          const auto ri = ctx.matches[t].v[i];
          const auto rj = ctx.matches[t].v[j];
          if (c.Valid(ri) && c.Valid(rj) && c.Value(ri) == c.Value(rj)) {
            group.push_back(t);
            ents_i.insert(ri);
            ents_j.insert(rj);
          }
        }
        if (group.size() < min_supp || ents_i.size() < 2 ||
            ents_j.size() < 2) {
          continue;
        }
        const auto [all, heads] = PivotStats(ctx, group);
        if (heads >= min_supp) {
          const double conf = static_cast<double>(heads) / all;
          if (conf >= conf_floor) {
            Candidate cand;
            cand.pred.kind = gar::PredKind::kVar;
            cand.pred.node = static_cast<gar::VertexID>(i);
            cand.pred.attr = c.name;
            cand.pred.op = gar::CmpOp::kEq;
            cand.pred.rhs_node = static_cast<gar::VertexID>(j);
            cand.pred.rhs_attr = c.name;
            cand.supp = heads;
            cand.body = all;
            cand.conf = conf;
            out.push_back(std::move(cand));
          }
        }
      }
    }
  }
  return out;
}

// Restricts `active` to matches satisfying the candidate predicate.
std::vector<std::uint32_t> FilterActive(const RuleContext& ctx,
                                        const std::vector<std::uint32_t>& active,
                                        const Candidate& cand) {
  const Column* col = nullptr;
  for (const auto& c : *ctx.cols) {
    if (c.name == cand.pred.attr) col = &c;
  }
  std::vector<std::uint32_t> next;
  next.reserve(active.size());
  for (std::uint32_t t : active) {
    const Match& m = ctx.matches[t];
    bool ok = false;
    if (cand.pred.kind == gar::PredKind::kConst) {
      const auto row = m.v[cand.pred.node];
      ok = col && col->Valid(row) &&
           col->AttrVal(row) == cand.pred.value;
    } else {  // kVar
      const auto ri = m.v[cand.pred.node];
      const auto rj = m.v[cand.pred.rhs_node];
      ok = col && col->Valid(ri) && col->Valid(rj) &&
           col->Value(ri) == col->Value(rj);
    }
    if (ok) next.push_back(t);
  }
  return next;
}

// Enumerates body matches of the rule's pattern (arbitrary connected
// topology, via PatternMatcher) and flags head satisfaction on the pivot
// pair. Returns false when the enumeration hit the match cap (rule skipped).
bool EnumerateMatches(const gar::miner::TripleGraph& g, const gar::GARule& rule,
                      std::uint64_t max_matches, RuleContext* ctx) {
  const auto& topo = rule.pattern;
  const auto n = static_cast<std::uint32_t>(topo.num_vertices());
  if (n < 2 || topo.num_edges() < 1 || !topo.has_edge_labels() ||
      !rule.consequent || rule.consequent->kind != gar::PredKind::kEdge) {
    return false;
  }
  const auto rh = rule.consequent->label;

  // Pivot variables: rule.pivot when present, else the consequent's.
  std::uint32_t pu = 0, pv = n - 1;
  if (rule.pivot.size() == 2) {
    pu = rule.pivot[0];
    pv = rule.pivot[1];
  } else {
    pu = rule.consequent->src;
    pv = rule.consequent->dst;
  }
  if (pu >= n || pv >= n) return false;

  // Expand the CSR pattern into an edge list for the generic matcher.
  gar::miner::PatternSpec spec;
  spec.num_vertices = n;
  for (std::uint32_t s = 0; s < n; ++s) {
    for (gar::EdgeIndex e = topo.offsets[s]; e < topo.offsets[s + 1]; ++e) {
      spec.edges.push_back({s, topo.indices[e], topo.edge_labels[e]});
    }
  }

  const gar::miner::PatternMatcher matcher(g);
  std::vector<std::uint32_t> embs;
  bool capped = false;
  const std::uint64_t n_emb = matcher.Enumerate(spec, max_matches, &embs, &capped);
  if (capped) return false;

  const bool typed = g.has_vertex_labels();
  auto label_ok = [&](std::uint32_t node, std::uint32_t vid) {
    return !typed || node >= rule.node_labels.size() ||
           rule.node_labels[node] == gar::kWildcardLabel ||
           g.VertexLabel(vid) == rule.node_labels[node];
  };
  auto has_edge = [&](std::uint32_t s, std::uint32_t d, std::uint32_t slot) {
    const auto [b, e] = g.OutEdgeRange(s);
    const std::uint32_t* dsts = g.OutEdgeDsts();
    auto lo = std::lower_bound(dsts + b, dsts + e, d);
    for (auto it = lo; it != dsts + e && *it == d; ++it) {
      if (g.OutEdgeSlots()[it - dsts] == slot) return true;
    }
    return false;
  };

  ctx->n = n;
  ctx->pu = pu;
  ctx->pv = pv;
  ctx->matches.clear();
  ctx->matches.reserve(n_emb);
  for (std::uint64_t i = 0; i < n_emb; ++i) {
    const std::uint32_t* emb = embs.data() + i * n;
    bool ok = true;
    for (std::uint32_t u = 0; u < n; ++u) {
      if (!label_ok(u, emb[u])) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    Match m;
    m.v.assign(emb, emb + n);
    m.head = has_edge(emb[pu], emb[pv], rh);
    ctx->matches.push_back(std::move(m));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    gar::miner::TripleGraph g =
        gar::miner::TripleGraph::LoadImmutableCSR(args.csr_dir,
                                                  /*with_inverses=*/true);
    std::cerr << "[mine_predicates] |V|=" << g.num_entities()
              << " |R|=" << g.num_relations() << " |E|=" << g.num_edges()
              << "\n";

    AttributesIO::Storage st;
    AttributesIO::Read(args.csr_dir + "/vertex_attr", &st);
    std::vector<Column> cols;
    for (const auto& a : st.columns) {
      if (a.type == mg_attr::ValueType::kInt) {
        cols.push_back({a.name, true, &a});
      } else if (a.type == mg_attr::ValueType::kFloat64) {
        cols.push_back({a.name, false, &a});
      }
    }
    std::cerr << "[mine_predicates] " << cols.size()
              << " numeric attribute columns\n";
    if (cols.empty()) {
      std::cerr << "[mine_predicates] nothing to mine over, exit\n";
      return 0;
    }

    std::vector<std::filesystem::path> rule_dirs;
    for (const auto& e : std::filesystem::directory_iterator(args.rules_root)) {
      if (e.is_directory() &&
          std::filesystem::exists(e.path() / "rule.yaml")) {
        rule_dirs.push_back(e.path());
      }
    }
    std::sort(rule_dirs.begin(), rule_dirs.end());
    std::cerr << "[mine_predicates] " << rule_dirs.size() << " rules\n";

    std::vector<gar::GARule> enriched;
    std::uint64_t skipped_truncated = 0, skipped_no_pred = 0;
    std::mutex mu;
    std::atomic<std::uint64_t> next{0};
    auto worker = [&]() {
      std::vector<gar::GARule> local;
      std::uint64_t local_trunc = 0, local_nopred = 0;
      for (;;) {
        const std::uint64_t i = next.fetch_add(1);
        if (i >= rule_dirs.size()) break;
        gar::GARule rule = gar::RuleLoader::Load(rule_dirs[i]);

        RuleContext ctx{&g, &cols, 0, 0, 0, {}};
        if (!EnumerateMatches(g, rule, args.max_matches, &ctx)) {
          ++local_trunc;
          continue;
        }
        std::vector<std::uint32_t> active(ctx.matches.size());
        std::iota(active.begin(), active.end(), 0);
        const auto [base_all, base_heads] = PivotStats(ctx, active);
        if (base_heads < args.min_supp) {
          ++local_nopred;
          continue;
        }
        double cur_conf = static_cast<double>(base_heads) / base_all;
        std::uint64_t cur_supp = base_heads;

        std::vector<gar::Predicate> preds;
        for (std::uint32_t round = 0; round < args.max_preds; ++round) {
          auto cands = GenerateCandidates(ctx, active, args.min_supp,
                                          std::max(args.min_conf, cur_conf));
          if (cands.empty()) break;
          std::sort(cands.begin(), cands.end(), [](const auto& a,
                                                   const auto& b) {
            if (a.conf != b.conf) return a.conf > b.conf;
            return a.supp > b.supp;
          });
          const Candidate& best = cands.front();
          if (best.conf <= cur_conf) break;
          active = FilterActive(ctx, active, best);
          preds.push_back(best.pred);
          cur_supp = best.supp;
          cur_conf = best.conf;
        }
        if (preds.empty()) {
          ++local_nopred;
          continue;
        }

        rule.antecedent = std::move(preds);
        rule.description =
            "base: supp=" + std::to_string(base_heads) +
            " conf=" + std::to_string(static_cast<double>(base_heads) /
                                      base_all) +
            " (without X)";
        rule.quality.support = cur_supp;
        rule.quality.confidence = cur_conf;
        rule.quality.miner = "predicate-miner-v1";
        local.push_back(std::move(rule));
      }
      std::lock_guard<std::mutex> lk(mu);
      std::move(local.begin(), local.end(), std::back_inserter(enriched));
      skipped_truncated += local_trunc;
      skipped_no_pred += local_nopred;
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < hw; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    std::cerr << "[mine_predicates] enriched: " << enriched.size()
              << " (skipped: " << skipped_truncated << " truncated/capped, "
              << skipped_no_pred << " without qualifying predicates)\n";

    gar::RuleStore::SaveDir(enriched, args.out_root);
    std::cerr << "[mine_predicates] wrote " << enriched.size()
              << " rule dirs to " << args.out_root << "\n";

    std::sort(enriched.begin(), enriched.end(), [](const auto& a, const auto& b) {
      if (a.quality.confidence != b.quality.confidence) {
        return a.quality.confidence > b.quality.confidence;
      }
      return a.quality.support > b.quality.support;
    });
    std::cout << "===== enriched GARs (top " << args.print_n << " of "
              << enriched.size() << ") =====\n";
    for (std::size_t i = 0; i < enriched.size() && i < args.print_n; ++i) {
      const auto& r = enriched[i];
      std::cout << "supp=" << r.quality.support
                << " conf=" << r.quality.confidence << "  " << r.name << "\n";
      for (const auto& p : r.antecedent) {
        std::ostringstream os;
        os << p;
        std::cout << "    X: " << os.str() << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
