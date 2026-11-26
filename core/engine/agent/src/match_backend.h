#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "gar/core/agent/config.h"
#include "gar/core/graph/fact.h"
#include "gar/core/graph/graph.h"
#include "gar/core/graph/pattern_matcher.h"

namespace gar::core::engine::agent {

struct EdgeSeed {
  std::uint32_t slot = 0;
  std::uint32_t src = 0;
  std::uint32_t dst = 0;
  std::uint32_t edge = 0;
};

struct MatchRequest {
  gar::core::graph::PatternSpec spec;
  std::uint64_t cap = 0;
  std::unordered_map<std::uint32_t, std::uint32_t> pins;
  std::optional<EdgeSeed> seed;
  // Owning star (rule plan index). Bulk-delta groups requests by this so matches
  // are attributed to the correct rule/head; distinct stars may share a spec.
  std::uint32_t group = 0;
};

struct MatchResult {
  std::vector<std::uint32_t> embeddings;
  bool capped = false;
};

class MatchBackend {
 public:
  virtual ~MatchBackend() = default;

  virtual std::vector<MatchResult> RunBatch(
      const std::vector<MatchRequest>& requests) = 0;
  virtual void CommitDerived(
      const gar::core::graph::DerivedEdge& derived) = 0;
};

std::unique_ptr<MatchBackend> MakeMatchBackend(
    MatchEngine engine, const gar::core::graph::Graph& graph);

}  // namespace gar::core::engine::agent
