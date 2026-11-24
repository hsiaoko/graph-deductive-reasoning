// Diagnostic probe: does HDC action-goal similarity carry instance-level
// proof signal beyond the handcrafted score? Computes cosine(v_mu, v_g) for
// every trace candidate, labels proof membership (consequent on the proof DAG),
// and reports AUC for HDC cosine vs handcrafted f, overall and per goal round.
// Throwaway instrument for the learned-policy design decision; not a product
// surface. Reuses the real FactEncoder so the encoding matches training exactly.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gar/core/agent/fact_encoder.h"
#include "gar/core/agent/learning/policy_trace.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

namespace {
namespace agent = gar::core::engine::agent;
namespace learning = agent::learning;
using gar::core::graph::Graph;

struct Sample {
  double cosine;
  double score;
  int label;
  int round;
};

double Auc(std::vector<double> pos, std::vector<double> neg) {
  if (pos.empty() || neg.empty()) return 0.0;
  std::sort(neg.begin(), neg.end());
  double wins = 0.0;
  for (double s : pos) {
    auto lo = std::lower_bound(neg.begin(), neg.end(), s);
    auto hi = std::upper_bound(neg.begin(), neg.end(), s);
    wins += (lo - neg.begin()) + 0.5 * (hi - lo);
  }
  return wins / (static_cast<double>(pos.size()) * neg.size());
}

void Report(const std::string& name, const std::vector<Sample>& samples) {
  std::vector<double> cp, cn, fp, fn;
  for (const auto& s : samples) {
    if (s.label == 1) {
      cp.push_back(s.cosine);
      fp.push_back(s.score);
    } else {
      cn.push_back(s.cosine);
      fn.push_back(s.score);
    }
  }
  std::cout << name << " n=" << samples.size() << " pos=" << cp.size()
            << " neg=" << cn.size() << " HDC_cosine_AUC=" << Auc(cp, cn)
            << " f_AUC=" << Auc(fp, fn) << "\n";
}

std::map<std::string, int> LoadRounds(const std::filesystem::path& path) {
  std::map<std::string, int> rounds;
  if (path.empty()) return rounds;
  std::ifstream in(path);
  std::string line;
  bool first = true;
  int id_col = -1, round_col = -1;
  auto fields = [](const std::string& v) {
    std::vector<std::string> out;
    std::size_t b = 0;
    while (true) {
      std::size_t t = v.find('\t', b);
      out.push_back(v.substr(b, t - b));
      if (t == std::string::npos) return out;
      b = t + 1;
    }
  };
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto cols = fields(line);
    if (first) {
      for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "goal_id") id_col = i;
        if (cols[i] == "round") round_col = i;
      }
      first = false;
      continue;
    }
    if (id_col >= 0 && round_col >= 0 &&
        id_col < (int)cols.size() && round_col < (int)cols.size()) {
      rounds[cols[id_col]] = std::stoi(cols[round_col]);
    }
  }
  return rounds;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr, rules, traces, goals_tsv;
    std::uint32_t dim = 128;
    std::uint64_t seed = 42;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--rules") rules = next();
      else if (k == "--traces") traces = next();
      else if (k == "--goals-tsv") goals_tsv = next();
      else if (k == "--hdc-dim") dim = std::stoul(next());
      else if (k == "--hdc-seed") seed = std::stoull(next());
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty() || rules.empty() || traces.empty())
      throw std::invalid_argument(
          "usage: gar_hdc_probe --csr DIR --rules DIR --traces DIR "
          "[--goals-tsv FILE] [--hdc-dim N] [--hdc-seed N]");

    Graph graph = gar::core::graph::LoadGraph(csr, true);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(rules))
      if (e.is_directory() &&
          std::filesystem::exists(e.path() / "rule.yaml"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<gar::GARule> rule_vec;
    for (const auto& d : dirs) rule_vec.push_back(gar::RuleLoader::Load(d));
    gar::core::analysis::PredicateTable predicates;
    gar::core::analysis::DependencyGraph dg;
    dg.Build(rule_vec, &predicates);
    agent::FactEncoder encoder(
        gar::core::hdc::HdcEncoder({dim, seed}), &graph, &predicates);
    auto rounds = LoadRounds(goals_tsv);

    std::vector<Sample> all;
    std::map<int, std::vector<Sample>> by_round;
    std::vector<std::filesystem::path> paths;
    for (const auto& e :
         std::filesystem::recursive_directory_iterator(traces))
      if (e.is_regular_file() &&
          e.path().filename() == "policy_trace.json")
        paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());

    for (const auto& p : paths) {
      learning::PolicyTrace trace = learning::ReadPolicyTrace(p);
      if (!trace.goal_derived) continue;
      std::string goal_id = p.parent_path().parent_path().filename().string();
      int round = rounds.count(goal_id) ? rounds[goal_id] : 0;
      auto vg = encoder.EncodeGoal(trace.goal);
      double vg_norm = vg.Norm();
      std::unordered_set<agent::FactKey, agent::FactKeyHash> proof;
      for (const auto& d : trace.derivations)
        proof.insert(KeyOf(d.consequent));
      std::unordered_set<agent::FactKey, agent::FactKeyHash> seen;
      for (const auto& c : trace.candidates) {
        auto key = KeyOf(c.consequent);
        if (!seen.insert(key).second) continue;
        auto vm = encoder.Encode(c.consequent);
        double cos = vm.Dot(vg) / (vm.Norm() * vg_norm);
        Sample s{cos, c.handcrafted_score, proof.count(key) ? 1 : 0, round};
        all.push_back(s);
        by_round[round].push_back(s);
      }
    }
    Report("overall", all);
    for (const auto& [round, samples] : by_round)
      Report("round" + std::to_string(round), samples);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_hdc_probe: " << e.what() << '\n';
    return 2;
  }
}