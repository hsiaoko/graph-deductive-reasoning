// gar_reason: unified entry point for the reasoning engine family.
//
// --mode chase  runs the whole rule set to a (capped) fixpoint, on the CPU
//     chase (gar::core::engine::chase::CpuChaseEngine, oneTBB) or the GPU engine
//     (gar::core::engine::chase::GpuChaseEngine, MatrixGraph task::GARDeduce);
//     both share the semantics and the output layout of the legacy
//     deduce / deduce_gpu apps.
// --mode dep-only runs the CPU goal-cone Chase in fixed input order, with one
//     thread and goal early stop.
// --mode agent runs policy-guided deduction (gar::core::engine::agent): a single
//     greedy chain toward a goal fact under a budget, with pluggable
//     policy / scorer / executor.
//
// Usage:
//   gar_reason --mode chase --engine cpu|gpu --csr <immutable_csr_dir>
//              --rules <rules_root> --out <out_dir>
//              [--max-rounds 100] [--max-derivations 10000000]
//              [--max-matches 1000000] [--no-matches] [--print 20]
//              [--threads 0]
//
//   gar_reason --mode agent --csr <dir> --rules <root>
//              --goal 'rel_name(src,dst)' --budget B
//              [--policy deterministic|random|hybrid|jev|learned] [--scorer exact|hdc|null]
//              [--hdc-dim N] [--seed 42]
//              [--max-candidates-per-rule 256]
//              [--alpha A] [--beta B] [--lambda L] [--gamma G] [--nu2 N2]
//              [--use-confidence]
//              [--out <dir>] [--verify-against-closure] [--print 20]
//
// Agent-mode goal arguments are entity names; '_' leaves an argument open.
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "agent_batch.h"
#include "agent_mode.h"
#include "full_deduce_cli.h"
#include "gar/core/chase/cpu_chase_engine.h"
#include "gar/core/chase/gpu_chase_engine.h"

namespace {

struct Args {
  std::string mode;    // "chase" | "agent"
  std::string engine;  // chase mode: "cpu" | "gpu"
  gar_apps::FullDeduceArgs full;
  gar_apps::AgentModeArgs agent;
};

void PrintUsage() {
  std::cerr
      << "usage:\n"
         "  gar_reason --mode chase --engine cpu|gpu --csr <csr_dir> "
         "--rules <rules_root> --out <out_dir> [--max-rounds N] "
         "[--max-derivations N] [--max-matches N] [--no-matches] "
         "[--print N] [--threads N]\n"
         "  gar_reason --mode dep-only --csr <csr_dir> --rules <rules_root> "
         "--goal 'rel_name(src,dst)' --out <out_dir>\n"
         "  gar_reason --mode agent [--engine cpu|gpu] --csr <csr_dir> "
         "--rules <rules_root> --goal 'rel_name(src,dst)' --budget B [--policy "
         "deterministic|random|hybrid|jev|learned|adaptive] [--scorer exact|hdc|null] [--hdc-dim N] "
         "[--seed N] [--policy-seed N] [--hdc-seed N] "
         "[--max-candidates-per-rule N] [--value-model PATH] "
         "[--hdc-value-backend original|simd|bit-lut] "
         "[--wall-limit S] [--fair-interval N] [--batch-size K] "
         "[--star-order input|goal] [--q-trace PATH] "
         "[--alpha F] [--beta F] [--lambda F] [--gamma F] [--nu2 F] "
         "[--use-confidence] [--out <dir>] "
         "[--verify-against-closure] [--print N]\n"
         "  gar_reason --mode agent-batch [--engine cpu|gpu] --csr <csr_dir> "
         "--rules <rules_root> --tasks goals.tsv --out <dir> "
         "[--workers N] [--gt closure_dir]\n"
         "    jev: [--jev-endpoint URL] [--jev-timeout-ms N] [--jev-top-k N] "
         "[--jev-sample] [--jev-mock]\n"
         "    initial guide: [--llm-guide] [--llm-mock] [--guide-strength F] "
         "[--guide-max-options N] [--guide-min-cost F] "
         "[--llm-backend openai|jev] [--llm-base-url URL] [--llm-model NAME] "
         "[--llm-timeout-ms N]\n";
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
    else if (k == "--engine") {
      a.engine = next();
      a.agent.engine = gar_apps::ParseMatchEngine(a.engine);
    }
    else if (k == "--goal") {
      // Routed by mode (--mode precedes --goal in every invocation): chase
      // mode uses it as a goal-cone filter, agent mode as the goal fact.
      if (a.mode == "agent") a.agent.goal_spec = next();
      else a.full.goal = next();
    }
    else if (gar_apps::ParseFullDeduceOption(k, next, &a.full,
                                             /*allow_threads=*/true)) {
    } else if (!gar_apps::ParseAgentModeOption(k, next, &a.agent)) {
      std::cerr << "unknown option: " << k << "\n";
      std::exit(2);
    }
  }
  if (a.mode == "chase") {
    if ((a.engine != "cpu" && a.engine != "gpu") || a.full.csr_dir.empty() ||
        a.full.rules_root.empty()) {
      PrintUsage();
      std::exit(2);
    }
  } else if (a.mode == "dep-only") {
    if (a.engine.empty()) a.engine = "cpu";
    a.full.variant = "dep-only";
    a.full.goal_early_stop = true;
    a.full.cfg.threads = 1;
    if (a.engine != "cpu" || a.full.csr_dir.empty() ||
        a.full.rules_root.empty() || a.full.goal.empty()) {
      PrintUsage();
      std::exit(2);
    }
  } else if (a.mode == "agent") {
    if (a.full.csr_dir.empty() || a.full.rules_root.empty() ||
        a.agent.goal_spec.empty() || a.agent.budget == 0) {
      PrintUsage();
      std::exit(2);
    }
  } else if (a.mode == "agent-batch") {
    if (a.full.csr_dir.empty() || a.full.rules_root.empty() ||
        a.agent.tasks_path.empty() || a.agent.workers == 0) {
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

  // GoalChase: restrict the rule set to the goal's dependency cone, then
  // run the plain chase to fixpoint or the explicitly requested goal stop.
  if (!args.full.goal.empty()) {
    const std::string goal_pred =
        args.full.goal.substr(0, args.full.goal.find('('));
    const auto kept = gar::core::analysis::GoalConeRuleIndices(in.rules, goal_pred);
    std::vector<gar::GARule> filtered_rules;
    std::vector<std::string> filtered_ids;
    filtered_rules.reserve(kept.size());
    filtered_ids.reserve(kept.size());
    for (std::size_t i : kept) {
      filtered_rules.push_back(in.rules[i]);
      filtered_ids.push_back(in.rule_ids[i]);
    }
    std::cerr << "[" << log_tag << "] goal-cone filter (" << goal_pred
              << "): " << filtered_rules.size() << " / " << in.rules.size()
              << " rules\n";
    in.rules = std::move(filtered_rules);
    in.rule_ids = std::move(filtered_ids);
  }
  auto chase_cfg = args.full.cfg;
  if (args.full.goal_early_stop) {
    if (!cpu || args.full.goal.empty()) {
      throw std::invalid_argument(
          "--goal-early-stop requires CPU chase and --goal");
    }
    const auto goal = gar_apps::ParseGoalSpec(args.full.goal, in.graph);
    if (!goal.args[0] || !goal.args[1]) {
      throw std::invalid_argument("--goal-early-stop requires a bound goal");
    }
    chase_cfg.stop_edge = std::array<std::uint32_t, 3>{
        goal.slot, *goal.args[0], *goal.args[1]};
  }

  const gar::core::graph::VertexAttributes* attrs =
      in.attrs.columns.empty() ? nullptr : &in.attrs;

  namespace deduction = gar::core::engine::chase;
  std::unique_ptr<deduction::ChaseEngine> engine;
  if (cpu) {
    engine = std::make_unique<deduction::CpuChaseEngine>(
        in.graph, attrs, &in.ml_models);
  } else {
    engine = std::make_unique<deduction::GpuChaseEngine>(in.graph, attrs);
  }
  const auto res = engine->Run(in.rules, chase_cfg);
  gar_apps::ReportFullDeduce(args.full, in, res, log_tag, summary_title);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = ParseArgs(argc, argv);
    if (args.mode == "agent") {
      return gar_apps::RunAgentMode(args.full, args.agent);
    }
    if (args.mode == "agent-batch") {
      return gar_apps::agent_batch::Run(args.full, args.agent);
    }
    return RunFullMode(args);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
