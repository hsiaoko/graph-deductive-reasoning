// Builds the union of the dependency cones of a goal set: the rule subset A
// whose consequents can reach ANY goal predicate. A is a subset of the full
// rule library, so chasing A once must derive no more (and cost no more) than
// chasing all rules. Writes A as a directory of symlinks to the selected rule
// dirs, preserving rule ids, so `gar_reason --mode chase --rules A` runs the
// fair "goal-relevant rules, one pass" baseline (not a per-goal sum).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

namespace {
using gar::core::graph::Graph;

std::string RelOf(const std::string& spec) {
  const auto lp = spec.find('(');
  if (lp == std::string::npos)
    throw std::invalid_argument("bad goal spec: " + spec);
  return spec.substr(0, lp);
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
          "usage: gar_cone_union --csr DIR --rules DIR --goals-tsv FILE --out DIR");

    Graph graph = gar::core::graph::LoadGraph(csr, true);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(rules))
      if (e.is_directory() && std::filesystem::exists(e.path() / "rule.yaml"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<gar::GARule> rule_vec;
    for (const auto& d : dirs) rule_vec.push_back(gar::RuleLoader::Load(d));

    std::set<std::string> goal_preds;
    std::ifstream in(goals_tsv);
    std::string line;
    bool first = true;
    int spec_col = -1;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      auto cols = Fields(line);
      if (first) {
        for (std::size_t i = 0; i < cols.size(); ++i)
          if (cols[i] == "spec") spec_col = i;
        first = false;
        continue;
      }
      if (spec_col >= 0 && spec_col < (int)cols.size())
        goal_preds.insert(RelOf(cols[spec_col]));
    }

    std::set<std::size_t> kept;
    for (const auto& pred : goal_preds) {
      auto cone = gar::core::analysis::GoalConeRuleIndices(rule_vec, pred);
      kept.insert(cone.begin(), cone.end());
    }

    std::filesystem::create_directories(out);
    for (std::size_t i : kept) {
      const auto& src = dirs[i];
      auto dst = out / src.filename();
      if (!std::filesystem::exists(dst))
        std::filesystem::create_directory_symlink(
            std::filesystem::absolute(src), dst);
    }
    std::cout << "goal predicates: " << goal_preds.size()
              << "  cone-union rules A: " << kept.size() << " / "
              << rule_vec.size() << "\n";
    std::cout << "filtered rules dir: " << out << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_cone_union: " << e.what() << '\n';
    return 2;
  }
}