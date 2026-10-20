#pragma once

// Shared closure-output writer for the deduce apps (CPU + GPU): writes
// summary.yaml / closure_edges.tsv / matches/<rule>.tsv under the output
// directory, in the exact same layout for both engines so their results can
// be diffed directly.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gar/engine/deduction/deduction_engine.h"
#include "gar/graph/triple_graph.h"

namespace gar_apps {

inline void WriteClosureOutputs(const gar::graph::TripleGraph& g,
                                const std::vector<std::string>& rule_ids,
                                const gar::engine::deduction::ChaseResult& res,
                                const std::string& out_root,
                                bool write_matches) {
  std::filesystem::create_directories(out_root);
  {
    std::ofstream f(out_root + "/closure_edges.tsv");
    f << "# src\trelation\tdst\tround\trule\n";
    for (const auto& e : res.derived) {
      f << g.EntityName(e.src) << '\t' << g.SlotName(e.slot) << '\t'
        << g.EntityName(e.dst) << '\t' << e.round << '\t'
        << rule_ids[e.rule] << "\n";
    }
  }
  {
    std::ofstream f(out_root + "/summary.yaml");
    f << "rounds: " << res.rounds << "\nfixpoint: "
      << (res.fixpoint ? "true" : "false")
      << "\nderived_total: " << res.derived.size() << "\nrules:\n";
    for (std::size_t i = 0; i < rule_ids.size(); ++i) {
      const auto& st = res.rules[i];
      f << "  - id: " << rule_ids[i] << "\n"
        << "    embeddings: " << st.embeddings << "\n"
        << "    x_satisfied: " << st.x_satisfied << "\n"
        << "    derived: " << st.derived << "\n";
      if (st.match_capped) f << "    match_capped: true\n";
      if (st.skipped) f << "    skipped: \"" << st.note << "\"\n";
    }
  }
  if (write_matches) {
    const std::string mdir = out_root + "/matches";
    std::filesystem::create_directories(mdir);
    for (std::size_t i = 0; i < rule_ids.size(); ++i) {
      const auto& rm = res.matches[i];
      if (rm.n == 0 || rm.embs.empty()) continue;
      std::ofstream f(mdir + "/" + rule_ids[i] + ".tsv");
      f << "#";
      for (std::uint32_t u = 0; u < rm.n; ++u) f << "\tx" << u;
      f << "\tx_ok\thead\n";
      const std::uint64_t n_rows = rm.x_ok.size();
      for (std::uint64_t t = 0; t < n_rows; ++t) {
        for (std::uint32_t u = 0; u < rm.n; ++u) {
          f << g.EntityName(rm.embs[t * rm.n + u]) << "\t";
        }
        f << (rm.x_ok[t] ? 1 : 0) << "\t" << (rm.head_hit[t] ? 1 : 0)
          << "\n";
      }
    }
  }
}

}  // namespace gar_apps
