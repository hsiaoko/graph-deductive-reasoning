// gar_reason: unified entry point for the deduction engine family.
//
// --mode full runs the whole rule set to a (capped) fixpoint, on the CPU
// chase (engine::deduction::CpuChaseEngine, oneTBB) or the GPU engine
// (engine::deduction::GpuDeduceEngine, MatrixGraph task::GARDeduce); both
// engines share the same semantics and the same output layout as the legacy
// deduce / deduce_gpu apps. The argument surface is structured so further
// modes (agent-guided deduction) can plug in later.
//
// Usage:
//   gar_reason --mode full --engine cpu|gpu --csr <immutable_csr_dir>
//              --rules <rules_root> --out <out_dir>
//              [--max-rounds 100] [--max-derivations 10000000]
//              [--max-matches 1000000] [--no-matches] [--print 20]
//              [--threads 0]
//
// Output under <out_dir>:
//   summary.yaml        per-rule embeddings / X-satisfied / derived + totals
//   closure_edges.tsv   src \t relation \t dst \t round \t rule_id
//   matches/<rule>.tsv  final-round embeddings (entity names) + x_ok + head
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "full_deduce_cli.h"
#include "gar/engine/deduction/cpu_chase_engine.h"
#include "gar/engine/deduction/gpu_deduce_engine.h"

namespace {

struct Args {
  std::string mode;    // "full" (only mode for now)
  std::string engine;  // "cpu" | "gpu"
  gar_apps::FullDeduceArgs full;
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
    if (k == "--mode") a.mode = next();
    else if (k == "--engine") a.engine = next();
    else if (!gar_apps::ParseFullDeduceOption(k, next, &a.full,
                                              /*allow_threads=*/true)) {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.mode != "full" || (a.engine != "cpu" && a.engine != "gpu") ||
      a.full.csr_dir.empty() || a.full.rules_root.empty()) {
    std::cerr << "usage: gar_reason --mode full --engine cpu|gpu "
                 "--csr <csr_dir> --rules <rules_root> --out <out_dir> "
                 "[--max-rounds N] [--max-derivations N] [--max-matches N] "
                 "[--no-matches] [--print N] [--threads N]\n";
    std::exit(2);
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    // Log tag / summary title mirror the legacy apps, so gar_reason's
    // output is byte-identical to deduce (--engine cpu) resp. deduce_gpu
    // (--engine gpu).
    const bool cpu = args.engine == "cpu";
    const char* log_tag = cpu ? "deduce" : "deduce_gpu";
    const char* summary_title = cpu ? "chase" : "chase (GPU)";

    gar_apps::FullDeduceInputs in =
        gar_apps::LoadFullDeduceInputs(args.full, log_tag);
    const gar::graph::VertexAttrs* attrs =
        in.attrs.columns.empty() ? nullptr : &in.attrs;

    namespace deduction = gar::engine::deduction;
    std::unique_ptr<deduction::DeductionEngine> engine;
    if (cpu) {
      engine = std::make_unique<deduction::CpuChaseEngine>(in.graph, attrs);
    } else {
      engine = std::make_unique<deduction::GpuDeduceEngine>(in.graph, attrs);
    }
    const auto res = engine->Run(in.rules, args.full.cfg);
    gar_apps::ReportFullDeduce(args.full, in, res, log_tag, summary_title);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
