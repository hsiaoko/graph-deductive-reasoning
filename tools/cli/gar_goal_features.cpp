// Diagnostic feature extractor for the learned strategy selector (path C).
// Emits per-goal structural features that predict whether proof-directed
// backward search succeeds: dependency-cone rule count and goal-predicate base
// degree. Central predicates have large cones and high degree, and backward
// search explodes on them. Throwaway instrument; pairs with backward-success
// labels to train a goal-level strategy classifier.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

namespace {
using gar::core::graph::Graph;

struct GoalSpec {
  std::string rel;
  std::uint32_t slot;
};

GoalSpec ParseRel(const std::string& spec, const Graph& g) {
  const auto lp = spec.find('(');
  if (lp == std::string::npos)
    throw std::invalid_argument("bad goal spec: " + spec);
  const std::string rel = spec.substr(0, lp);
  for (std::uint32_t r = 0; r < g.num_relations(); ++r)
    if (g.RelationName(r) == rel) return {rel, r};
  throw std::invalid_argument("unknown relation: " + rel);
}

std::vector<std::string> Fields(const std::string& v) {
  std::vector<std::string> out;
  std::size_t b = 0;
  while (true) {
    std::size_t t = v.find('\t', b);
    out.push_back(v.substr(b, t - b));
    if (t == std::string::npos) return out;
    b = t + 1;
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr, rules, goals_tsv, out;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--rules") rules = next();
      else if (k == "--goals-tsv") goals_tsv = next();
      else if (k == "--out") out = next();
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty() || rules.empty() || goals_tsv.empty() || out.empty())
      throw std::invalid_argument(
          "usage: gar_goal_features --csr DIR --rules DIR --goals-tsv FILE "
          "--out FILE");

    Graph graph = gar::core::graph::LoadGraph(csr, true);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(rules))
      if (e.is_directory() &&
          std::filesystem::exists(e.path() / "rule.yaml"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<gar::GARule> rule_vec;
    for (const auto& d : dirs) rule_vec.push_back(gar::RuleLoader::Load(d));

    std::ifstream in(goals_tsv);
    std::ofstream os(out);
    os << "goal_id\tcone_size\tpred_degree\n";
    std::string line;
    bool first = true;
    int id_col = -1, spec_col = -1;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      auto cols = Fields(line);
      if (first) {
        for (std::size_t i = 0; i < cols.size(); ++i) {
          if (cols[i] == "goal_id") id_col = i;
          if (cols[i] == "spec") spec_col = i;
        }
        first = false;
        continue;
      }
      if (id_col < 0 || spec_col < 0 ||
          id_col >= (int)cols.size() || spec_col >= (int)cols.size())
        continue;
      GoalSpec g = ParseRel(cols[spec_col], graph);
      auto cone = gar::core::analysis::GoalConeRuleIndices(rule_vec, g.rel);
      std::size_t degree = graph.RelationPairs(g.slot).size();
      os << cols[id_col] << "\t" << cone.size() << "\t" << degree << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_goal_features: " << e.what() << '\n';
    return 2;
  }
}