// deduce: full logical closure (chase) of a GAR rule set over a KG, CPU.
//
//   G_{i+1} = G_i ∪ { p₀(emb) | rule Q[x̄](X → p₀), emb ∈ Q(G_i), X(emb) }
//
// Topology matching reuses graph::PatternMatcher (homomorphism, edge labels,
// vertex-label constraints); X predicates (const/var/exists/edge) are
// evaluated per embedding against the vertex_attr table and the current
// graph. Derived edges are merged into the graph after each round, so the
// iteration runs to a fixpoint (bounded by --max-rounds / --max-derivations).
//
// Usage:
//   deduce --csr <immutable_csr_dir> --rules <rules_root> --out <out_dir>
//          [--max-rounds 100] [--max-derivations 10000000]
//          [--max-matches 1000000] [--no-matches] [--print 20]
//
// Output under <out_dir>:
//   summary.yaml        per-rule embeddings / X-satisfied / derived + totals
//   closure_edges.tsv   src \t relation \t dst \t round \t rule_id
//   matches/<rule>.tsv  final-round embeddings (entity names) + x_ok + head
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// NOTE: do not include <execution> here — it drags in AVX512 intrinsics
// headers that nvcc + GCC 12 cannot compile (MatrixGraph avoids it in .cu
// files for the same reason).
#include "closure_output.h"
#include "gar/bridge/graph_loader.h"
#include "gar/bridge/vertex_attrs_loader.h"
#include "gar/reason/chase.h"
#include "gar/rule_loader.h"

namespace {

struct Args {
  std::string csr_dir;
  std::string rules_root;
  std::string out_root = "closure";
  gar::reason::ChaseConfig cfg;
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
    else if (k == "--max-rounds")
      a.cfg.max_rounds = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--max-derivations") a.cfg.max_derivations = std::stoull(next());
    else if (k == "--max-matches")
      a.cfg.max_matches_per_rule = std::stoull(next());
    else if (k == "--no-matches") a.cfg.keep_matches = false;
    else if (k == "--print") a.print_n = std::stoull(next());
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.csr_dir.empty() || a.rules_root.empty()) {
    std::cerr << "usage: deduce --csr <csr_dir> --rules <rules_root> "
                 "--out <out_dir> [--max-rounds N] [--max-derivations N] "
                 "[--max-matches N] [--no-matches] [--print N]\n";
    std::exit(2);
  }
  return a;
}

// Loads vertex_attr/ (if present) into the engine's plain CPU column type.
gar::graph::VertexAttrs LoadAttrs(const std::string& csr_dir) {
  return gar::bridge::LoadVertexAttrs(csr_dir);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    gar::graph::TripleGraph g =
        gar::bridge::LoadTripleGraph(args.csr_dir,
                                     /*with_inverses=*/true);
    std::cerr << "[deduce] |V|=" << g.num_entities()
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
    std::cerr << "[deduce] " << rules.size() << " rules\n";

    gar::graph::VertexAttrs attrs = LoadAttrs(args.csr_dir);
    std::cerr << "[deduce] " << attrs.columns.size()
              << " numeric attribute columns\n";

    gar::reason::ChaseEngine engine(
        g, attrs.columns.empty() ? nullptr : &attrs);
    gar::reason::ChaseResult res = engine.Run(rules, args.cfg);
    std::cerr << "[deduce] rounds=" << res.rounds
              << " derived=" << res.derived.size()
              << (res.fixpoint ? " (fixpoint reached)" : " (STOPPED by cap)")
              << "\n";

    // ---- write outputs ----
    gar_apps::WriteClosureOutputs(g, rule_ids, res, args.out_root,
                                  args.cfg.keep_matches);

    // ---- CLI summary ----
    std::vector<std::size_t> order(rules.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return res.rules[a].derived > res.rules[b].derived;
    });
    std::cout << "===== chase: " << res.rounds << " rounds, "
              << res.derived.size() << " derived facts"
              << (res.fixpoint ? " (fixpoint)" : " (capped)") << " =====\n";
    for (std::size_t k = 0; k < order.size() && k < args.print_n; ++k) {
      const std::size_t i = order[k];
      const auto& st = res.rules[i];
      if (st.derived == 0 && st.embeddings == 0 && !st.skipped) continue;
      std::cout << rule_ids[i] << "  emb=" << st.embeddings
                << " x_ok=" << st.x_satisfied << " derived=" << st.derived
                << (st.match_capped ? " [capped]" : "")
                << (st.skipped ? " [skipped: " + st.note + "]" : "") << "\n  "
                << rules[i].name << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
