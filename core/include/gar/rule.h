#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/csr.h"
#include "gar/predicate.h"
#include "gar/types.h"

// GARule: a graph association rule
//
//   R = (Q[x̄], X -> p₀)
//
// * Q    : pattern graph. Its topology is stored as CSR (`pattern`), in
//          RAPIDS/cuGraph layout (uint32), loaded from csr_offsets.bin /
//          csr_indices.bin or built from inline edges in rule.yaml.
// * X    : preconditions (`antecedent`), a conjunction of predicates over
//          the pattern variables, loaded from rule.yaml.
// * p₀   : a single consequence predicate (`consequent`). When absent, the
//          object is a pattern query ψ = Q[x̄](X) instead of a rule.
//
// `pivot` designates the variables at which support is counted (usually the
// variables of p₀), cf. the pivot-based support of GFD/GAR discovery.
namespace gar {

struct RuleQuality {
  std::uint64_t support = 0;
  double confidence = 0.0;
  std::string dataset;  // provenance: source graph
  std::string miner;    // provenance: mining algorithm
};

struct GARule {
  std::string id;
  std::string name;
  std::string description;

  // Pattern topology Q (dependency graph of pattern nodes).
  CSRTopology pattern;

  // Flat edge list mirroring `pattern` (GARPatternArrays-compatible: int32
  // src/dst/label), filled by the loader. Kept in CSR order (grouped by
  // source vertex). This is the MatrixGraph-facing view; `pattern` (CSR)
  // remains for the in-repo consumers until Phase 2 finishes the switch.
  std::vector<std::int32_t> edge_src;
  std::vector<std::int32_t> edge_dst;
  std::vector<std::int32_t> edge_labels;

  // Per-node label constraints (size == pattern.num_vertices(),
  // kWildcardLabel for unconstrained).
  std::vector<LabelID> node_labels;
  std::vector<std::string> node_label_names;  // optional, parallel to above

  // Label dictionary (name -> LabelID) for node/edge labels used by the rule.
  std::unordered_map<std::string, LabelID> labels;

  // Pivot variables for support counting (subset of pattern nodes).
  std::vector<VertexID> pivot;

  // X: preconditions.
  std::vector<Predicate> antecedent;

  // p₀: consequence; std::nullopt makes this a pattern query ψ.
  std::optional<Predicate> consequent;

  // Mining results / provenance, filled by the mining stage.
  RuleQuality quality;

  [[nodiscard]] bool IsQuery() const { return !consequent.has_value(); }

  // Checks consistency between topology, labels, pivot and predicates.
  // Throws std::runtime_error on violation.
  void Validate() const;

  void Print(std::ostream& os) const;
};

}  // namespace gar
