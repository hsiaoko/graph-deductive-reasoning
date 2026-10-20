// Mines GAR chain rules from a graph G and writes each rule as a directory
// (rule.yaml + csr_offsets.bin + csr_indices.bin [+ csr_edge_labels.bin])
// under the output root, printing a one-line description of each rule.
//
// Usage:
//   mine_rules (--graph <triple_graph_dir> | --csr <immutable_csr_dir>)
//              --out <rules_root_dir>
//              [--min-supp 50] [--min-conf 0.05] [--max-body-len 2]
//              [--min-body-len 1] [--pair-cap 4000000] [--no-inverses]
//              [--no-typed] [--dataset ROOT_PATH] [--print 20]
//
// --graph reads the TripleGraph binary layout (tools/prepare_triples.py);
// --csr reads a MatrixGraph ImmutableCSR directory with edge_attr and dict
// (kg_loader convert), which is the canonical format for downstream
// MatrixGraph computation.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "gar/miner/chain_miner.h"
#include "gar/rule_store.h"

namespace {

struct Args {
  std::string graph_dir;
  std::string csr_dir;
  std::string out_root = "rules";
  std::size_t print_n = 20;
  gar::miner::MinerConfig cfg;
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
    else if (k == "--max-body-len")
      a.cfg.max_body_len = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--min-body-len")
      a.cfg.min_body_len = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--pair-cap") a.cfg.pair_cap = std::stoull(next());
    else if (k == "--no-inverses") a.cfg.with_inverses = false;
    else if (k == "--no-typed") a.cfg.use_vertex_labels = false;
    else if (k == "--dataset") a.cfg.dataset_root = next();
    else if (k == "--print") a.print_n = std::stoull(next());
    else {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.graph_dir.empty() == a.csr_dir.empty()) {
    std::cerr << "usage: mine_rules (--graph <triple_graph_dir> | --csr "
                 "<immutable_csr_dir>) --out <rules_root_dir> [--min-supp N] "
                 "[--min-conf F] [--max-body-len K] [--min-body-len K] [--pair-cap N] "
                 "[--no-inverses] [--no-typed] [--dataset ROOT_PATH] [--print N]\n";
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
    gar::miner::TripleGraph g =
        args.graph_dir.empty()
            ? gar::miner::TripleGraph::LoadImmutableCSR(
                  args.csr_dir, args.cfg.with_inverses)
            : gar::miner::TripleGraph::Load(args.graph_dir,
                                            args.cfg.with_inverses);
    std::cerr << "[mine_rules] |V|=" << g.num_entities()
              << " |R|=" << g.num_relations() << " |E|=" << g.num_edges();
    if (g.has_vertex_labels()) {
      std::cerr << " |L|=" << g.num_labels() << " (typed patterns)";
    }
    std::cerr << "\n";

    gar::miner::ChainMiner miner(g, args.cfg);
    auto rules = miner.Mine();
    const auto& st = miner.stats();
    std::cerr << "[mine_rules] chains evaluated: " << st.chains_evaluated
              << " (capped: " << st.chains_capped << ")"
              << ", rules emitted: " << st.rules_emitted << "\n";

    gar::RuleStore::SaveDir(rules, args.out_root);
    std::cerr << "[mine_rules] wrote " << rules.size() << " rule dirs to "
              << args.out_root << "\n";

    // One-line description per rule, best confidence first.
    std::sort(rules.begin(), rules.end(), [](const gar::GARule& a,
                                             const gar::GARule& b) {
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
