#pragma once

#include <cstdint>
#include <unordered_map>

#include "gar/core/agent/fact.h"
#include "gar/core/agent/goal.h"
#include "gar/core/graph/graph.h"
#include "gar/core/hdc/encoder.h"

namespace gar::core::analysis {
class PredicateTable;
}  // namespace gar::core::analysis

namespace gar::core::engine::agent {

std::uint64_t FactPredicateSymbol(
    const Fact& fact, const gar::core::graph::Graph& graph,
    const gar::core::analysis::PredicateTable& predicates);

std::uint64_t GraphFingerprint(const gar::core::graph::Graph& graph);

class FactEncoder {
 public:
  FactEncoder(gar::core::hdc::HdcEncoder encoder,
              const gar::core::graph::Graph* graph,
              const gar::core::analysis::PredicateTable* predicates)
      : encoder_(encoder), graph_(graph), predicates_(predicates) {}

  [[nodiscard]] gar::core::hdc::DenseVector Encode(const Fact& fact) const;
  [[nodiscard]] gar::core::hdc::DenseVector EncodeGoal(const Goal& goal) const;
  [[nodiscard]] const gar::core::hdc::HdcEncoder& hdc() const {
    return encoder_;
  }

 private:
  // Indexed store of materialized symbol vectors. A symbol's dense vector is a
  // pure function of (seed, symbol_id), and the same entity/predicate recurs
  // across countless facts, so generating it once and looking it up by id
  // replaces the per-fact hash->permute->dense pipeline with table reads. The
  // vectors remain hash-derived (no learned embeddings); this memoizes the
  // on-the-fly generation rather than changing the encoding.
  const gar::core::hdc::DenseVector& PredicateDense(std::uint64_t symbol) const;
  const gar::core::hdc::DenseVector& ArgumentDense(std::uint32_t position,
                                                   std::uint64_t entity) const;

  gar::core::hdc::HdcEncoder encoder_;
  const gar::core::graph::Graph* graph_;
  const gar::core::analysis::PredicateTable* predicates_;
  // Relation -> predicate symbol (pure hash of the relation name; avoids a
  // string allocation + FNV hash per encoded fact).
  mutable std::unordered_map<std::uint32_t, std::uint64_t> pred_symbol_cache_;
  mutable std::unordered_map<std::uint64_t, gar::core::hdc::DenseVector>
      pred_dense_cache_;
  // (position << 32 | entity) -> rho^position(v_entity) as a dense vector.
  mutable std::unordered_map<std::uint64_t, gar::core::hdc::DenseVector>
      arg_dense_cache_;
};

}  // namespace gar::core::engine::agent
