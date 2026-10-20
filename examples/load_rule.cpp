// Loads a GAR rule (CSR topology + YAML attributes) and prints it.
//
// Usage: load_rule <rule_dir>
//   e.g. load_rule data/rules/example
#include <iostream>

#include "gar/rule_loader.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <rule_dir>\n";
    return 1;
  }
  try {
    gar::GARule rule = gar::RuleLoader::Load(argv[1]);
    rule.Print(std::cout);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
