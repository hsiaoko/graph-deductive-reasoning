// deduce: full logical closure (chase) of a GAR rule set over a KG, CPU.
// Thin shell over engine::deduction::CpuChaseEngine; argument parsing,
// input loading and output reporting are shared with gar_reason and
// deduce_gpu via full_deduce_cli.h.
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
#include <exception>
#include <iostream>

#include "full_deduce_cli.h"
#include "gar/engine/deduction/cpu_chase_engine.h"

int main(int argc, char** argv) {
  const gar_apps::FullDeduceArgs args =
      gar_apps::ParseFullDeduceArgs(argc, argv, "deduce");
  try {
    gar_apps::FullDeduceInputs in =
        gar_apps::LoadFullDeduceInputs(args, "deduce");
    gar::engine::deduction::CpuChaseEngine engine(
        in.graph, in.attrs.columns.empty() ? nullptr : &in.attrs);
    const auto res = engine.Run(in.rules, args.cfg);
    gar_apps::ReportFullDeduce(args, in, res, "deduce", "chase");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
