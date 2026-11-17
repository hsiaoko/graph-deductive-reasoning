#include "match_backend.h"

#include <stdexcept>
#include <string>

#include "gpu_match_backend.h"

namespace gar::core::engine::agent {

std::unique_ptr<MatchBackend> CreateCpuMatchBackend(
    const gar::core::graph::Graph& graph);

std::unique_ptr<MatchBackend> MakeMatchBackend(
    const std::string_view engine, const gar::core::graph::Graph& graph) {
  if (engine == "cpu") return CreateCpuMatchBackend(graph);
  if (engine == "gpu") return CreateGpuMatchBackend(graph);
  throw std::invalid_argument("unknown agent match engine: " +
                              std::string(engine));
}

}  // namespace gar::core::engine::agent
