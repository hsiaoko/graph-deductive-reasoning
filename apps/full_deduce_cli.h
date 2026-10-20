#pragma once

// Shared full-closure deduction CLI for apps/gar_reason.cu and the legacy
// deduce / deduce_gpu thin shells: argument parsing, input loading
// (bridge::LoadTripleGraph / bridge::LoadVertexAttrs / RuleLoader) and the
// summary/closure/matches reporting. All three apps go through this header
// so their logs and output layout stay byte-identical.
//
// Must be included from a .cu translation unit (vertex_attrs_loader.h pulls
// in MatrixGraph's CUDA-compiled AttributesIO).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "closure_output.h"
#include "gar/bridge/graph_loader.h"
#include "gar/bridge/vertex_attrs_loader.h"
#include "gar/engine/deduction/deduction_engine.h"
#include "gar/rule_loader.h"

namespace gar_apps {

struct FullDeduceArgs {
  std::string csr_dir;
  std::string rules_root;
  std::string out_root = "closure";
  gar::engine::deduction::ChaseConfig cfg;
  std::size_t print_n = 20;
};

// Parses one full-deduction option into *a; returns false when `k` is not
// one. `next` yields the option's value. --threads is accepted only when
// allow_threads (gar_reason; the legacy apps keep their original CLIs).
template <typename NextFn>
bool ParseFullDeduceOption(const std::string& k, NextFn&& next,
                           FullDeduceArgs* a, bool allow_threads) {
  if (k == "--csr") a->csr_dir = next();
  else if (k == "--rules") a->rules_root = next();
  else if (k == "--out") a->out_root = next();
  else if (k == "--max-rounds")
    a->cfg.max_rounds = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--max-derivations") a->cfg.max_derivations = std::stoull(next());
  else if (k == "--max-matches")
    a->cfg.max_matches_per_rule = std::stoull(next());
  else if (k == "--no-matches") a->cfg.keep_matches = false;
  else if (k == "--print") a->print_n = std::stoull(next());
  else if (allow_threads && k == "--threads")
    a->cfg.threads = static_cast<std::uint32_t>(std::stoul(next()));
  else return false;
  return true;
}

// Legacy-app parse: identical to the old deduce/deduce_gpu CLIs, usage text
// included (prog is "deduce" resp. "deduce_gpu").
inline FullDeduceArgs ParseFullDeduceArgs(int argc, char** argv,
                                          const char* prog) {
  FullDeduceArgs a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << k << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (!ParseFullDeduceOption(k, next, &a, /*allow_threads=*/false)) {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.csr_dir.empty() || a.rules_root.empty()) {
    std::cerr << "usage: " << prog << " --csr <csr_dir> --rules <rules_root> "
                 "--out <out_dir> [--max-rounds N] [--max-derivations N] "
                 "[--max-matches N] [--no-matches] [--print N]\n";
    std::exit(2);
  }
  return a;
}

struct FullDeduceInputs {
  gar::graph::TripleGraph graph;
  std::vector<gar::GARule> rules;
  std::vector<std::string> rule_ids;
  gar::graph::VertexAttrs attrs;
};

// Loads the graph (with inverse relations), the rule set and the vertex
// attributes, logging with the "[<log_tag>] " prefix of the legacy apps.
inline FullDeduceInputs LoadFullDeduceInputs(const FullDeduceArgs& args,
                                             const char* log_tag) {
  FullDeduceInputs in{gar::bridge::LoadTripleGraph(args.csr_dir,
                                                   /*with_inverses=*/true),
                      {}, {}, {}};
  std::cerr << "[" << log_tag << "] |V|=" << in.graph.num_entities()
            << " |R|=" << in.graph.num_relations()
            << " |E|=" << in.graph.num_edges() << "\n";

  std::vector<std::filesystem::path> rule_dirs;
  for (const auto& e :
       std::filesystem::directory_iterator(args.rules_root)) {
    if (e.is_directory() &&
        std::filesystem::exists(e.path() / "rule.yaml")) {
      rule_dirs.push_back(e.path());
    }
  }
  std::sort(rule_dirs.begin(), rule_dirs.end());
  for (const auto& d : rule_dirs) {
    in.rules.push_back(gar::RuleLoader::Load(d));
    in.rule_ids.push_back(d.filename().string());
  }
  std::cerr << "[" << log_tag << "] " << in.rules.size() << " rules\n";

  in.attrs = gar::bridge::LoadVertexAttrs(args.csr_dir);
  std::cerr << "[" << log_tag << "] " << in.attrs.columns.size()
            << " numeric attribute columns\n";
  return in;
}

// Writes the closure outputs and prints the final summary, byte-identical
// to the legacy apps (summary_title is "chase" resp. "chase (GPU)").
inline void ReportFullDeduce(
    const FullDeduceArgs& args, const FullDeduceInputs& in,
    const gar::engine::deduction::ChaseResult& res, const char* log_tag,
    const char* summary_title) {
  std::cerr << "[" << log_tag << "] rounds=" << res.rounds
            << " derived=" << res.derived.size()
            << (res.fixpoint ? " (fixpoint reached)" : " (STOPPED by cap)")
            << "\n";

  // ---- write outputs ----
  WriteClosureOutputs(in.graph, in.rule_ids, res, args.out_root,
                      args.cfg.keep_matches);

  // ---- CLI summary ----
  std::vector<std::size_t> order(in.rules.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return res.rules[a].derived > res.rules[b].derived;
  });
  std::cout << "===== " << summary_title << ": " << res.rounds << " rounds, "
            << res.derived.size() << " derived facts"
            << (res.fixpoint ? " (fixpoint)" : " (capped)") << " =====\n";
  for (std::size_t k = 0; k < order.size() && k < args.print_n; ++k) {
    const std::size_t i = order[k];
    const auto& st = res.rules[i];
    if (st.derived == 0 && st.embeddings == 0 && !st.skipped) continue;
    std::cout << in.rule_ids[i] << "  emb=" << st.embeddings
              << " x_ok=" << st.x_satisfied << " derived=" << st.derived
              << (st.match_capped ? " [capped]" : "")
              << (st.skipped ? " [skipped: " + st.note + "]" : "") << "\n  "
              << in.rules[i].name << "\n";
  }
}

}  // namespace gar_apps
