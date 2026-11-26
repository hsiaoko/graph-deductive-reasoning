// Reports the dependency-cone size of every relation predicate: |cone(g)| / |R|.
// Used by exp-2-goal to characterize central vs peripheral predicates and pick a
// goal panel spanning goal-irrelevant proportions. Loads graph + rules once.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr, rules;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--rules") rules = next();
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty() || rules.empty())
      throw std::invalid_argument("usage: gar_cone_sizes --csr DIR --rules DIR");

    gar::core::graph::Graph graph = gar::core::graph::LoadGraph(csr, true);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(rules))
      if (e.is_directory() && std::filesystem::exists(e.path() / "rule.yaml"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<gar::GARule> rule_vec;
    for (const auto& d : dirs) rule_vec.push_back(gar::RuleLoader::Load(d));

    std::cout << "predicate\tcone_size\ttotal_rules\tirrelevant_pct\n";
    for (std::uint32_t r = 0; r < graph.num_relations(); ++r) {
      const std::string name = graph.RelationName(r);
      auto cone = gar::core::analysis::GoalConeRuleIndices(rule_vec, name);
      const double irr =
          100.0 * (1.0 - static_cast<double>(cone.size()) / rule_vec.size());
      std::cout << name << "\t" << cone.size() << "\t" << rule_vec.size()
                << "\t" << irr << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_cone_sizes: " << e.what() << '\n';
    return 2;
  }
}