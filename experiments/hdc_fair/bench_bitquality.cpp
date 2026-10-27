// Quality: binary-majority (bit-planes) vs int16-accumulator scoring, both
// vs exact witnessed-share. Same generation as bench_scoring.cpp (seed 42,
// P=4, 30% premises witnessed, C=10k).
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <unordered_set>
#include <vector>

#include "gar/hdc/encoder.h"
#include "gar/hdc/evidence.h"

using gar::hdc::EvidenceVector;
using gar::hdc::HdcEncoder;
using gar::hdc::Hypervector;

namespace {
constexpr int kEvPlanes = 14;
constexpr int kCandPlanes = 6;
struct SimFact { std::uint64_t pred, a0, a1; };
std::uint64_t FactKey(const SimFact& f) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
  mix(f.pred); mix(f.a0); mix(f.a1);
  return h;
}
void PlanesAdd(std::uint64_t* pl, const std::uint64_t* v, std::size_t W, int np) {
  for (std::size_t w = 0; w < W; ++w) {
    std::uint64_t c = v[w];
    for (int j = 0; j < np && (c || j == 0); ++j) {
      const std::uint64_t nc = pl[j * W + w] & c;
      pl[j * W + w] ^= c;
      c = nc;
    }
  }
}
void PlanesMajority(const std::uint64_t* pl, std::uint64_t k, std::size_t W,
                    int np, const std::uint64_t* tie, std::uint64_t* out) {
  const std::uint64_t t = k / 2;
  for (std::size_t w = 0; w < W; ++w) {
    std::uint64_t gt = 0, eq = ~0ull;
    for (int j = np - 1; j >= 0; --j) {
      const std::uint64_t pj = pl[j * W + w];
      if (!((t >> j) & 1)) gt |= eq & pj;
      eq &= ((t >> j) & 1) ? pj : ~pj;
    }
    out[w] = gt | ((k % 2 == 0) ? (eq & tie[w]) : 0);
  }
}
double KendallTau(const std::vector<double>& a, const std::vector<double>& b) {
  const std::size_t n = a.size(), cap = std::min<std::size_t>(n, 2000);
  const std::size_t stride = std::max<std::size_t>(1, n / cap);
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < n && idx.size() < cap; i += stride) idx.push_back(i);
  std::int64_t conc = 0, disc = 0;
  for (std::size_t i = 0; i < idx.size(); ++i)
    for (std::size_t j = i + 1; j < idx.size(); ++j) {
      const double s = (a[idx[i]] - a[idx[j]]) * (b[idx[i]] - b[idx[j]]);
      if (s > 0) ++conc; else if (s < 0) ++disc;
    }
  const double d = double(conc + disc);
  return d == 0 ? 1.0 : double(conc - disc) / d;
}
}  // namespace

int main() {
  const std::uint32_t P = 4;
  std::mt19937_64 rng(42);
  std::printf("%-6s %-6s %-8s %-8s %-8s %-8s %-8s %-8s\n", "D", "m",
              "tau_bit", "tau_i16", "top1_bit", "top1_i16", "bitvs16", "bound");
  for (const std::uint32_t D : {512u, 8192u}) {
    const std::size_t W = D / 64;
    for (const std::uint64_t m : {8ul, 32ul, 128ul, 512ul, 2048ul, 8192ul}) {
      std::vector<SimFact> wit(m);
      for (auto& f : wit) f = {rng() % 60 + 1000, rng() % 60000, rng() % 60000};
      HdcEncoder enc({D, 7});
      std::vector<Hypervector> wv;
      for (const auto& f : wit) {
        const std::uint64_t args[2] = {f.a0, f.a1};
        wv.push_back(enc.EncodeFact(f.pred, args, 2));
      }
      std::unordered_set<std::uint64_t> ws;
      for (const auto& f : wit) ws.insert(FactKey(f));
      // E: int16 + planes
      EvidenceVector E16(D);
      for (const auto& v : wv) E16.Witness(v);
      const Hypervector ep16 = E16.Packed();
      std::vector<std::uint64_t> epl(kEvPlanes * W, 0);
      for (const auto& v : wv) PlanesAdd(epl.data(), v.words().data(), W, kEvPlanes);
      const Hypervector tie = Hypervector::FromHash(0x71E, D);
      std::vector<std::uint64_t> epb(W);
      PlanesMajority(epl.data(), m, W, kEvPlanes, tie.words().data(), epb.data());
      // candidates
      const std::uint64_t C = 10000;
      std::vector<double> e_exact, e_i16, e_bit;
      std::vector<std::uint64_t> cpl(kCandPlanes * W);
      std::vector<std::uint64_t> vmb(W);
      for (std::uint64_t ci = 0; ci < C; ++ci) {
        SimFact c[P];
        for (auto& f : c)
          if (rng() % 10 < 3) f = wit[rng() % m];
          else f = {rng() % 60 + 1000, rng() % 60000, rng() % 60000};
        std::uint64_t h = 0;
        EvidenceVector vm16(D);
        std::memset(cpl.data(), 0, cpl.size() * 8);
        for (const auto& f : c) {
          h += ws.count(FactKey(f));
          const std::uint64_t args[2] = {f.a0, f.a1};
          const Hypervector v = enc.EncodeFact(f.pred, args, 2);
          vm16.Witness(v);
          PlanesAdd(cpl.data(), v.words().data(), W, kCandPlanes);
        }
        e_exact.push_back(double(h) / P);
        e_i16.push_back(Hypervector::Cosine(ep16, vm16.Packed()));
        PlanesMajority(cpl.data(), P, W, kCandPlanes, tie.words().data(), vmb.data());
        std::uint64_t hd = 0;
        for (std::size_t w = 0; w < W; ++w) hd += __builtin_popcountll(vmb[w] ^ epb[w]);
        e_bit.push_back(1.0 - 2.0 * hd / double(D));
      }
      const std::size_t t16 = std::max_element(e_i16.begin(), e_i16.end()) - e_i16.begin();
      const std::size_t tbit = std::max_element(e_bit.begin(), e_bit.end()) - e_bit.begin();
      const std::size_t tex = std::max_element(e_exact.begin(), e_exact.end()) - e_exact.begin();
      const bool top1_bit = e_exact[tbit] == e_exact[tex];
      const bool top1_16 = e_exact[t16] == e_exact[tex];
      const bool in_bound = (m + P) * (m + P) <= D;
      std::printf("%-6u %-6lu %-8.3f %-8.3f %-8d %-8d %-8.3f %-8s\n", D, m,
                  KendallTau(e_exact, e_bit), KendallTau(e_exact, e_i16),
                  int(top1_bit), int(top1_16), KendallTau(e_bit, e_i16),
                  in_bound ? "in" : "OUT");
    }
  }
  return 0;
}
