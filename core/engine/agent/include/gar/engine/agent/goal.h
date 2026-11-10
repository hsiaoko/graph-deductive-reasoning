#pragma once

// Goal: the target of an agent run — a fact of the conclusion shape (an
// edge on a base-relation slot) with optional open arguments. `_` in the
// CLI parses to std::nullopt: any value at that position satisfies it.

#include <array>
#include <cstdint>
#include <optional>

#include "gar/graph/fact.h"

namespace gar::core::engine {
namespace agent {

struct Goal {
  std::uint32_t slot = 0;  // base relation slot
  std::array<std::optional<std::uint32_t>, 2> args;  // nullopt = open
};

// A derived edge satisfies the goal when slot and all bound arguments match.
inline bool GoalSatisfied(const Goal& g, const gar::core::graph::DerivedEdge& e) {
  if (e.slot != g.slot) return false;
  if (g.args[0] && *g.args[0] != e.src) return false;
  if (g.args[1] && *g.args[1] != e.dst) return false;
  return true;
}

}  // namespace agent
}  // namespace gar::core::engine
