#pragma once

// WitnessLog: chain-level witness record (paper §4.1.1). Two
// implementations are updated in lockstep:
//   * exact:   a hash set of the witnessed facts (for ExactScorer)
//   * hdc_ev:  the facts bundled into an HDC evidence vector (HdcScorer)
// Witnessed = the pattern facts a chain's applications matched, successful
// or failed ("a match computed is information gained"), plus the evaluated
// antecedent premises and the facts successful applications derived.
//
// Fact -> SymbolId mapping lives HERE (the hdc module stays dependency-free
// and sees bare uint64 symbols):
//   * predicate symbol = splitmix64(FNV-1a(name)) with bit 63 set, the name
//     being the relation name (edge facts) resp. the predicate-table name
//     (attr/ml facts);
//   * vertex symbol    = the vertex id itself.
// Bit 63 namespaces predicates away from vertex ids (vertex counts are far
// below 2^63).

#include <cstdint>
#include <unordered_set>

#include "gar/engine/agent/fact.h"
#include "gar/graph/triple_graph.h"
#include "gar/hdc/encoder.h"
#include "gar/hdc/evidence.h"

namespace gar::core::analysis {
class PredicateTable;
}  // namespace analysis

namespace gar::core::engine {
namespace agent {

// Name-based predicate symbol, namespaced by bit 63 (see header comment):
// splitmix64(FNV-1a(name)) | (1<<63), where the name is the relation name
// for edge facts resp. the predicate-table name for attr/ml facts.
std::uint64_t FactPredicateSymbol(const Fact& f, const gar::core::graph::TripleGraph& g,
                                  const gar::core::analysis::PredicateTable& table);

class WitnessLog {
 public:
  // `use_hdc` gates the HDC evidence accumulation: ExactScorer/NullScorer
  // never read the evidence vector, so maintaining the 8192-dim bundle for
  // them is pure cost. The exact set is always maintained (ExactScorer's
  // witnessed-premise share and failure accounting read it). HdcScorer
  // passes true and gets bit-identical HDC numerics.
  WitnessLog(std::uint32_t hdc_dim, const gar::core::graph::TripleGraph* g,
             const gar::core::analysis::PredicateTable* table, bool use_hdc = true);

  // Returns true iff the fact is new to the exact witness set. HDC evidence
  // still accumulates every event, including an already witnessed fact.
  bool WitnessFact(const Fact& f, const gar::core::hdc::HdcEncoder& enc);

  [[nodiscard]] bool Contains(const Fact& f) const {
    return exact_.count(KeyOf(f)) != 0;
  }
  [[nodiscard]] std::uint64_t Count() const { return count_; }
  [[nodiscard]] const gar::core::hdc::EvidenceVector& hdc() const { return hdc_ev_; }
  // Sign-packed majority of the evidence bundle, cached until the next
  // witnessed fact. Scoring does several cosine readouts per candidate;
  // re-packing per readout is pure waste — identical value by construction
  // (Packed is a pure function of the accumulator).
  [[nodiscard]] const gar::core::hdc::Hypervector& PackedHdc() const {
    if (packed_dirty_) {
      packed_cache_ = hdc_ev_.Packed();
      packed_dirty_ = false;
    }
    return packed_cache_;
  }

 private:
  std::unordered_set<FactKey, FactKeyHash> exact_;
  gar::core::hdc::EvidenceVector hdc_ev_;
  mutable gar::core::hdc::Hypervector packed_cache_;
  mutable bool packed_dirty_ = true;
  std::uint64_t count_ = 0;
  const gar::core::graph::TripleGraph* g_;
  const gar::core::analysis::PredicateTable* table_;
  bool use_hdc_ = true;
};

}  // namespace agent
}  // namespace gar::core::engine
