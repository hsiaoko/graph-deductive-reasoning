// Diagnostic probe: does multi-hop graph proximity between an action's entities
// and the goal's entities carry instance-level proof signal on deep goals?
// HDC cosine only captures 1-hop entity overlap (gar_hdc_probe); this measures
// shortest-path distance (multi-source BFS from the goal entities over the
// undirected base adjacency) as the candidate feature. Reports proof-membership
// AUC of -distance vs handcrafted f, overall and per goal round. Throwaway
// instrument for the learned-policy design decision.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
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
  double neg_dist;
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
  std::vector<double> dp, dn, fp, fn;
  for (const auto& s : samples) {
    if (s.label == 1) {
      dp.push_back(s.neg_dist);
      fp.push_back(s.score);
    } else {
      dn.push_back(s.neg_dist);
      fn.push_back(s.score);
    }
  }
  std::cout << name << " n=" << samples.size() << " pos=" << dp.size()
            << " neg=" << dn.size() << " graph_dist_AUC=" << Auc(dp, dn)
            << " f_AUC=" << Auc(fp, fn) << "\n";
}

std::vector<int> Bfs(const Graph& g, const std::vector<std::uint32_t>& sources) {
  const std::uint32_t n = g.num_entities();
  std::vector<int> dist(n, -1);
  std::queue<std::uint32_t> q;
  for (auto s : sources) {
    if (s < n && dist[s] < 0) {
      dist[s] = 0;
      q.push(s);
    }
  }
  const std::uint32_t* dsts = g.OutEdgeDsts();
  while (!q.empty()) {
    std::uint32_t v = q.front();
    q.pop();
    const auto [b, e] = g.OutEdgeRange(v);
    for (std::size_t i = b; i < e; ++i) {
      std::uint32_t u = dsts[i];
      if (dist[u] < 0) {
        dist[u] = dist[v] + 1;
        q.push(u);
      }
    }
  }
  return dist;
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
        id_col < (int)cols.size() && round_col < (int)cols.size())
      rounds[cols[id_col]] = std::stoi(cols[round_col]);
  }
  return rounds;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr, rules, traces, goals_tsv;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--rules") rules = next();
      else if (k == "--traces") traces = next();
      else if (k == "--goals-tsv") goals_tsv = next();
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty() || rules.empty() || traces.empty())
      throw std::invalid_argument(
          "usage: gar_graph_probe --csr DIR --rules DIR --traces DIR "
          "[--goals-tsv FILE]");

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
    auto rounds = LoadRounds(goals_tsv);
    const int unreachable = static_cast<int>(graph.num_entities());

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
      std::vector<std::uint32_t> sources;
      if (trace.goal.args[0]) sources.push_back(*trace.goal.args[0]);
      if (trace.goal.args[1]) sources.push_back(*trace.goal.args[1]);
      auto dist = Bfs(graph, sources);
      std::unordered_set<agent::FactKey, agent::FactKeyHash> proof;
      for (const auto& d : trace.derivations)
        proof.insert(KeyOf(d.consequent));
      std::unordered_set<agent::FactKey, agent::FactKeyHash> seen;
      for (const auto& c : trace.candidates) {
        auto key = KeyOf(c.consequent);
        if (!seen.insert(key).second) continue;
        int d0 = c.consequent.args[0] < dist.size()
                     ? dist[c.consequent.args[0]] : -1;
        int d1 = c.consequent.args[1] < dist.size()
                     ? dist[c.consequent.args[1]] : -1;
        int best = unreachable;
        if (d0 >= 0) best = std::min(best, d0);
        if (d1 >= 0) best = std::min(best, d1);
        Sample s{static_cast<double>(-best), c.handcrafted_score,
                 proof.count(key) ? 1 : 0, round};
        all.push_back(s);
        by_round[round].push_back(s);
      }
    }
    Report("overall", all);
    for (const auto& [round, samples] : by_round)
      Report("round" + std::to_string(round), samples);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_graph_probe: " << e.what() << '\n';
    return 2;
  }
}