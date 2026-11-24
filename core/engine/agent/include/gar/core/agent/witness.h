#pragma once

// WitnessLog: chain-level witness record (paper §4.1.1). Two
// implementations are updated in lockstep:
//   * exact:   a hash set of the witnessed facts (for ExactScorer)
//   * hdc_ev:  the facts bundled into an HDC evidence vector (HdcScorer)
// Witnessed = the pattern facts a chain's applications matched, successful
// or failed ("a match computed is information gained"), plus the evaluated
// antecedent premises and the facts successful applications derived.

#include <cstdint>
#include <unordered_set>

#include "gar/core/agent/fact.h"
#include "gar/core/agent/fact_encoder.h"
#include "gar/core/graph/graph.h"
#include "gar/core/hdc/encoder.h"
#include "gar/core/hdc/evidence.h"

namespace gar::core::analysis {
class PredicateTable;
}  // namespace analysis

namespace gar::core::engine {
namespace agent {

class WitnessLog {
 public:
  // `use_hdc` gates the HDC evidence accumulation: ExactScorer/NullScorer
  // never read the evidence vector, so maintaining the 8192-dim bundle for
  // them is pure cost. The exact set is always maintained (ExactScorer's
  // witnessed-premise share and failure accounting read it). HdcScorer
  // passes true and gets bit-identical HDC numerics.
  WitnessLog(std::uint32_t hdc_dim, const gar::core::graph::Graph* g,
             const gar::core::analysis::PredicateTable* table, bool use_hdc = true);

  // Adds only a new set member to HDC evidence and returns whether it was new.
  bool WitnessFact(const Fact& f, const gar::core::hdc::HdcEncoder& enc);

  [[nodiscard]] bool Contains(const Fact& f) const {
    return exact_.count(KeyOf(f)) != 0;
  }
  [[nodiscard]] std::uint64_t Count() const { return count_; }
  [[nodiscard]] const gar::core::hdc::EvidenceVector& hdc() const { return hdc_ev_; }

 private:
  std::unordered_set<FactKey, FactKeyHash> exact_;
  gar::core::hdc::EvidenceVector hdc_ev_;
  std::uint64_t count_ = 0;
  const gar::core::graph::Graph* g_;
  const gar::core::analysis::PredicateTable* table_;
  bool use_hdc_ = true;
};

}  // namespace agent
}  // namespace gar::core::engine
