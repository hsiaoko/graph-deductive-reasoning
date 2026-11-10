#include "gar/hdc/encoder.h"

#include <vector>

namespace gar::core::hdc {

namespace {

// Salt separating the tie-break hash stream from symbol streams.
constexpr std::uint64_t kTieBreakSalt = 0x544945425245414Bull;  // "TIEBREAK"

// boost-style hash_combine over the argument ids.
void HashCombine(std::uint64_t& seed, std::uint64_t v) {
  seed ^= v + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

// Per-coordinate majority vote over `ops`, ties broken by the
// corresponding bit of `tie`.
//
// The paper breaks majority ties with a fresh random bit per coordinate.
// We instead read the bit from a pseudorandom vector derived from the
// operands' own ids (hash of the argument symbols, salted per encoder
// seed): the tie bit is ±1 with probability 1/2 independently per
// coordinate, so the expected bundle matches the paper's random
// tie-break, but the choice is a pure function of the bundle's content.
// Two consequences: runs are byte-reproducible, and — crucially — two
// DIFFERENT bundles do not share tie randomness, so no hidden common
// component leaks into every fact (a single global tie vector would
// correlate all bundles at ≈ 1/4 cosine).
Hypervector BundleMajority(const std::vector<Hypervector>& ops,
                           const Hypervector& tie) {
  const std::size_t k = ops.size();
  const std::size_t nw = ops[0].words().size();
  const std::uint32_t dim = ops[0].dim();
  std::vector<std::uint64_t> rw(nw, 0);
  const auto& tw = tie.words();
  if (k == 2) {
    // Majority of 2 ties wherever the operands differ:
    // (a&b) keeps the agreements, tie decides the disagreements.
    const auto& a = ops[0].words();
    const auto& b = ops[1].words();
    for (std::size_t w = 0; w < nw; ++w) {
      rw[w] = (a[w] & b[w]) | (tw[w] & (a[w] ^ b[w]));
    }
    return Hypervector::FromWords(dim, std::move(rw));
  }
  if (k == 3) {
    // Classic majority-of-3 bit trick; 3 operands never tie.
    const auto& a = ops[0].words();
    const auto& b = ops[1].words();
    const auto& c = ops[2].words();
    for (std::size_t w = 0; w < nw; ++w) {
      rw[w] = (a[w] & b[w]) | (a[w] & c[w]) | (b[w] & c[w]);
    }
    return Hypervector::FromWords(dim, std::move(rw));
  }
  // k >= 4: int16 per-coordinate counts, thresholded at k/2, ties broken
  // by `tie`. (Bit-sliced counting would be faster; fact arities here are
  // small, so the plain loop wins on clarity.)
  const std::uint32_t half = static_cast<std::uint32_t>(k) / 2;
  for (std::size_t w = 0; w < nw; ++w) {
    std::uint64_t out = 0;
    for (std::uint32_t b = 0; b < 64; ++b) {
      std::uint32_t cnt = 0;
      for (const auto& op : ops) cnt += (op.words()[w] >> b) & 1;
      const bool bit = cnt > half ? true
                       : cnt < half ? false
                                    : (tw[w] >> b) & 1;
      out |= static_cast<std::uint64_t>(bit) << b;
    }
    rw[w] = out;
  }
  return Hypervector::FromWords(dim, std::move(rw));
}

}  // namespace

Hypervector HdcEncoder::SymbolVector(std::uint64_t symbol_id) const {
  return Hypervector::FromHash(cfg_.seed ^ symbol_id, cfg_.dim);
}

Hypervector HdcEncoder::EncodeFact(std::uint64_t pred_symbol,
                                   const std::uint64_t* arg_symbols,
                                   std::uint32_t arity) const {
  const Hypervector vp = SymbolVector(pred_symbol);
  if (arity == 0) return vp;

  // ρ^i(v_{a_i}): argument i permuted by i positions (0-based, so the
  // first argument is unshifted).
  std::vector<Hypervector> ops;
  ops.reserve(arity);
  for (std::uint32_t i = 0; i < arity; ++i) {
    ops.push_back(
        Hypervector::Permute(SymbolVector(arg_symbols[i]), i));
  }
  // Deterministic, content-derived tie-break vector: hash-combine the
  // argument ids (plus the encoder seed) into the tie stream seed. Same
  // operands => same tie vector (same bundle); different operands =>
  // independent tie vector (no cross-bundle leakage).
  std::uint64_t tie_seed = cfg_.seed ^ kTieBreakSalt;
  for (std::uint32_t i = 0; i < arity; ++i) {
    HashCombine(tie_seed, arg_symbols[i]);
  }
  const Hypervector tie = Hypervector::FromHash(tie_seed, cfg_.dim);
  return Hypervector::Bind(vp, BundleMajority(ops, tie));
}

}  // namespace gar::core::hdc
