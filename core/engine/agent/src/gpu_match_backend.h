#pragma once

#include <memory>

#include "match_backend.h"

namespace gar::core::engine::agent {

std::unique_ptr<MatchBackend> CreateGpuMatchBackend(
    const gar::core::graph::Graph& graph);

}  // namespace gar::core::engine::agent
