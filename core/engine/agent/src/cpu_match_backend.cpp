#include "match_backend.h"

#include <vector>

#include "gar/core/graph/pattern_matcher.h"

namespace gar::core::engine::agent {
namespace {

class CpuMatchBackend final : public MatchBackend {
 public:
  explicit CpuMatchBackend(const gar::core::graph::Graph& graph)
      : matcher_(graph) {}

  std::vector<MatchResult> RunBatch(
      const std::vector<MatchRequest>& requests) override {
    std::vector<MatchResult> results(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const MatchRequest& request = requests[i];
      matcher_.Enumerate(request.spec, request.cap, &results[i].embeddings,
                         &results[i].capped, request.pins);
    }
    return results;
  }

  void CommitDerived(
      const gar::core::graph::DerivedEdge& derived) override {
    (void)derived;
  }

 private:
  gar::core::graph::PatternMatcher matcher_;
};

}  // namespace

std::unique_ptr<MatchBackend> CreateCpuMatchBackend(
    const gar::core::graph::Graph& graph) {
  return std::make_unique<CpuMatchBackend>(graph);
}

}  // namespace gar::core::engine::agent
