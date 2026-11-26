// Diagnostic probe: does the §5.2 binary-sketch resumption filter actually skip
// work? Builds B_Gamma = bitwise-OR of binarized fact HDC vectors over a growing
// fact set, reports the saturation curve (popcount / dim), then measures the
// skip rate on absent facts and the false-pass rate on present facts. A sketch
// that saturates to all-ones filters nothing and only adds overhead. Throwaway
// instrument; reuses the real FactEncoder so binarization matches the paper.
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "gar/core/agent/fact_encoder.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule_loader.h"

namespace {
namespace agent = gar::core::engine::agent;
using gar::core::graph::Graph;

std::vector<std::uint64_t> Binarize(const gar::core::hdc::DenseVector& v) {
  const std::uint32_t d = v.dim();
  std::vector<std::uint64_t> b((d + 63) / 64, 0);
  const auto& vals = v.values();
  for (std::uint32_t i = 0; i < d; ++i)
    if (vals[i] > 0.0f) b[i >> 6] |= (1ull << (i & 63));
  return b;
}

std::uint32_t Popcount(const std::vector<std::uint64_t>& b) {
  std::uint32_t n = 0;
  for (auto w : b) n += __builtin_popcountll(w);
  return n;
}

// Sparse k-hot signature (Bloom-style): set k hash positions of the fact key,
// leaving the rest 0. Fewer set bits per fact slows OR-saturation; this is the
// "allocate more zeros" idea. Contrasts with dense binarization (~dim/2 ones).
std::vector<std::uint64_t> Sparse(std::uint32_t r, std::uint32_t s,
                                  std::uint32_t d, std::uint32_t k,
                                  std::uint32_t dim) {
  std::vector<std::uint64_t> b((dim + 63) / 64, 0);
  std::uint64_t state = (static_cast<std::uint64_t>(r) << 40) ^
                        (static_cast<std::uint64_t>(s) << 20) ^ d;
  state = gar::core::hdc::SplitMix64Next(state);
  for (std::uint32_t i = 0; i < k; ++i) {
    const std::uint64_t h = gar::core::hdc::SplitMix64Next(state);
    const std::uint32_t pos = static_cast<std::uint32_t>(h % dim);
    b[pos >> 6] |= (1ull << (pos & 63));
  }
  return b;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csr, rules;
    std::uint32_t dim = 128;
    std::uint64_t seed = 42;
    std::size_t max_facts = 0;
    std::uint32_t sparse_k = 0;
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      auto next = [&]() { return std::string(argv[++i]); };
      if (k == "--csr") csr = next();
      else if (k == "--rules") rules = next();
      else if (k == "--hdc-dim") dim = std::stoul(next());
      else if (k == "--hdc-seed") seed = std::stoull(next());
      else if (k == "--max-facts") max_facts = std::stoull(next());
      else if (k == "--sparse-k") sparse_k = std::stoul(next());
      else throw std::invalid_argument("unknown option: " + k);
    }
    if (csr.empty() || rules.empty())
      throw std::invalid_argument(
          "usage: gar_sketch_probe --csr DIR --rules DIR [--hdc-dim N] "
          "[--hdc-seed N]");

    Graph graph = gar::core::graph::LoadGraph(csr, true);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(rules))
      if (e.is_directory() && std::filesystem::exists(e.path() / "rule.yaml"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<gar::GARule> rule_vec;
    for (const auto& d : dirs) rule_vec.push_back(gar::RuleLoader::Load(d));
    gar::core::analysis::PredicateTable predicates;
    gar::core::analysis::DependencyGraph dg;
    dg.Build(rule_vec, &predicates);
    agent::FactEncoder encoder(
        gar::core::hdc::HdcEncoder({dim, seed}), &graph, &predicates);

    // Collect base edges as facts (id triples).
    struct E { std::uint32_t r, s, d; };
    std::vector<E> edges;
    for (std::uint32_t r = 0; r < graph.num_relations(); ++r)
      for (auto p : graph.RelationPairs(r))
        edges.push_back({r, static_cast<std::uint32_t>(p >> 32),
                         static_cast<std::uint32_t>(p)});
    std::cout << "dim=" << dim << " base_edges=" << edges.size() << "\n";
    std::cout << "N_facts\tpopcount(B_Gamma)\tfraction_set\n";

    std::vector<std::uint64_t> bg((dim + 63) / 64, 0);
    std::vector<std::vector<std::uint64_t>> sketches;
    sketches.reserve(edges.size());
    const std::uint32_t checkpoints[] = {10, 50, 100, 500, 1000, 5000,
                                         10000, 50000, 100000};
    std::size_t ci = 0;
    const std::size_t limit =
        max_facts ? std::min(max_facts, edges.size()) : edges.size();
    std::cout << "mode=" << (sparse_k ? "sparse-k" : "dense-binarize")
              << (sparse_k ? std::to_string(sparse_k) : "") << "\n";
    for (std::size_t i = 0; i < limit; ++i) {
      auto b = sparse_k
                   ? Sparse(edges[i].r, edges[i].s, edges[i].d, sparse_k, dim)
                   : Binarize(encoder.Encode(agent::MakeEdgeFact(
                         edges[i].r, edges[i].s, edges[i].d)));
      for (std::size_t w = 0; w < bg.size(); ++w) bg[w] |= b[w];
      if (sketches.size() < 20000) sketches.push_back(std::move(b));
      const std::uint32_t n = i + 1;
      if (ci < std::size(checkpoints) && n == checkpoints[ci]) {
        std::cout << n << "\t" << Popcount(bg) << "\t"
                  << static_cast<double>(Popcount(bg)) / dim << "\n";
        ++ci;
      }
    }
    std::cout << "all\t" << Popcount(bg) << "\t"
              << static_cast<double>(Popcount(bg)) / dim << "\n";

    // Skip rate on ABSENT facts (random non-edges): a working sketch skips these
    // (B(Delta) & B_Gamma != B(Delta)). False-pass rate on PRESENT facts: the
    // sketch must never skip a present fact (soundness).
    std::mt19937_64 rng(7);
    const std::uint32_t nv = graph.num_entities();
    auto absent_sketch = [&]() {
      std::vector<std::uint64_t> b((dim + 63) / 64, 0);
      // OR of 3 random absent facts (a typical missing set Delta_mu).
      for (int k = 0; k < 3; ++k) {
        std::uint32_t r = rng() % graph.num_relations();
        std::uint32_t s = rng() % nv, d = rng() % nv;
        auto bb = sparse_k ? Sparse(r, s, d, sparse_k, dim)
                           : Binarize(encoder.Encode(agent::MakeEdgeFact(r, s, d)));
        for (std::size_t w = 0; w < b.size(); ++w) b[w] |= bb[w];
      }
      return b;
    };
    std::uint32_t skip = 0, trials = 2000;
    for (std::uint32_t t = 0; t < trials; ++t) {
      auto b = absent_sketch();
      bool covered = true;
      for (std::size_t w = 0; w < b.size(); ++w)
        if ((b[w] & bg[w]) != b[w]) { covered = false; break; }
      if (!covered) ++skip;
    }
    std::cout << "absent_fact_skip_rate=" << static_cast<double>(skip) / trials
              << " (" << skip << "/" << trials << ")\n";
    std::uint32_t false_skip = 0;
    for (std::size_t i = 0; i < sketches.size(); i += 97) {
      const auto& b = sketches[i];
      for (std::size_t w = 0; w < b.size(); ++w)
        if ((b[w] & bg[w]) != b[w]) { ++false_skip; break; }
    }
    std::cout << "present_fact_false_skip=" << false_skip << " (must be 0)\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gar_sketch_probe: " << e.what() << '\n';
    return 2;
  }
}