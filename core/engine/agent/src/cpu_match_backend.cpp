#include "match_backend.h"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "gar/core/graph/pattern_matcher.h"

namespace gar::core::engine::agent {
namespace {

// Bulk-delta switch (GAR_BULK_DELTA=1): group delta requests that share a
// pattern and seed edge, then enumerate their new-edge seeds in one tight
// EnumerateSeeds pass instead of one pinned Enumerate per edge. The prototype
// (gar_bulk_probe) measured 7-28x over per-request on real graphs with identical
// match counts. Combined matches land in the group's first result; the session's
// AddComplete dedups, so empty sibling results contribute nothing.
bool BulkDelta() {
  static const bool on = [] {
    const char* e = std::getenv("GAR_BULK_DELTA");
    return e == nullptr || std::string(e) != "0";
  }();
  return on;
}

// Exact grouping key (no hash collisions): num_vertices, seed edge, then every
// pattern edge. A collision would batch seeds under the wrong pattern and drop
// derivations, so the key compares full spec content.
std::vector<std::uint32_t> SpecKey(const gar::core::graph::PatternSpec& p,
                                   std::uint32_t seed_edge,
                                   std::uint32_t group) {
  std::vector<std::uint32_t> k;
  k.reserve(3 + p.edges.size() * 3);
  k.push_back(group);
  k.push_back(p.num_vertices);
  k.push_back(seed_edge);
  for (const auto& e : p.edges) {
    k.push_back(e[0]);
    k.push_back(e[1]);
    k.push_back(e[2]);
  }
  return k;
}

class CpuMatchBackend final : public MatchBackend {
 public:
  explicit CpuMatchBackend(const gar::core::graph::Graph& graph)
      : matcher_(graph), bulk_(BulkDelta()) {}

  std::vector<MatchResult> RunBatch(
      const std::vector<MatchRequest>& requests) override {
    std::vector<MatchResult> results(requests.size());
    if (!bulk_) {
      for (std::size_t i = 0; i < requests.size(); ++i) {
        const MatchRequest& request = requests[i];
        matcher_.Enumerate(request.spec, request.cap, &results[i].embeddings,
                           &results[i].capped, request.pins);
      }
      return results;
    }
    // Group seed-bearing (delta) requests by (pattern, seed edge); requests
    // without a seed (goal-pinned generation) stay per-request.
    std::map<std::vector<std::uint32_t>, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const MatchRequest& request = requests[i];
      if (!request.seed.has_value()) {
        matcher_.Enumerate(request.spec, request.cap, &results[i].embeddings,
                           &results[i].capped, request.pins);
        continue;
      }
      groups[SpecKey(request.spec, request.seed->edge, request.group)]
          .push_back(i);
    }
    for (const auto& [key, idxs] : groups) {
      const MatchRequest& first = requests[idxs[0]];
      std::vector<std::uint64_t> seed_pairs;
      seed_pairs.reserve(idxs.size());
      for (std::size_t i : idxs) {
        const auto& s = *requests[i].seed;
        seed_pairs.push_back(gar::core::graph::PackPair(s.src, s.dst));
      }
      // Requests in a group share one star (group), pattern and seed edge; only
      // the new-edge seeds differ. The combined embeddings are the dedup-union of
      // the per-seed matches, so placing them in the first result and leaving
      // siblings empty reproduces the per-request AddComplete outcome.
      matcher_.EnumerateSeeds(first.spec, first.seed->edge,
                              static_cast<std::uint64_t>(1000000000),
                              seed_pairs, &results[idxs[0]].embeddings,
                              &results[idxs[0]].capped);
    }
    return results;
  }

  void CommitDerived(
      const gar::core::graph::DerivedEdge& derived) override {
    (void)derived;
  }

 private:
  gar::core::graph::PatternMatcher matcher_;
  bool bulk_;
};

}  // namespace

std::unique_ptr<MatchBackend> CreateCpuMatchBackend(
    const gar::core::graph::Graph& graph) {
  return std::make_unique<CpuMatchBackend>(graph);
}

}  // namespace gar::core::engine::agent
