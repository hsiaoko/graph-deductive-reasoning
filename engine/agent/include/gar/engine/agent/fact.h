#pragma once

// Fact: a ground predicate instance — the atom of the fact set Γ.
//
//   kEdge   edge (slot, args[0], args[1]); slot is always the BASE relation
//           slot (inverse-slot matches are normalized by swapping args, so
//           a derived fact r(u,v) and a later match of r^-1(v,u) key equal)
//   kAttr   attribute fact on a vertex: (predicate-table id of the
//           attribute name, vertex) arity 1; a grounded var predicate
//           x.A=y.B keeps both vertices (arity 2)
//   kMl     ML-predicate fact: (predicate-table id of the model name, two
//           vertices)

#include <array>
#include <cstdint>

namespace gar {
namespace engine {
namespace agent {

enum class FactKind { kEdge, kAttr, kMl };

struct Fact {
  FactKind kind = FactKind::kEdge;
  std::uint32_t pred = 0;  // kEdge: base slot; kAttr/kMl: predicate-table id
  std::array<std::uint32_t, 2> args = {0, 0};
  std::uint32_t arity = 0;

  bool operator==(const Fact& o) const {
    return kind == o.kind && pred == o.pred && args == o.args &&
           arity == o.arity;
  }
};

// Hash key form of a Fact (kind+pred packed with both args).
struct FactKey {
  std::uint64_t hi = 0;  // kind | pred
  std::uint64_t lo = 0;  // args[0] | args[1]

  bool operator==(const FactKey& o) const {
    return hi == o.hi && lo == o.lo;
  }
};

struct FactKeyHash {
  std::size_t operator()(const FactKey& k) const {
    std::uint64_t h = k.hi * 0x9E3779B97F4A7C15ull;
    h ^= k.lo + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

inline FactKey KeyOf(const Fact& f) {
  FactKey k;
  k.hi = (static_cast<std::uint64_t>(f.kind) << 32) | f.pred;
  k.lo = (static_cast<std::uint64_t>(f.args[0]) << 32) | f.args[1];
  return k;
}

inline Fact MakeEdgeFact(std::uint32_t base_slot, std::uint32_t src,
                         std::uint32_t dst) {
  return Fact{FactKind::kEdge, base_slot, {src, dst}, 2};
}

inline Fact MakeAttrFact(std::uint32_t pred_id, std::uint32_t vertex) {
  return Fact{FactKind::kAttr, pred_id, {vertex, 0}, 1};
}

// Grounded var predicate x.A=y.B: both vertices kept.
inline Fact MakeAttrFact2(std::uint32_t pred_id, std::uint32_t v0,
                          std::uint32_t v1) {
  return Fact{FactKind::kAttr, pred_id, {v0, v1}, 2};
}

inline Fact MakeMlFact(std::uint32_t pred_id, std::uint32_t v0,
                       std::uint32_t v1) {
  return Fact{FactKind::kMl, pred_id, {v0, v1}, 2};
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
