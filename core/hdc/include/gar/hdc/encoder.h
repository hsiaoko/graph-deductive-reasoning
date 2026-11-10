#pragma once

// HdcEncoder: symbol/fact encoding for the HDC scorer (paper §4.1.1 /
// App I). Every symbol gets a random bipolar vector generated ON THE FLY
// from its identifier by hashing (seed ^ symbol_id into a splitmix64
// stream) — nothing is materialized or cached per symbol (App I:
// "generated on the fly from identifiers by hashing, nothing is
// materialized per vertex").
//
// Fact encoding:  v_f = v_p ⊕ bundle_i( ρ^i(v_{a_i}) )
//   v_p        predicate symbol vector
//   ρ^i        i-fold cyclic left shift (positional encoding of argument i)
//   bundle     per-coordinate majority vote, ties broken deterministically
//   ⊕          binding (XOR)

#include <cstdint>

#include "gar/hdc/const.h"
#include "gar/hdc/hypervector.h"

namespace gar::core::hdc {

class HdcEncoder {
 public:
  struct Config {
    std::uint32_t dim = kHdcDefaultDim;
    std::uint64_t seed = 0;
  };

  explicit HdcEncoder(Config cfg) : cfg_(cfg) {}

  // Random bipolar vector of a symbol, hashed on the fly; the same
  // (seed, symbol_id) always yields the same vector.
  [[nodiscard]] Hypervector SymbolVector(std::uint64_t symbol_id) const;

  // v_p ⊕ bundle_i(ρ^i(v_{a_i})). arity 0 degenerates to the bare
  // predicate vector (bundle of nothing is the all-zero vector, and
  // Bind(v, 0) == v).
  [[nodiscard]] Hypervector EncodeFact(std::uint64_t pred_symbol,
                                       const std::uint64_t* arg_symbols,
                                       std::uint32_t arity) const;

  [[nodiscard]] std::uint32_t dim() const { return cfg_.dim; }
  [[nodiscard]] std::uint64_t seed() const { return cfg_.seed; }

 private:
  Config cfg_;
};

}  // namespace gar::core::hdc
