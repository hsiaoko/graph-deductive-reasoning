// Mines GAR rules with arbitrary small connected body topologies (chains,
// stars, triangles, ...) and writes each rule as a directory
// (rule.yaml + csr_offsets.bin + csr_indices.bin [+ csr_edge_labels.bin])
// under the output root, printing a one-line description of each rule.
//
// This is the generic-topology counterpart of mine_rules (ChainMiner):
// on chain patterns of length <= 2 both miners produce identical rule sets.
//
// Usage:
//   mine_graph_rules (--graph <triple_graph_dir> | --csr <immutable_csr_dir>)
//                    --out <rules_root_dir>
//                    [--min-supp 50] [--min-conf 0.05] [--max-edges 3]
//                    [--min-body-edges 1] [--pair-cap 4000000] [--match-cap 2000000]
//                    [--dataset ROOT_PATH] [--print 20]
//
// --graph reads the TripleGraph binary layout (tools/prepare_triples.py);
// --csr reads a MatrixGraph ImmutableCSR directory with edge_attr and dict
// (kg_loader convert), which is the canonical format for downstream
// MatrixGraph computation. The graph is always loaded with inverse
// relations: the generic matcher walks edges in both directions.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

#include "gar/bridge/graph_loader.h"
#include "gar/miner/pattern_miner.h"
#include "gar/rule_store.h"

namespace {

struct Args {
  std::string graph_dir;
  std::string csr_dir;
  std::string out_root = "rules";
  std::size_t print_n = 20;
  gar::miner::PatternMinerConfig cfg;
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
    if (k == "--graph") a.graph_dir = next();
    else if (k == "--csr") a.csr_dir = next();
    else if (k == "--out") a.out_root = next();
    else if (k == "--min-supp") a.cfg.min_support = std::stoull(next());
    else if (k == "--min-conf") a.cfg.min_confidence = std::stod(next());
    else if (k == "--max-edges")
      a.cfg.max_body_edges = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--min-body-edges")
      a.cfg.min_body_edges = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--pair-cap") a.cfg.pair_cap = std::stoull(next());
    else if (k == "--match-cap") a.cfg.match_cap = std::stoull(next());
    else if (k == "--dataset") a.cfg.dataset_root = next();
    else if (k == "--print") a.print_n = std::stoull(next());
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.graph_dir.empty() == a.csr_dir.empty()) {
    std::cerr << "usage: mine_graph_rules (--graph <triple_graph_dir> | --csr "
                 "<immutable_csr_dir>) --out <rules_root_dir> [--min-supp N] "
                 "[--min-conf F] [--max-edges K] [--min-body-edges K] "
                 "[--pair-cap N] "
                 "[--match-cap N] [--dataset ROOT_PATH] [--print N]\n";
    std::exit(2);
  }
  // Dataset provenance defaults to the input root path.
  if (a.cfg.dataset_root.empty()) {
    a.cfg.dataset_root = a.graph_dir.empty() ? a.csr_dir : a.graph_dir;
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    gar::graph::TripleGraph g =
        args.graph_dir.empty()
            ? gar::bridge::LoadTripleGraph(args.csr_dir,
                                           /*with_inverses=*/true)
            : gar::graph::TripleGraph::Load(args.graph_dir,
                                            /*with_inverses=*/true);
    std::cerr << "[mine_graph_rules] |V|=" << g.num_entities()
              << " |R|=" << g.num_relations() << " |E|=" << g.num_edges()
              << "\n";

    gar::miner::PatternMiner miner(g, args.cfg);
    auto rules = miner.Mine();
    const auto& st = miner.stats();
    std::cerr << "[mine_graph_rules] patterns evaluated: "
              << st.patterns_evaluated << " (capped: " << st.patterns_capped
              << ")"
              << ", rules emitted: " << st.rules_emitted << "\n";

    gar::RuleStore::SaveDir(rules, args.out_root);
    std::cerr << "[mine_graph_rules] wrote " << rules.size()
              << " rule dirs to " << args.out_root << "\n";

    // One-line description per rule, best confidence first.
    std::sort(rules.begin(), rules.end(),
              [](const gar::GARule& a, const gar::GARule& b) {
                if (a.quality.confidence != b.quality.confidence) {
                  return a.quality.confidence > b.quality.confidence;
                }
                return a.quality.support > b.quality.support;
              });
    std::cout << "===== mined GARs (top " << args.print_n << " of "
              << rules.size() << ") =====\n";
    for (std::size_t i = 0; i < rules.size() && i < args.print_n; ++i) {
      const auto& r = rules[i];
      std::cout << "supp=" << r.quality.support
                << " conf=" << r.quality.confidence << "  " << r.name << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
