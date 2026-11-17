#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agent_mode.h"
#include "closure_output.h"

namespace gar_apps {
namespace agent_batch {

struct Task {
  std::string id;
  std::string spec;
};

struct TaskResult {
  std::string id;
  std::string spec;
  bool goal_derived = false;
  bool sound = true;
  std::uint64_t derived = 0;
  std::uint64_t evaluate_events = 0;
  double wall_seconds = 0.0;
  std::string termination;
  std::string error;
};

inline std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t tab = line.find('\t', begin);
    fields.push_back(line.substr(begin, tab == std::string::npos
                                           ? std::string::npos
                                           : tab - begin));
    if (tab == std::string::npos) return fields;
    begin = tab + 1;
  }
}

inline std::vector<Task> LoadTasks(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open tasks: " + path);
  std::vector<Task> tasks;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.starts_with('#') || line.starts_with("goal_id")) {
      continue;
    }
    const auto fields = SplitTabs(line);
    Task task;
    if (fields.size() == 1) {
      task.id = "g" + std::to_string(tasks.size());
      task.spec = fields[0];
    } else {
      task.id = fields[0];
      const auto spec = std::find_if(fields.begin() + 1, fields.end(),
                                     [](const std::string& field) {
        return field.find('(') != std::string::npos;
      });
      if (spec == fields.end()) continue;
      task.spec = *spec;
    }
    tasks.push_back(std::move(task));
  }
  return tasks;
}

inline std::unordered_set<std::string> LoadGroundTruth(
    const std::filesystem::path& directory) {
  std::unordered_set<std::string> facts;
  if (directory.empty()) return facts;
  std::ifstream input(directory / "closure_edges.tsv");
  if (!input) {
    throw std::runtime_error("cannot open ground-truth closure: " +
                             directory.string());
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.starts_with('#')) continue;
    const auto fields = SplitTabs(line);
    if (fields.size() < 3) continue;
    facts.insert(fields[1] + "\t" + fields[0] + "\t" + fields[2]);
  }
  return facts;
}

inline std::string FactKey(
    const gar::core::graph::Graph& graph,
    const gar::core::graph::DerivedEdge& edge) {
  return graph.SlotName(edge.slot) + "\t" + graph.EntityName(edge.src) + "\t" +
         graph.EntityName(edge.dst);
}

inline void WriteTaskOutputs(
    const Task& task, const AgentModeArgs& args,
    const gar::core::graph::Graph& graph,
    const std::vector<std::string>& rule_ids,
    const gar::core::engine::agent::AgentResult& result,
    const TaskResult& summary, const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  WriteClosureEdges(directory.string(), graph, rule_ids, result.derived);
  std::ofstream output(directory / "summary.yaml");
  output << "goal_id: \"" << task.id << "\"\n"
         << "goal: \"" << task.spec << "\"\n"
         << "engine: " << args.engine << "\n"
         << "policy: " << args.policy << "\n"
         << "scorer: " << args.scorer << "\n"
         << "goal_derived: " << (result.goal_derived ? "true" : "false")
         << "\ntermination: " << result.termination << "\n"
         << "derived: " << result.derived.size() << "\n"
         << "wall_seconds: " << summary.wall_seconds << "\n"
         << "soundness: " << (summary.sound ? "pass" : "fail") << "\n";
  if (result.stats.has_value()) {
    output << "evaluate_events: " << result.stats->evaluate_events << "\n"
           << "select_events: " << result.stats->select_events << "\n"
           << "candidate_caps: " << result.stats->candidate_caps << "\n";
  }
}

inline TaskResult RunOne(
    const Task& task, const AgentModeArgs& args, const FullDeduceInputs& inputs,
    const gar::core::analysis::DependencyGraph& dependency_graph,
    const std::unordered_set<std::string>& ground_truth,
    const std::filesystem::path& output_root) {
  namespace agent = gar::core::engine::agent;
  TaskResult out;
  out.id = task.id;
  out.spec = task.spec;
  try {
    gar::core::graph::Graph graph = inputs.graph;
    const agent::Goal goal = ParseGoalSpec(task.spec, graph);
    AgentModeArgs task_args = args;
    task_args.goal_spec = task.spec;
    agent::ReasoningAgent reasoning = MakeReasoningAgent(
        task_args, &dependency_graph, &goal, &graph, &inputs.rules,
        &inputs.rule_ids);
    agent::AgentState state(graph, inputs.attrs.columns.empty()
                                       ? nullptr
                                       : &inputs.attrs,
                            inputs.rules);
    agent::ExecConfig config;
    config.budget = args.budget;
    config.engine = args.engine;
    config.match_cap_per_rule = args.max_candidates_per_rule;
    config.utility_gamma = args.score.gamma;
    config.wall_limit_sec = args.wall_limit_sec;
    config.fair_interval = args.fair_interval;
    config.star_order = args.star_order;
    config.guide_strength = args.llm_guide ? args.guide_strength : 0.0;
    config.seed = args.PolicySeed();
    const auto started = std::chrono::steady_clock::now();
    const agent::AgentResult result = reasoning.Run(&state, goal, config);
    out.wall_seconds = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    out.goal_derived = result.goal_derived;
    out.termination = result.termination;
    out.derived = result.derived.size();
    out.evaluate_events = result.stats.has_value()
                              ? result.stats->evaluate_events
                              : 0;
    if (!ground_truth.empty()) {
      for (const auto& edge : result.derived) {
        if (!ground_truth.contains(FactKey(graph, edge))) {
          out.sound = false;
          break;
        }
      }
    }
    WriteTaskOutputs(task, task_args, graph, inputs.rule_ids, result, out,
                     output_root / task.id);
  } catch (const std::exception& error) {
    out.error = error.what();
  }
  return out;
}

inline int Run(const FullDeduceArgs& io, const AgentModeArgs& args) {
  const auto started = std::chrono::steady_clock::now();
  FullDeduceInputs inputs = LoadFullDeduceInputs(io, "gar_agent_batch");
  const std::vector<Task> tasks = LoadTasks(args.tasks_path);
  if (tasks.empty()) throw std::runtime_error("no goals in tasks file");

  gar::core::analysis::PredicateTable predicates;
  gar::core::analysis::DependencyGraph dependency_graph;
  dependency_graph.Build(inputs.rules, &predicates);
  (void)dependency_graph.StronglyConnectedComponents();
  const auto ground_truth = LoadGroundTruth(args.ground_truth_dir);

  std::vector<TaskResult> results(tasks.size());
  std::atomic<std::size_t> next{0};
  const std::size_t worker_count =
      std::min<std::size_t>(args.workers, tasks.size());
  auto worker = [&]() {
    while (true) {
      const std::size_t index = next.fetch_add(1);
      if (index >= tasks.size()) return;
      results[index] = RunOne(tasks[index], args, inputs, dependency_graph,
                              ground_truth, std::filesystem::path(io.out_root));
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) threads.emplace_back(worker);
  for (auto& thread : threads) thread.join();

  std::size_t hits = 0;
  std::size_t soundness_failures = 0;
  std::size_t errors = 0;
  std::uint64_t derived = 0;
  std::uint64_t evaluations = 0;
  for (const auto& result : results) {
    hits += result.goal_derived;
    soundness_failures += !result.sound;
    errors += !result.error.empty();
    derived += result.derived;
    evaluations += result.evaluate_events;
  }
  const double wall = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  std::filesystem::create_directories(io.out_root);
  std::ofstream report(std::filesystem::path(io.out_root) / "summary.yaml");
  report << "mode: agent-batch\n"
         << "engine: " << args.engine << "\n"
         << "policy: " << args.policy << "\n"
         << "scorer: " << args.scorer << "\n"
         << "tasks: " << tasks.size() << "\n"
         << "workers: " << worker_count << "\n"
         << "wall_seconds: " << wall << "\n"
         << "goal_hits: " << hits << "\n"
         << "soundness_failures: " << soundness_failures << "\n"
         << "errors: " << errors << "\n"
         << "derived: " << derived << "\n"
         << "evaluate_events: " << evaluations << "\n"
         << "results:\n";
  for (const auto& result : results) {
    report << "  - {id: \"" << result.id << "\", goal_derived: "
           << (result.goal_derived ? "true" : "false") << ", wall_seconds: "
           << result.wall_seconds << ", derived: " << result.derived
           << ", termination: \"" << result.termination << "\"";
    if (!result.error.empty()) report << ", error: \"" << result.error << "\"";
    report << "}\n";
  }
  std::cout << "agent-batch: tasks=" << tasks.size()
            << " workers=" << worker_count << " wall=" << wall
            << "s hits=" << hits << " soundness_failures="
            << soundness_failures << " errors=" << errors << "\n";
  return errors == 0 && soundness_failures == 0 ? 0 : 1;
}

}  // namespace agent_batch
}  // namespace gar_apps
