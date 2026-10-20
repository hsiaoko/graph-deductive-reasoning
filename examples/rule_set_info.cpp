// Loads GAR rules and prints a summary. Accepts either a rule root
// directory (one subdirectory per rule, the mine_rules output layout) or a
// single rule-set YAML file.
//
// Usage: rule_set_info <rules_root_dir | rules.yaml> [max_print]
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include "gar/rule_loader.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0]
              << " <rules_root_dir | rules.yaml> [max_print]\n";
    return 1;
  }
  const std::size_t max_print = argc > 2 ? std::strtoul(argv[2], nullptr, 10)
                                         : static_cast<std::size_t>(10);
  try {
    std::vector<gar::GARule> rules;
    const std::filesystem::path p(argv[1]);
    if (std::filesystem::is_directory(p)) {
      std::vector<std::filesystem::path> dirs;
      for (const auto& entry : std::filesystem::directory_iterator(p)) {
        if (entry.is_directory() &&
            std::filesystem::exists(entry.path() / "rule.yaml")) {
          dirs.push_back(entry.path());
        }
      }
      std::sort(dirs.begin(), dirs.end());
      for (const auto& d : dirs) {
        rules.push_back(gar::RuleLoader::Load(d));
      }
      std::cout << "loaded " << rules.size() << " rule dirs from " << p
                << "\n";
    } else {
      rules = gar::RuleLoader::LoadRuleSet(p);
      std::cout << "loaded " << rules.size() << " rules from " << p << "\n";
    }
    for (std::size_t i = 0; i < rules.size() && i < max_print; ++i) {
      rules[i].Print(std::cout);
      std::cout << "---\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
