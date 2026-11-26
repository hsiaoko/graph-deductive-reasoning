// Throwaway prototype: per-request pinned Enumerate vs bulk EnumerateSeeds on
// the same seed batch. Settles whether sharing setup + a tight seed loop (no
// per-seed binary search) beats the fragmented per-request delta join. Verifies
// match counts agree, then reports timing. Not a product surface.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/core/graph/graph.h"
#include "gar/core/graph/pattern_matcher.h"

namespace {
using gar::core::graph::Graph;
using gar::core::graph::PackPair;
using gar::core::graph::PatternMatcher;
using gar::core::graph::PatternSpec;
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr;
    std::size_t nseeds = 50000;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--seeds") nseeds = std::stoull(next());
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty()) throw std::invalid_argument("usage: gar_bulk_probe --csr DIR [--seeds N]");
    Graph g = gar::core::graph::LoadGraph(csr, true);
    // Pick two slots with edges for a 3-node chain (0)-A->(1)-B->(2).
    std::uint32_t A = UINT32_MAX, B = UINT32_MAX;
    for (std::uint32_t s = 0; s < g.num_relations(); ++s) {
      if (g.RelationPairs(s).size() < 100) continue;
      if (A == UINT32_MAX) A = s;
      else if (B == UINT32_MAX && s != A) { B = s; break; }
    }
    if (B == UINT32_MAX) throw std::runtime_error("need two nonempty slots");
    PatternSpec p;
    p.num_vertices = 3;
    p.edges = {{0, 1, A}, {1, 2, B}};
    PatternMatcher m(g);

    const auto& slotA = g.RelationPairs(A);
    std::vector<std::uint64_t> seeds(slotA.begin(),
                                     slotA.begin() + std::min(nseeds, slotA.size()));
    std::printf("pattern (0)-%u->(1)-%u->(2)  seeds=%zu\n", A, B, seeds.size());

    // Variant A: per-seed pinned Enumerate (the current delta style).
    std::vector<std::uint32_t> outA;
    bool capA = false;
    auto t0 = Clock::now();
    for (std::uint64_t sp : seeds) {
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      pins[0] = static_cast<std::uint32_t>(sp >> 32);
      pins[1] = static_cast<std::uint32_t>(sp);
      bool c = false;
      m.Enumerate(p, 1000000000, &outA, &c, pins);
      capA = capA || c;
    }
    auto t1 = Clock::now();

    // Variant B: bulk EnumerateSeeds (shared setup, tight loop).
    std::vector<std::uint32_t> outB;
    bool capB = false;
    auto t2 = Clock::now();
    m.EnumerateSeeds(p, 0, 1000000000, seeds, &outB, &capB);
    auto t3 = Clock::now();

    const std::size_t nA = outA.size() / 3, nB = outB.size() / 3;
    std::printf("seed_edge=0 per-request: matches=%zu time=%.1fms\n", nA, Ms(t0, t1));
    std::printf("seed_edge=0 bulk-seeds : matches=%zu time=%.1fms  agree=%s speedup=%.2fx\n",
                nB, Ms(t2, t3), nA == nB ? "yes" : "NO",
                Ms(t2, t3) > 0 ? Ms(t0, t1) / Ms(t2, t3) : 0.0);

    // Generalization check: seed from the SECOND pattern edge (1)-B->(2).
    const auto& slotB = g.RelationPairs(B);
    std::vector<std::uint64_t> seedsB(slotB.begin(),
                                      slotB.begin() + std::min(nseeds, slotB.size()));
    std::vector<std::uint32_t> outC, outD;
    bool cC = false, cD = false;
    auto u0 = Clock::now();
    for (std::uint64_t sp : seedsB) {
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      pins[1] = static_cast<std::uint32_t>(sp >> 32);
      pins[2] = static_cast<std::uint32_t>(sp);
      bool c = false;
      m.Enumerate(p, 1000000000, &outC, &c, pins);
      cC = cC || c;
    }
    auto u1 = Clock::now();
    m.EnumerateSeeds(p, 1, 1000000000, seedsB, &outD, &cD);
    auto u2 = Clock::now();
    const std::size_t nC = outC.size() / 3, nD = outD.size() / 3;
    std::printf("seed_edge=1 per-request: matches=%zu time=%.1fms\n", nC, Ms(u0, u1));
    std::printf("seed_edge=1 bulk-seeds : matches=%zu time=%.1fms  agree=%s speedup=%.2fx\n",
                nD, Ms(u1, u2), nC == nD ? "yes" : "NO",
                Ms(u1, u2) > 0 ? Ms(u0, u1) / Ms(u1, u2) : 0.0);
    return (nA == nB && nC == nD) ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "gar_bulk_probe: %s\n", e.what());
    return 2;
  }
}