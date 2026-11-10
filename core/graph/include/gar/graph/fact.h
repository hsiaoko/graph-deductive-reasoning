#pragma once

#include <cstdint>

// A fact derived by one chase round: a new edge (src, dst) in relation
// `slot`, with its provenance (deriving rule and 1-based round).
namespace gar {
namespace graph {

struct DerivedEdge {
  std::uint32_t src, dst, slot;  // slot as named by the rule's head label
  std::uint32_t rule;            // index into the input rule vector
  std::uint32_t round;           // 1-based chase round
};

}  // namespace graph
}  // namespace gar
