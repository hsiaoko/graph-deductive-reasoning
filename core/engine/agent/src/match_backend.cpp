#include "match_backend.h"

#include <stdexcept>

#include "gpu_match_backend.h"

namespace gar::core::engine::agent {

std::unique_ptr<MatchBackend> CreateCpuMatchBackend(
    const gar::core::graph::Graph& graph);

std::unique_ptr<MatchBackend> MakeMatchBackend(
    const MatchEngine engine, const gar::core::graph::Graph& graph) {
  switch (engine) {
    case MatchEngine::kCpu: return CreateCpuMatchBackend(graph);
    case MatchEngine::kGpu: return CreateGpuMatchBackend(graph);
  }
  throw std::invalid_argument("invalid agent match engine");
}

}  // namespace gar::core::engine::agent
