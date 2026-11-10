// validate_rules: re-verifies mined GAR rules on a knowledge graph — the
// "inference check" counterpart of the mining apps. For every rule it
// re-enumerates the pattern embeddings (graph::PatternMatcher, homomorphism
// + vertex-label post-filter, the miners' counting convention), evaluates X
// (gar::graph::EvalAntecedent), and recomputes the pivot-pair quality:
//
//   support    = |{(emb[pu], emb[pv]) : topology ∧ label ∧ X ∧ head ∈ G}|
//   confidence = support / |{(emb[pu], emb[pv]) : topology ∧ label ∧ X}|
//
// then diffs both against the values stored in rule.yaml (mining time):
//   PASS   support identical and |Δconf| <= --conf-tol
//   DRIFT  recomputed values differ (graph changed or a counting bug)
//   SKIP   rule shape unsupported here (ml predicate, wildcards, ...)
//
// Usage:
//   validate_rules --csr <immutable_csr_dir> --rules <rules_root>
//                  [--max-matches 100000000] [--conf-tol 1e-9]
//                  [--out <report.tsv>] [--print 20]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>  // must precede MatrixGraph headers (format_converter.cuh)
#include <unordered_set>
#include <vector>
// NOTE: do not include <execution> here — it drags in AVX512 intrinsics
// headers that nvcc + GCC 12 cannot compile.

#include "gar/bridge/graph_loader.h"
#include "gar/bridge/vertex_attrs_loader.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"
#include "gar/graph/triple_graph.h"
#include "gar/rule_loader.h"

namespace {

struct Args {
  std::string csr_dir;
  std::string rules_root;
  std::string out_path;
  std::uint64_t max_matches = 100'000'000;
  double conf_tol = 1e-9;
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
    else if (k == "--out") a.out_path = next();
    else if (k == "--max-matches") a.max_matches = std::stoull(next());
    else if (k == "--conf-tol") a.conf_tol = std::stod(next());
    else if (k == "--print") a.print_n = std::stoull(next());
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.csr_dir.empty() || a.rules_root.empty()) {
    std::cerr << "usage: validate_rules --csr <csr_dir> --rules <rules_root> "
                 "[--max-matches N] [--conf-tol F] [--out <report.tsv>] "
                 "[--print N]\n";
    std::exit(2);
  }
  return a;
}

bool HasEdge(const gar::graph::TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  const auto [b, e] = g.OutEdgeRange(s);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  auto lo = std::lower_bound(dsts + b, dsts + e, d);
  for (auto it = lo; it != dsts + e && *it == d; ++it) {
    if (g.OutEdgeSlots()[it - dsts] == slot) return true;
  }
  return false;
}

enum class Verdict { kPass, kDrift, kSkip };

const char* ToString(Verdict v) {
  switch (v) {
    case Verdict::kPass: return "PASS";
    case Verdict::kDrift: return "DRIFT";
    case Verdict::kSkip: return "SKIP";
  }
  return "?";
}

struct RuleReport {
  Verdict verdict = Verdict::kPass;
  std::string note;
  std::uint64_t embeddings = 0;   // topology matches after label filter
  std::uint64_t x_ok = 0;         // embeddings satisfying X
  std::uint64_t body = 0;         // distinct pivot pairs (body ∧ X)
  std::uint64_t support = 0;      // distinct pivot pairs (+ head ∈ G)
  double confidence = 0.0;        // support / body
  bool match_capped = false;
};

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    gar::graph::TripleGraph g =
        gar::bridge::LoadTripleGraph(args.csr_dir,
                                     /*with_inverses=*/true);
    std::cerr << "[validate_rules] |V|=" << g.num_entities()
              << " |R|=" << g.num_relations() << " |E|=" << g.num_edges()
              << "\n";

    std::vector<std::filesystem::path> rule_dirs;
    for (const auto& e :
         std::filesystem::directory_iterator(args.rules_root)) {
      if (e.is_directory() &&
          std::filesystem::exists(e.path() / "rule.yaml")) {
        rule_dirs.push_back(e.path());
      }
    }
    std::sort(rule_dirs.begin(), rule_dirs.end());
    std::vector<gar::GARule> rules;
    std::vector<std::string> rule_ids;
    for (const auto& d : rule_dirs) {
      rules.push_back(gar::RuleLoader::Load(d));
      rule_ids.push_back(d.filename().string());
    }
    std::cerr << "[validate_rules] " << rules.size() << " rules\n";

    const gar::graph::VertexAttrs attrs =
        gar::bridge::LoadVertexAttrs(args.csr_dir);
    const gar::graph::VertexAttrs* attrs_p =
        attrs.columns.empty() ? nullptr : &attrs;

    const gar::graph::PatternMatcher matcher(g);
    const bool typed = g.has_vertex_labels();
    std::vector<RuleReport> reports(rules.size());
    std::size_t n_pass = 0, n_drift = 0, n_skip = 0;

    for (std::size_t i = 0; i < rules.size(); ++i) {
      const gar::GARule& r = rules[i];
      RuleReport& rep = reports[i];
      const auto n = static_cast<std::uint32_t>(r.num_vertices());
      auto skip = [&](const char* note) {
        rep.verdict = Verdict::kSkip;
        rep.note = note;
      };
      if (n < 2 || r.num_edges() < 1 ||
          !r.has_edge_labels()) {
        skip("pattern missing or unlabeled");
        ++n_skip;
        continue;
      }
      bool bad = false;
      for (const auto& pred : r.antecedent) {
        if (pred.kind == gar::PredKind::kMl) {
          skip("ml predicate unsupported");
          bad = true;
          break;
        }
        if (pred.kind == gar::PredKind::kEdge &&
            pred.label == gar::kWildcardLabel) {
          skip("unresolved edge predicate label");
          bad = true;
          break;
        }
      }
      if (bad) {
        ++n_skip;
        continue;
      }

      gar::graph::PatternSpec spec;
      spec.num_vertices = n;
      for (std::size_t e = 0; e < r.edge_src.size(); ++e) {
        spec.edges.push_back(
            {static_cast<std::uint32_t>(r.edge_src[e]),
             static_cast<std::uint32_t>(r.edge_dst[e]),
             static_cast<gar::LabelID>(r.edge_labels[e])});
      }
      bool has_head = false;
      std::uint32_t head_slot = 0, pu = 0, pv = 0;
      if (r.consequent && r.consequent->kind == gar::PredKind::kEdge &&
          r.consequent->label != gar::kWildcardLabel) {
        has_head = true;
        head_slot = r.consequent->label;
        pu = r.consequent->src;
        pv = r.consequent->dst;
      }
      if (r.pivot.size() == 2) {  // same pivot override as the chase
        pu = r.pivot[0];
        pv = r.pivot[1];
      }

      std::vector<std::uint32_t> embs;
      const std::uint64_t n_emb =
          matcher.Enumerate(spec, args.max_matches, &embs, &rep.match_capped);

      // Pivot-pair quality recompute (the miners' convention): X is
      // evaluated per embedding, pairs are deduplicated, and the head is
      // checked once per newly seen pair.
      std::unordered_set<std::uint64_t> body_pairs, head_pairs;
      for (std::uint64_t t = 0; t < n_emb; ++t) {
        const std::uint32_t* emb = embs.data() + t * n;
        bool label_ok = true;
        for (std::uint32_t u = 0; u < n; ++u) {
          if (typed && u < r.node_labels.size() &&
              r.node_labels[u] != gar::kWildcardLabel &&
              g.VertexLabel(emb[u]) != r.node_labels[u]) {
            label_ok = false;
            break;
          }
        }
        if (!label_ok) continue;
        ++rep.embeddings;
        if (!gar::graph::EvalAntecedent(r.antecedent, emb, attrs_p, g)) {
          continue;
        }
        ++rep.x_ok;
        const std::uint64_t key =
            gar::graph::PackPair(emb[pu], emb[pv]);
        if (!body_pairs.insert(key).second) continue;
        if (has_head && HasEdge(g, emb[pu], emb[pv], head_slot)) {
          head_pairs.insert(key);
        }
      }
      rep.body = body_pairs.size();
      rep.support = head_pairs.size();
      rep.confidence =
          rep.body == 0 ? 0.0
                        : static_cast<double>(rep.support) / rep.body;

      if (!has_head) {
        skip("query rule (no consequent); body counted only");
        ++n_skip;
        continue;
      }
      const bool pass = rep.support == r.quality.support &&
                        std::fabs(rep.confidence - r.quality.confidence) <=
                            args.conf_tol;
      rep.verdict = pass ? Verdict::kPass : Verdict::kDrift;
      if (pass) {
        ++n_pass;
      } else {
        ++n_drift;
        rep.note = rep.match_capped ? "match cap hit (lower bounds)" : "";
      }
    }

    // ---- report ----
    std::cout << "===== validate_rules: " << n_pass << " PASS, " << n_drift
              << " DRIFT, " << n_skip << " SKIP (of " << rules.size()
              << ") =====\n";
    std::size_t shown = 0;
    for (std::size_t i = 0; i < rules.size() && shown < args.print_n; ++i) {
      const auto& rep = reports[i];
      if (rep.verdict == Verdict::kPass) continue;
      std::cout << rule_ids[i] << " [" << ToString(rep.verdict) << "] "
                << "support stored=" << rules[i].quality.support
                << " recomputed=" << rep.support
                << " | conf stored=" << rules[i].quality.confidence
                << " recomputed=" << rep.confidence << " (body=" << rep.body
                << ", emb=" << rep.embeddings << ", x_ok=" << rep.x_ok << ")"
                << (rep.note.empty() ? "" : "  " + rep.note) << "\n  "
                << rules[i].name << "\n";
      ++shown;
    }
    if (n_drift + n_skip > shown) {
      std::cout << "  ... " << (n_drift + n_skip - shown)
                << " more (see --out report)\n";
    }

    if (!args.out_path.empty()) {
      std::ofstream f(args.out_path);
      f << "# rule\tverdict\tsupport_stored\tsupport_recomputed\tconf_stored"
           "\tconf_recomputed\tbody\tembeddings\tx_ok\tcapped\tnote\n";
      for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& rep = reports[i];
        f << rule_ids[i] << '\t' << ToString(rep.verdict) << '\t'
          << rules[i].quality.support << '\t' << rep.support << '\t'
          << rules[i].quality.confidence << '\t' << rep.confidence << '\t'
          << rep.body << '\t' << rep.embeddings << '\t' << rep.x_ok << '\t'
          << (rep.match_capped ? 1 : 0) << '\t' << rep.note << "\n";
      }
      std::cerr << "[validate_rules] report written to " << args.out_path
                << "\n";
    }
    return n_drift == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
