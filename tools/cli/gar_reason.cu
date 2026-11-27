// gar_reason: unified entry point for the reasoning engine family.
//
// --mode chase  runs the whole rule set to a (capped) fixpoint, on the CPU
//     chase (gar::core::engine::chase::CpuChaseEngine, oneTBB) or the GPU engine
//     (gar::core::engine::chase::GpuChaseEngine, MatrixGraph task::GARDeduce);
//     both share the semantics and the output layout of the legacy
//     deduce / deduce_gpu apps.
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
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent_batch.h"
#include "agent_mode.h"
#include "full_deduce_cli.h"
#include "gar/core/chase/cpu_chase_engine.h"
#include "gar/core/chase/gpu_chase_engine.h"

namespace {

struct TargetAttrArg {
  std::uint32_t position;
  std::string attr;
  gar::AttrValue value;
};

struct Args {
  std::string mode;    // "chase" | "agent"
  std::string engine;  // chase mode: "cpu" | "gpu"
  gar_apps::FullDeduceArgs full;
  gar_apps::AgentModeArgs agent;
  std::vector<TargetAttrArg> target_attrs;
};

TargetAttrArg ParseTargetAttr(const std::string& spec) {
  const auto first = spec.find(':');
  const auto second = first == std::string::npos
                          ? std::string::npos
                          : spec.find(':', first + 1);
  if (first == std::string::npos || second == std::string::npos ||
      second + 1 == spec.size() || second == first + 1) {
    throw std::invalid_argument(
        "bad --target-attr (want POSITION:ATTR:VALUE): " + spec);
  }
  const std::string position = spec.substr(0, first);
  if (position != "0" && position != "1") {
    throw std::invalid_argument("--target-attr POSITION must be 0 or 1");
  }
  const YAML::Node value = YAML::Load(spec.substr(second + 1));
  if (!value.IsScalar()) {
    throw std::invalid_argument("--target-attr VALUE must be a scalar");
  }
  gar::AttrValue parsed;
  bool b = false;
  if (YAML::convert<bool>::decode(value, b)) {
    parsed = b;
  } else {
    try {
      parsed = value.as<std::int64_t>();
    } catch (const YAML::Exception&) {
      try {
        parsed = value.as<double>();
      } catch (const YAML::Exception&) {
        parsed = value.as<std::string>();
      }
    }
  }
  if (std::holds_alternative<std::string>(parsed)) {
    throw std::invalid_argument(
        "--target-attr VALUE must be numeric or boolean; string vertex "
        "attributes are not loaded");
  }
  return {static_cast<std::uint32_t>(position[0] - '0'),
          spec.substr(first + 1, second - first - 1), std::move(parsed)};
}

void PrintUsage() {
  std::cerr
      << "usage:\n"
         "  gar_reason --mode chase --engine cpu|gpu --csr <csr_dir> "
         "--rules <rules_root> --out <out_dir> [--max-rounds N] "
         "[--max-derivations N] [--max-matches N] [--no-matches] "
         "[--print N] [--threads N]\n"
         "  gar_reason --mode dep-only --csr <csr_dir> --rules <rules_root> "
         "--goal 'rel_name(src,dst)' --out <out_dir> [--enumerate-open] "
         "[--target-attr POSITION:ATTR:VALUE] [--max-rounds N] "
         "[--max-derivations N] [--max-matches N]\n"
         "  gar_reason --mode agent [--engine cpu|gpu] --csr <csr_dir> "
         "--rules <rules_root> --goal 'rel_name(src,dst)' --budget B "
         "[--enumerate-open] [--policy "
         "deterministic|random|uniform-random|hybrid|jev|learned|adaptive] [--scorer exact|hdc|null] [--hdc-dim N] "
         "[--seed N] [--policy-seed N] [--hdc-seed N] "
         "[--max-candidates-per-rule N] [--relevance-model PATH] "
         "[--wall-limit S] [--fair-interval N] [--batch-size K] "
         "[--star-order input|goal] [--policy-trace PATH] "
         "[--policy-trace-negatives N] "
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
    else if (k == "--target-attr")
      a.target_attrs.push_back(ParseTargetAttr(next()));
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
    a.full.goal_early_stop = false;
    a.full.cfg.threads = 1;
    if (!a.full.max_rounds_set) {
      a.full.cfg.max_rounds = std::numeric_limits<std::uint32_t>::max();
    }
    if (!a.full.max_derivations_set) {
      a.full.cfg.max_derivations = std::numeric_limits<std::uint64_t>::max();
    }
    if (!a.full.max_matches_set) {
      a.full.cfg.max_matches_per_rule =
          std::numeric_limits<std::uint64_t>::max();
    }
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
  if (!a.target_attrs.empty() && a.mode != "dep-only") {
    std::cerr << "--target-attr requires --mode dep-only\n";
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

  const bool dep_only = args.mode == "dep-only";
  std::optional<gar::core::engine::agent::Goal> dep_goal;
  bool dep_all_free = false;
  bool dep_recursive = false;
  if (dep_only) {
    dep_goal = gar_apps::ParseGoalSpec(args.full.goal, in.graph);
    dep_all_free = !dep_goal->args[0] && !dep_goal->args[1] &&
                   args.target_attrs.empty();
    if (!dep_all_free) {
      gar::core::analysis::PredicateTable table;
      gar::core::analysis::DependencyGraph dependencies;
      dependencies.Build(in.rules, &table);
      const auto target = table.Find(in.graph.RelationName(dep_goal->slot));
      dep_recursive =
          target != gar::core::analysis::kInvalidPredicate &&
          dependencies.RecursiveFamilyOf(target) >= 0;
    }
  }

  if (!args.full.goal.empty() && (!dep_only || !dep_all_free)) {
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
  } else if (dep_only) {
    std::cerr << "[" << log_tag << "] all-free target: using all "
              << in.rules.size() << " rules\n";
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
  if (dep_only && !dep_all_free) {
    if (dep_recursive) {
      std::cerr << "[" << log_tag
                << "] recursive target: target-side pruning disabled\n";
    } else {
      gar::core::engine::chase::TargetPattern target;
      target.slot = dep_goal->slot;
      target.args = dep_goal->args;
      for (const auto& arg : args.target_attrs) {
        gar::Predicate pred;
        pred.kind = gar::PredKind::kConst;
        pred.node = arg.position;
        pred.attr = arg.attr;
        pred.value = arg.value;
        target.attributes[arg.position].push_back(std::move(pred));
      }
      chase_cfg.target_pattern = std::move(target);
    }
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
  const auto run_t0 = std::chrono::steady_clock::now();
  const auto res = engine->Run(in.rules, chase_cfg);
  const double execution_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - run_t0)
                                  .count();
  gar_apps::ReportFullDeduce(args.full, in, res, log_tag, summary_title);
  if (args.agent.enumerate_open) {
    std::ofstream summary(args.full.out_root + "/summary.yaml", std::ios::app);
    summary << "enumerate_open: true\n"
            << "execution_cost_ms: " << execution_ms << "\n";
  }
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
