#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gar/core/agent/fact_encoder.h"
#include "gar/core/agent/learning/proof_dataset.h"
#include "gar/core/agent/learning/relevance_trainer.h"
#include "gar/core/agent/motif.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

namespace {

namespace agent = gar::core::engine::agent;
namespace learning = agent::learning;
using gar::core::graph::Graph;

struct Args {
  std::filesystem::path csr;
  std::filesystem::path rules;
  std::filesystem::path traces;
  std::filesystem::path test_goals;
  std::filesystem::path validation_goals;
  std::filesystem::path out;
  learning::DatasetConfig dataset;
  learning::RelevanceTrainingConfig training;
  std::uint32_t hidden = 32;
  std::uint32_t hdc_dim = 0;
  std::string label = "proof";
  // Value (MC cost-to-go) route: read rollout decision traces instead of proof
  // traces. Each run dir holds decision_trace.tsv + value_meta.tsv.
  std::filesystem::path value_traces;
  double value_scale = 10000.0;
  std::uint64_t value_stride = 1;
  std::uint64_t value_max_decisions = 0;
  std::uint64_t hdc_seed = 42;
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        throw std::invalid_argument("missing value for " + key);
      return argv[++i];
    };
    if (key == "--csr")
      args.csr = next();
    else if (key == "--rules")
      args.rules = next();
    else if (key == "--traces")
      args.traces = next();
    else if (key == "--test-goals")
      args.test_goals = next();
    else if (key == "--validation-goals")
      args.validation_goals = next();
    else if (key == "--out")
      args.out = next();
    else if (key == "--epochs")
      args.training.epochs = std::stoul(next());
    else if (key == "--batch-size")
      args.training.batch_size = std::stoull(next());
    else if (key == "--learning-rate")
      args.training.learning_rate = std::stod(next());
    else if (key == "--l2")
      args.training.l2 = std::stod(next());
    else if (key == "--hidden")
      args.hidden = std::stoul(next());
    else if (key == "--hdc-dim")
      args.hdc_dim = std::stoul(next());
    else if (key == "--hdc-seed")
      args.hdc_seed = std::stoull(next());
    else if (key == "--label")
      args.label = next();
    else if (key == "--value-traces")
      args.value_traces = next();
    else if (key == "--value-scale")
      args.value_scale = std::stod(next());
    else if (key == "--value-stride")
      args.value_stride = std::stoull(next());
    else if (key == "--value-max-decisions")
      args.value_max_decisions = std::stoull(next());
    else if (key == "--ranking")
      args.training.ranking = true;
    else if (key == "--label-gamma")
      args.dataset.label_gamma = std::stod(next());
    else if (key == "--negative-ratio") {
      args.dataset.negatives_per_positive = std::stoull(next());
    } else if (key == "--seed") {
      args.training.seed = std::stoull(next());
      args.dataset.seed = args.training.seed;
    } else {
      throw std::invalid_argument("unknown option: " + key);
    }
  }
  if (args.csr.empty() || args.rules.empty() || args.test_goals.empty() ||
      args.out.empty()) {
    throw std::invalid_argument(
        "usage: gar_train_policy --csr DIR --rules DIR --traces DIR "
        "--test-goals FILE [--validation-goals FILE] --out MODEL.meta "
        "[--epochs N] [--batch-size N] [--learning-rate F] [--l2 F] "
        "[--hidden N] [--label proof|binary|value] [--label-gamma F] "
        "[--negative-ratio N] [--seed N]");
  }
  if (args.label != "proof" && args.label != "binary" && args.label != "value") {
    throw std::invalid_argument("--label must be 'proof', 'binary', or 'value'");
  }
  if (args.label == "value") {
    if (args.value_traces.empty())
      throw std::invalid_argument("--label value requires --value-traces DIR");
    if (!(args.value_scale > 0.0))
      throw std::invalid_argument("--value-scale must be positive");
    if (args.value_stride == 0) args.value_stride = 1;
  } else if (args.traces.empty()) {
    throw std::invalid_argument("--label proof|binary requires --traces DIR");
  }
  return args;
}

std::vector<gar::GARule> LoadRules(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> directories;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_directory() &&
        std::filesystem::exists(entry.path() / "rule.yaml")) {
      directories.push_back(entry.path());
    }
  }
  std::sort(directories.begin(), directories.end());
  std::vector<gar::GARule> rules;
  for (const auto& directory : directories) {
    rules.push_back(gar::RuleLoader::Load(directory));
  }
  return rules;
}

std::string CanonicalGoal(const agent::Goal& goal, const Graph& graph) {
  return graph.RelationName(goal.slot) + "(" +
         (goal.args[0] ? graph.EntityName(*goal.args[0]) : "_") + "," +
         (goal.args[1] ? graph.EntityName(*goal.args[1]) : "_") + ")";
}

std::string NormalizeSpec(std::string value) {
  value.erase(
      std::remove_if(value.begin(), value.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; }),
      value.end());
  return value;
}

std::unordered_set<std::string> LoadGoalSpecs(
    const std::filesystem::path& path) {
  if (path.empty()) return {};
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read goals: " + path.string());
  std::unordered_set<std::string> goals;
  std::string line;
  std::size_t spec_column = 0;
  bool first = true;
  auto fields = [](const std::string& value) {
    std::vector<std::string> columns;
    std::size_t begin = 0;
    while (true) {
      const std::size_t tab = value.find('\t', begin);
      columns.push_back(value.substr(begin, tab - begin));
      if (tab == std::string::npos) return columns;
      begin = tab + 1;
    }
  };
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto columns = fields(line);
    if (first) {
      const auto spec = std::find(columns.begin(), columns.end(), "spec");
      if (spec != columns.end()) {
        spec_column = static_cast<std::size_t>(spec - columns.begin());
        first = false;
        continue;
      }
      first = false;
    }
    if (spec_column < columns.size()) {
      goals.insert(NormalizeSpec(columns[spec_column]));
    }
  }
  return goals;
}

std::vector<std::filesystem::path> TracePaths(
    const std::filesystem::path& root) {
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::is_regular_file(root)) {
    paths.push_back(root);
  } else {
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() &&
          entry.path().filename() == "policy_trace.json") {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.empty())
    throw std::invalid_argument("no policy_trace.json files found");
  return paths;
}

std::vector<learning::VectorRelevanceExample> Vectorize(
    const std::vector<learning::RelevanceExample>& examples,
    const agent::FactEncoder& encoder) {
  std::unordered_map<agent::FactKey, gar::core::hdc::DenseVector,
                     agent::FactKeyHash>
      action_cache;
  std::unordered_map<std::string, gar::core::hdc::DenseVector> goal_cache;
  std::vector<learning::VectorRelevanceExample> vectors;
  vectors.reserve(examples.size());
  for (const auto& example : examples) {
    const agent::FactKey action_key = KeyOf(example.action_fact);
    auto [action, inserted] =
        action_cache.try_emplace(action_key, encoder.hdc().dim());
    if (inserted) action->second = encoder.Encode(example.action_fact);
    const std::string goal_key =
        std::to_string(example.goal.slot) + ":" +
        (example.goal.args[0] ? std::to_string(*example.goal.args[0]) : "_") +
        ":" +
        (example.goal.args[1] ? std::to_string(*example.goal.args[1]) : "_");
    auto [goal, goal_inserted] =
        goal_cache.try_emplace(goal_key, encoder.hdc().dim());
    if (goal_inserted) goal->second = encoder.EncodeGoal(example.goal);
    const double norm_prod = action->second.Norm() * goal->second.Norm();
    const double cosine =
        norm_prod > 0.0 ? action->second.Dot(goal->second) / norm_prod : 0.0;
    vectors.push_back(learning::VectorRelevanceExample{
        action->second, goal->second, example.handcrafted_score,
        example.target, std::hash<std::string>{}(goal_key), cosine});
  }
  return vectors;
}

struct ValidationMetrics {
  double mse = 0.0;
  double pairwise_accuracy = 0.0;
  double recall_at_1 = 0.0;
  double recall_at_5 = 0.0;
  double recall_at_10 = 0.0;
  std::uint64_t ranked_goals = 0;
};

ValidationMetrics Validate(
    const std::vector<learning::RelevanceExample>& examples,
    const std::vector<learning::VectorRelevanceExample>& vectors,
    const learning::RelevanceModel& model) {
  ValidationMetrics metrics;
  if (examples.empty()) return metrics;
  std::unordered_map<std::string, std::vector<std::size_t>> by_goal;
  for (std::size_t i = 0; i < examples.size(); ++i) {
    const auto& goal = examples[i].goal;
    const std::string key =
        std::to_string(goal.slot) + ":" +
        (goal.args[0] ? std::to_string(*goal.args[0]) : "_") + ":" +
        (goal.args[1] ? std::to_string(*goal.args[1]) : "_");
    by_goal[key].push_back(i);
    const double error = model.Predict({&vectors[i].action, &vectors[i].goal,
                                        vectors[i].handcrafted_score,
                                        vectors[i].cosine}) -
                         vectors[i].target;
    metrics.mse += error * error;
  }
  metrics.mse /= static_cast<double>(examples.size());
  for (const auto& [goal, indices] : by_goal) {
    (void)goal;
    std::vector<std::pair<double, double>> ranked;
    std::uint64_t positives = 0;
    std::uint64_t pairs = 0;
    std::uint64_t agreements = 0;
    for (std::size_t index : indices) {
      const double prediction =
          model.Predict({&vectors[index].action, &vectors[index].goal,
                         vectors[index].handcrafted_score,
                         vectors[index].cosine});
      ranked.emplace_back(prediction, vectors[index].target);
      positives += vectors[index].target > 0.0;
    }
    for (const auto& left : ranked) {
      if (!(left.second > 0.0)) continue;
      for (const auto& right : ranked) {
        if (right.second != 0.0) continue;
        ++pairs;
        agreements += left.first > right.first;
      }
    }
    if (pairs == 0 || positives == 0) continue;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& left, const auto& right) {
                       return left.first > right.first;
                     });
    ++metrics.ranked_goals;
    metrics.pairwise_accuracy += static_cast<double>(agreements) / pairs;
    for (std::size_t k : {1UL, 5UL, 10UL}) {
      std::uint64_t found = 0;
      for (std::size_t i = 0; i < std::min(k, ranked.size()); ++i) {
        found += ranked[i].second > 0.0;
      }
      const double recall = static_cast<double>(found) / positives;
      if (k == 1) metrics.recall_at_1 += recall;
      if (k == 5) metrics.recall_at_5 += recall;
      if (k == 10) metrics.recall_at_10 += recall;
    }
  }
  if (metrics.ranked_goals != 0) {
    const double scale = 1.0 / metrics.ranked_goals;
    metrics.pairwise_accuracy *= scale;
    metrics.recall_at_1 *= scale;
    metrics.recall_at_5 *= scale;
    metrics.recall_at_10 *= scale;
  }
  return metrics;
}

agent::Goal ParseGoalSpecLocal(const std::string& spec, const Graph& g) {
  const auto lp = spec.find('(');
  const auto rp = spec.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp)
    throw std::invalid_argument("bad goal spec: " + spec);
  const std::string rel = spec.substr(0, lp);
  const std::string args = spec.substr(lp + 1, rp - lp - 1);
  const auto comma = args.find(',');
  if (comma == std::string::npos)
    throw std::invalid_argument("bad goal spec (two args): " + spec);
  agent::Goal goal;
  bool found = false;
  for (std::uint32_t r = 0; r < g.num_relations(); ++r) {
    if (g.RelationName(r) == rel) { goal.slot = r; found = true; break; }
  }
  if (!found) throw std::invalid_argument("unknown relation: " + rel);
  auto resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "_") return std::nullopt;
    for (std::uint32_t v = 0; v < g.num_entities(); ++v)
      if (g.EntityName(v) == name) return v;
    throw std::invalid_argument("unknown entity: " + name);
  };
  goal.args = {resolve(args.substr(0, comma)), resolve(args.substr(comma + 1))};
  return goal;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (true) {
    const std::size_t tab = line.find('\t', begin);
    out.push_back(line.substr(begin, tab - begin));
    if (tab == std::string::npos) return out;
    begin = tab + 1;
  }
}

// MC cost-to-go value dataset (Design 2). Each rollout run dir holds a
// decision_trace.tsv (chosen action per decision, with the 11 ActionFeatures)
// and a value_meta.tsv (goal_spec, total_derived). The label is the cheapness
// exp(-remaining/scale), remaining = total_derived - decision_index, so actions
// taken in cheap rollouts get high targets throughout while expensive rollouts
// get low ones -- the cross-rollout cost variation is the learnable signal.
// Only chosen actions carry an observed MC return; alternatives are skipped.
learning::RelevanceDataset BuildValueDataset(
    const std::filesystem::path& root, const std::vector<gar::GARule>& rules,
    const Graph& graph, double scale, std::uint64_t stride,
    std::uint64_t max_decisions, const std::unordered_set<std::string>& test_goals,
    const std::unordered_set<std::string>& validation_goals,
    std::vector<std::string>& trained_on,
    std::vector<learning::RelevanceExample>& validation_examples) {
  learning::RelevanceDataset dataset;
  std::vector<std::filesystem::path> runs;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_directory() &&
        std::filesystem::exists(entry.path() / "decision_trace.tsv") &&
        std::filesystem::exists(entry.path() / "value_meta.tsv"))
      runs.push_back(entry.path());
  }
  std::sort(runs.begin(), runs.end());
  if (runs.empty())
    throw std::invalid_argument("no value runs (decision_trace.tsv + value_meta.tsv)");
  for (const auto& run : runs) {
    std::string goal_spec;
    std::uint64_t total = 0;
    std::ifstream meta(run / "value_meta.tsv");
    std::string line;
    while (std::getline(meta, line)) {
      const auto cols = SplitTabs(line);
      if (cols.size() < 2) continue;
      if (cols[0] == "goal_spec") goal_spec = cols[1];
      else if (cols[0] == "total_derived") total = std::stoull(cols[1]);
    }
    if (goal_spec.empty())
      throw std::invalid_argument("value_meta.tsv missing goal_spec: " + run.string());
    const agent::Goal goal = ParseGoalSpecLocal(goal_spec, graph);
    const std::string canonical = CanonicalGoal(goal, graph);
    if (test_goals.count(canonical) != 0)
      throw std::invalid_argument("test goal leaked into value traces: " + canonical);
    const bool validation = validation_goals.count(canonical) != 0;
    auto& sink = validation ? validation_examples : dataset.examples;
    if (!validation) trained_on.push_back(run.string());
    std::ifstream trace(run / "decision_trace.tsv");
    std::uint64_t taken = 0;
    while (std::getline(trace, line)) {
      if (line.empty()) continue;
      const auto cols = SplitTabs(line);
      if (cols.size() < 17) continue;
      if (cols[1] != "1") continue;  // chosen action only (observed MC return)
      const std::uint64_t decision_index = std::stoull(cols[0]);
      if (decision_index % stride != 0) continue;
      if (max_decisions != 0 && decision_index >= max_decisions) continue;
      const std::uint32_t rule = static_cast<std::uint32_t>(std::stoul(cols[4]));
      if (rule >= rules.size()) continue;
      std::vector<std::uint32_t> embedding;
      std::size_t begin = 0;
      const std::string& emb = cols[5];
      while (begin < emb.size()) {
        const std::size_t comma = emb.find(',', begin);
        embedding.push_back(static_cast<std::uint32_t>(
            std::stoul(emb.substr(begin, comma - begin))));
        if (comma == std::string::npos) break;
        begin = comma + 1;
      }
      const agent::RuleHead head = agent::ResolveRuleHead(rules[rule]);
      if (!head.valid || head.pu >= embedding.size() || head.pv >= embedding.size())
        continue;
      const agent::Fact consequent =
          agent::MakeEdgeFact(head.slot, embedding[head.pu], embedding[head.pv]);
      const double f = std::stod(cols[13]);
      const double remaining =
          total > decision_index ? static_cast<double>(total - decision_index) : 0.0;
      const double target = std::exp(-remaining / scale);
      sink.push_back(learning::RelevanceExample{consequent, goal, f, target, decision_index});
      ++taken;
    }
    if (!validation) dataset.stats.successful_traces += taken > 0 ? 1 : 0;
  }
  dataset.stats.positive_candidates = dataset.examples.size();
  return dataset;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = ParseArgs(argc, argv);
    Graph graph = gar::core::graph::LoadGraph(args.csr, true);
    const auto rules = LoadRules(args.rules);
    gar::core::analysis::PredicateTable predicates;
    gar::core::analysis::DependencyGraph dependencies;
    dependencies.Build(rules, &predicates);
    const auto test_goals = LoadGoalSpecs(args.test_goals);
    const auto validation_goals = LoadGoalSpecs(args.validation_goals);
    const std::uint64_t fingerprint = agent::GraphFingerprint(graph);

    std::vector<std::string> trained_on;
    learning::RelevanceDataset train;
    learning::RelevanceDataset validation;
    std::uint32_t effective_dim = args.hdc_dim;
    std::uint64_t hdc_seed = args.hdc_seed;
    if (args.label == "value") {
      if (effective_dim == 0)
        throw std::invalid_argument("--label value requires --hdc-dim");
      train = BuildValueDataset(args.value_traces, rules, graph, args.value_scale,
                                args.value_stride, args.value_max_decisions,
                                test_goals, validation_goals, trained_on,
                                validation.examples);
    } else {
      std::vector<learning::PolicyTrace> train_traces;
      std::vector<learning::PolicyTrace> validation_traces;
      for (const auto& path : TracePaths(args.traces)) {
        learning::PolicyTrace trace = learning::ReadPolicyTrace(path);
        if (trace.graph_fingerprint != fingerprint) {
          throw std::invalid_argument("trace graph fingerprint mismatch: " +
                                      path.string());
        }
        const std::string goal = CanonicalGoal(trace.goal, graph);
        if (test_goals.count(goal) != 0) {
          throw std::invalid_argument("test goal leaked into training traces: " +
                                      goal);
        }
        const bool validation = validation_goals.count(goal) != 0;
        if (validation) {
          validation_traces.push_back(std::move(trace));
        } else {
          trained_on.push_back(path.string());
          train_traces.push_back(std::move(trace));
        }
      }
      if (train_traces.empty())
        throw std::invalid_argument("no training traces remain");
      const auto build_dataset =
          args.label == "binary" ? learning::BuildBinaryProofDataset
                                 : learning::BuildRelevanceDataset;
      train = build_dataset(train_traces, args.dataset);
      validation = build_dataset(validation_traces, args.dataset);
      if (effective_dim == 0) effective_dim = train_traces.front().hdc.dim;
      hdc_seed = train_traces.front().hdc.seed;
    }
    if (train.examples.empty())
      throw std::invalid_argument("no training examples");
    agent::FactEncoder encoder(
        gar::core::hdc::HdcEncoder({effective_dim, hdc_seed}),
        &graph, &predicates);
    auto train_vectors = Vectorize(train.examples, encoder);
    auto validation_vectors = Vectorize(validation.examples, encoder);

    learning::RelevanceModelMetadata metadata;
    metadata.dim = effective_dim;
    metadata.hidden = args.hidden;
    metadata.hdc_seed = hdc_seed;
    metadata.graph_fingerprint = fingerprint;
    metadata.label_gamma = args.dataset.label_gamma;
    metadata.negatives_per_positive = args.dataset.negatives_per_positive;
    metadata.seed = args.training.seed;
    metadata.trained_on = trained_on;
    learning::RelevanceModel model =
        learning::RelevanceModel::Initialize(metadata);
    const learning::RelevanceTrainer trainer(args.training);
    const auto report = trainer.Train(train_vectors, &model);
    model.Save(args.out);
    const ValidationMetrics metrics =
        Validate(validation.examples, validation_vectors, model);
    std::filesystem::path report_path = args.out;
    report_path.replace_extension(".report.json");
    std::ofstream out(report_path);
    out << "{\n  \"label\": \"" << args.label
        << "\",\n  \"ranking\": " << (args.training.ranking ? "true" : "false")
        << ",\n  \"train_examples\": " << train.examples.size()
        << ",\n  \"positive_examples\": " << train.stats.positive_candidates
        << ",\n  \"negative_examples\": " << train.stats.negative_candidates
        << ",\n  \"pairs\": " << report.pairs
        << ",\n  \"validation_examples\": " << validation.examples.size()
        << ",\n  \"epoch_mse\": [";
    for (std::size_t i = 0; i < report.epoch_mse.size(); ++i) {
      if (i != 0) out << ", ";
      out << report.epoch_mse[i];
    }
    out << "],\n  \"epoch_pairwise_accuracy\": [";
    for (std::size_t i = 0; i < report.epoch_pairwise_accuracy.size(); ++i) {
      if (i != 0) out << ", ";
      out << report.epoch_pairwise_accuracy[i];
    }
    out << "],\n  \"validation\": {\"mse\": " << metrics.mse
        << ", \"pairwise_accuracy\": " << metrics.pairwise_accuracy
        << ", \"recall_at_1\": " << metrics.recall_at_1
        << ", \"recall_at_5\": " << metrics.recall_at_5
        << ", \"recall_at_10\": " << metrics.recall_at_10
        << ", \"ranked_goals\": " << metrics.ranked_goals << "}\n}\n";
    std::cout << "trained " << train.examples.size()
              << " examples; model=" << args.out
              << " validation_mse=" << metrics.mse
              << " pairwise=" << metrics.pairwise_accuracy << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "gar_train_policy: " << error.what() << '\n';
    return 2;
  }
}
