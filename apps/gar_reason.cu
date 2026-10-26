// gar_reason: unified entry point for the reasoning engine family.
//
// --mode full  runs the whole rule set to a (capped) fixpoint, on the CPU
//     chase (engine::deduction::CpuChaseEngine, oneTBB) or the GPU engine
//     (engine::deduction::GpuDeduceEngine, MatrixGraph task::GARDeduce);
//     both share the semantics and the output layout of the legacy
//     deduce / deduce_gpu apps.
// --mode agent runs policy-guided deduction (engine::agent): a single
//     greedy chain toward a goal fact under a budget, with pluggable
//     policy / scorer / executor.
//
// Usage:
//   gar_reason --mode full --engine cpu|gpu --csr <immutable_csr_dir>
//              --rules <rules_root> --out <out_dir>
//              [--max-rounds 100] [--max-derivations 10000000]
//              [--max-matches 1000000] [--no-matches] [--print 20]
//              [--threads 0]
//
//   gar_reason --mode agent --csr <dir> --rules <root>
//              --goal 'rel_name(src,dst)' --budget B
//              [--policy deterministic|random|hybrid|jev] [--scorer exact|hdc]
//              [--hdc-dim N] [--executor greedy] [--seed 42]
//              [--max-candidates-per-rule 256]
//              [--alpha A] [--beta B] [--gamma G] [--nu N] [--nu2 N2]
//              [--tau-hdc T] [--no-confidence]
//              [--out <dir>] [--verify-against-closure] [--print 20]
//
// Agent-mode goal arguments are entity names; '_' leaves an argument open.
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "agent_mode.h"
#include "full_deduce_cli.h"
#include "gar/engine/deduction/cpu_chase_engine.h"
#include "gar/engine/deduction/gpu_deduce_engine.h"

namespace {

struct Args {
  std::string mode;    // "full" | "agent"
  std::string engine;  // full mode: "cpu" | "gpu"
  gar_apps::FullDeduceArgs full;
  gar_apps::AgentModeArgs agent;
};

void PrintUsage() {
  std::cerr
      << "usage:\n"
         "  gar_reason --mode full --engine cpu|gpu --csr <csr_dir> "
         "--rules <rules_root> --out <out_dir> [--max-rounds N] "
         "[--max-derivations N] [--max-matches N] [--no-matches] "
         "[--print N] [--threads N]\n"
         "  gar_reason --mode agent --csr <csr_dir> --rules <rules_root> "
         "--goal 'rel_name(src,dst)' --budget B [--policy "
         "deterministic|random|hybrid|jev] [--scorer exact|hdc] [--hdc-dim N] "
         "[--executor greedy|bnb] [--top-k N] [--seed N] [--max-candidates-per-rule N] "
         "[--alpha F] [--beta F] [--gamma F] [--nu F] [--nu2 F] "
         "[--tau-hdc F] [--no-confidence] [--out <dir>] "
         "[--verify-against-closure] [--print N]\n"
         "    jev: [--jev-endpoint URL] [--jev-timeout-ms N] [--jev-top-k N] "
         "[--jev-sample] [--jev-mock]\n";
}

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
    else if (gar_apps::ParseFullDeduceOption(k, next, &a.full,
                                             /*allow_threads=*/true)) {
    } else if (!gar_apps::ParseAgentModeOption(k, next, &a.agent)) {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.mode == "full") {
    if ((a.engine != "cpu" && a.engine != "gpu") || a.full.csr_dir.empty() ||
        a.full.rules_root.empty()) {
      PrintUsage();
      std::exit(2);
    }
  } else if (a.mode == "agent") {
    if (a.full.csr_dir.empty() || a.full.rules_root.empty() ||
        a.agent.goal_spec.empty() || a.agent.budget == 0 ||
        (a.agent.policy != "deterministic" && a.agent.policy != "random" &&
         a.agent.policy != "hybrid" && a.agent.policy != "jev") ||
        (a.agent.scorer != "exact" && a.agent.scorer != "hdc") ||
        (a.agent.executor != "greedy" && a.agent.executor != "bnb" &&
         a.agent.executor != "backward")) {
      PrintUsage();
      std::exit(2);
    }
  } else {
    PrintUsage();
    std::exit(2);
  }
  return a;
}

int RunFullMode(const Args& args) {
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
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    if (args.mode == "agent") {
      return gar_apps::RunAgentMode(args.full, args.agent);
    }
    return RunFullMode(args);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
