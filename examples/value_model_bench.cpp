// Benchmark one HDC value-model backend on deterministic feature rows.
// Usage: value_model_bench <model.meta> <original|simd|bit-lut> <calls>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>

#include "gar/engine/agent/value_model.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: " << argv[0]
              << " <model.meta> <original|simd|bit-lut> <calls>\n";
    return 1;
  }
  namespace agent = gar::engine::agent;
  agent::HdcBackend backend;
  const std::string name = argv[2];
  if (name == "original") {
    backend = agent::HdcBackend::kOriginal;
  } else if (name == "simd") {
    backend = agent::HdcBackend::kSimd;
  } else if (name == "bit-lut") {
    backend = agent::HdcBackend::kBitLut;
  } else {
    std::cerr << "unknown backend: " << name << "\n";
    return 1;
  }
  const std::uint64_t calls = std::stoull(argv[3]);
  if (calls == 0) {
    std::cerr << "calls must be positive\n";
    return 1;
  }
  agent::ValueModel model;
  model.SetHdcBackend(backend);
  const auto load_started = std::chrono::steady_clock::now();
  if (!model.Load(argv[1]) || model.kind() != "hdc") return 1;
  const auto loaded = std::chrono::steady_clock::now();

  std::mt19937_64 rng(20261108);
  std::uniform_real_distribution<double> value(-5.0, 5.0);
  std::array<std::array<double, 11>, 128> rows{};
  for (auto& row : rows) {
    for (double& x : row) x = value(rng);
  }
  double checksum = 0.0;
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < calls; ++i) {
    checksum += model.PredictRemaining(rows[i % rows.size()]);
  }
  const double wall_ns = std::chrono::duration<double, std::nano>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  const double load_ms = std::chrono::duration<double, std::milli>(loaded -
                                                                  load_started)
                             .count();
  std::cout << std::setprecision(17) << "backend\t" << name << "\n"
            << "calls\t" << calls << "\n"
            << "load_ms\t" << load_ms << "\n"
            << "wall_ms\t" << wall_ns / 1.0e6 << "\n"
            << "infer_ms\t" << model.infer_ns / 1.0e6 << "\n"
            << "wall_ns_per_call\t" << wall_ns / calls << "\n"
            << "infer_ns_per_call\t" << model.infer_ns / calls << "\n"
            << "checksum\t" << checksum << "\n";
  return 0;
}
