// deduce_gpu: full logical closure (chase) of a GAR rule set over a KG,
// with the per-rule match + X-evaluation + head emission running on the GPU
// (MatrixGraph's task::GARDeduce). Thin shell over
// gar::core::engine::deduction::GpuDeduceEngine; argument parsing, input loading and
// output reporting are shared with gar_reason and deduce via
// full_deduce_cli.h. The host-side round driver lives in the engine and is
// SHARED with the CPU chase (gar::core::graph::MergeRoundFacts), so both engines
// produce the same derived fact set round by round (row order inside a
// round may differ: GPU emission is atomicAdd-ordered).
//
// Usage:
//   deduce_gpu --csr <immutable_csr_dir> --rules <rules_root> --out <out_dir>
//              [--max-rounds 100] [--max-derivations 10000000]
//              [--max-matches 1000000] [--no-matches] [--print 20]
//
// Output under <out_dir>: identical layout to the CPU deduce
// (summary.yaml, closure_edges.tsv, matches/<rule>.tsv).
#include <exception>
#include <iostream>

#include "full_deduce_cli.h"
#include "gar/engine/deduction/gpu_deduce_engine.h"

int main(int argc, char** argv) {
  const gar_apps::FullDeduceArgs args =
      gar_apps::ParseFullDeduceArgs(argc, argv, "deduce_gpu");
  try {
    gar_apps::FullDeduceInputs in =
        gar_apps::LoadFullDeduceInputs(args, "deduce_gpu");
    gar::core::engine::chase::GpuDeduceEngine engine(
        in.graph, in.attrs.columns.empty() ? nullptr : &in.attrs);
    const auto res = engine.Run(in.rules, args.cfg);
    gar_apps::ReportFullDeduce(args, in, res, "deduce_gpu", "chase (GPU)");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
