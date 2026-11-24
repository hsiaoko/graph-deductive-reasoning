#include "gar/core/agent/witness.h"

#include "gar/core/analysis/predicate_table.h"

namespace gar::core::engine {
namespace agent {

WitnessLog::WitnessLog(std::uint32_t hdc_dim, const gar::core::graph::Graph* g,
                       const gar::core::analysis::PredicateTable* table, bool use_hdc)
    : hdc_ev_(hdc_dim),
      g_(g),
      table_(table),
      use_hdc_(use_hdc) {}

bool WitnessLog::WitnessFact(const Fact& f, const gar::core::hdc::HdcEncoder& enc) {
  const bool inserted = exact_.insert(KeyOf(f)).second;
  if (!inserted) return false;
  if (use_hdc_) {
    const std::uint64_t args[2] = {f.args[0], f.args[1]};
    hdc_ev_.Witness(enc.EncodeFactDense(
        FactPredicateSymbol(f, *g_, *table_), args, f.arity));
  }
  ++count_;
  return true;
}

}  // namespace agent
}  // namespace gar::core::engine
