// Loads a ValueModel (meta + sibling bin) and prints PredictRemaining for
// 11-feature rows read from stdin (whitespace-separated doubles, one row
// per line). Used to cross-check the C++ inference path against the Python
// training-side reference (experiments/hdc_hard_goals/verify_numeric.py).
//
// Usage: value_model_check <model.meta> [original|simd|bit-lut] < rows.txt
#include <array>
#include <iostream>
#include <string>

#include "gar/engine/agent/value_model.h"

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: " << argv[0]
              << " <model.meta> [original|simd|bit-lut]\n";
    return 1;
  }
  namespace agent = gar::engine::agent;
  agent::ValueModel vm;
  if (argc == 3) {
    const std::string backend = argv[2];
    if (backend == "original") {
      vm.SetHdcBackend(agent::HdcBackend::kOriginal);
    } else if (backend == "simd") {
      vm.SetHdcBackend(agent::HdcBackend::kSimd);
    } else if (backend == "bit-lut") {
      vm.SetHdcBackend(agent::HdcBackend::kBitLut);
    } else {
      std::cerr << "unknown backend: " << backend << "\n";
      return 1;
    }
  }
  if (!vm.Load(argv[1])) return 1;
  std::cerr << "[value_model_check] kind=" << vm.kind()
            << " backend=" << agent::HdcBackendName(vm.hdc_backend()) << "\n";
  std::array<double, 11> raw{};
  while (true) {
    bool ok = true;
    for (auto& v : raw) {
      if (!(std::cin >> v)) { ok = false; break; }
    }
    if (!ok) break;
    std::cout.precision(17);
    std::cout << vm.PredictRemaining(raw) << "\n";
  }
  return 0;
}
