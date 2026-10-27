#include "gar/engine/agent/witness.h"

#include "gar/analysis/predicate_table.h"

namespace gar {
namespace engine {
namespace agent {

namespace {

std::uint64_t Fnv1a64(const std::string& s) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  for (const unsigned char c : s) {
    h = (h ^ c) * 0x100000001B3ull;
  }
  return h;
}

}  // namespace

WitnessLog::WitnessLog(std::uint32_t hdc_dim, const graph::TripleGraph* g,
                       const analysis::PredicateTable* table)
    : hdc_ev_(hdc_dim), packed_cache_(hdc_dim), g_(g), table_(table) {}

std::uint64_t FactPredicateSymbol(const Fact& f, const graph::TripleGraph& g,
                                  const analysis::PredicateTable& table) {
  const std::string& name = f.kind == FactKind::kEdge
                                ? g.RelationName(f.pred)
                                : table.Name(f.pred);
  std::uint64_t s = Fnv1a64(name);
  return hdc::SplitMix64Next(s) | (1ull << 63);
}

void WitnessLog::WitnessFact(const Fact& f, const hdc::HdcEncoder& enc) {
  exact_.insert(KeyOf(f));
  const std::uint64_t args[2] = {f.args[0], f.args[1]};
  hdc_ev_.Witness(
      enc.EncodeFact(FactPredicateSymbol(f, *g_, *table_), args, f.arity));
  packed_dirty_ = true;
  ++count_;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
