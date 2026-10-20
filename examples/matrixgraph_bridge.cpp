// Loads a GAR rule and converts it to MatrixGraph's GARPatternArrays layout
// (the input format of MatrixGraph's GAR matching kernels).
//
// Usage: matrixgraph_bridge <rule_dir>
#include <iostream>

#include "gar/bridge/pattern_bridge.h"
#include "gar/rule_loader.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <rule_dir>\n";
    return 1;
  }
  try {
    gar::GARule rule = gar::RuleLoader::Load(argv[1]);
    auto p = gar::bridge::ToPatternArrays(rule);
    p.Print();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
