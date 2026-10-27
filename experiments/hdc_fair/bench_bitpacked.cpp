// Bit-packed binary-HDC scoring vs exact hash witnessing.
//   candidate score = Hamming(maj(P premise vectors), E_packed) -> cosine
//   E_packed        = bitwise majority over witness vectors (counter planes)
// Paths: scalar / auto-vec (same source, -O2 vs -O3 -march=native builds) /
//        hand AVX-512 (512-bit xor/and planes + shuffle popcount; the
//        machine has AVX512BW but no VPOPCNTDQ).
// Semantics note: binary majority with a deterministic tie vector; the int16
// accumulator path (previous round) differs only in tie-breaking and the
// counting floor — quality impact measured below.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <unordered_set>
#include <vector>
#include <immintrin.h>

#include "gar/hdc/encoder.h"
#include "gar/hdc/evidence.h"

using gar::hdc::EvidenceVector;
using gar::hdc::HdcEncoder;
using gar::hdc::Hypervector;

namespace {

constexpr int kMaxPlanes = 6;   // candidate bundles (P <= 32)
constexpr int kEvPlanes = 14;   // evidence bundles (m <= 16384)

double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct SimFact { std::uint64_t pred, a0, a1; };
std::uint64_t FactKey(const SimFact& f) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
  mix(f.pred); mix(f.a0); mix(f.a1);
  return h;
}

// ---- scalar bit-plane majority ------------------------------------------

void PlanesClear(std::vector<std::uint64_t>& pl, std::size_t W) {
  std::memset(pl.data(), 0, pl.size() * 8);
  (void)W;
}

// add one binary vector (W words) to the per-coordinate counters
void PlanesAdd(std::uint64_t* pl, const std::uint64_t* v, std::size_t W,
               int nplanes) {
  for (std::size_t w = 0; w < W; ++w) {
    std::uint64_t c = v[w];
    for (int j = 0; j < nplanes && (c || j == 0); ++j) {
      const std::uint64_t nc = pl[j * W + w] & c;
      pl[j * W + w] ^= c;
      c = nc;
    }
  }
}

// majority readout: count > k/2 per coordinate; exact half -> tie vector
void PlanesMajority(const std::uint64_t* pl, std::uint64_t k, std::size_t W,
                    int nplanes, const std::uint64_t* tie,
                    std::uint64_t* out) {
  const std::uint64_t t = k / 2;
  for (std::size_t w = 0; w < W; ++w) {
    std::uint64_t gt = 0, eq = ~0ull;
    for (int j = nplanes - 1; j >= 0; --j) {
      const std::uint64_t pj = pl[j * W + w];
      if (!((t >> j) & 1)) gt |= eq & pj;
      eq &= ((t >> j) & 1) ? pj : ~pj;
    }
    out[w] = gt | ((k % 2 == 0) ? (eq & tie[w]) : 0);
  }
}

std::uint64_t HammingScalar(const std::uint64_t* a, const std::uint64_t* b,
                            std::size_t W) {
  std::uint64_t h = 0;
  for (std::size_t w = 0; w < W; ++w) h += __builtin_popcountll(a[w] ^ b[w]);
  return h;
}

#if defined(__AVX512F__) && defined(__AVX512BW__)
#define HAVE_AVX512 1
// ---- AVX-512 versions -----------------------------------------------------

void PlanesAdd512(std::uint64_t* pl, const std::uint64_t* v, std::size_t W,
                  int nplanes) {
  std::size_t w = 0;
  for (; w + 8 <= W; w += 8) {
    __m512i c = _mm512_loadu_si512(v + w);
    for (int j = 0; j < nplanes; ++j) {
      __m512i p = _mm512_loadu_si512(pl + j * W + w);
      __m512i nc = _mm512_and_si512(p, c);
      _mm512_storeu_si512(pl + j * W + w, _mm512_xor_si512(p, c));
      c = nc;
      if (_mm512_test_epi64_mask(c, c) == 0) break;
    }
  }
  for (; w < W; ++w) {
    std::uint64_t c = v[w];
    for (int j = 0; j < nplanes && (c || j == 0); ++j) {
      const std::uint64_t nc = pl[j * W + w] & c;
      pl[j * W + w] ^= c;
      c = nc;
    }
  }
}

std::uint64_t Hamming512(const std::uint64_t* a, const std::uint64_t* b,
                         std::size_t W) {
  // Mula nibble-LUT popcount in 512-bit lanes (no VPOPCNTDQ on this CPU)
  const __m512i lut = _mm512_set1_epi64(0x0302020102010100ull);
  const __m512i low = _mm512_set1_epi64(0x0F0F0F0F0F0F0F0Full);
  __m512i acc = _mm512_setzero_si512();
  std::size_t w = 0;
  for (; w + 8 <= W; w += 8) {
    const __m512i x = _mm512_xor_si512(_mm512_loadu_si512(a + w),
                                       _mm512_loadu_si512(b + w));
    const __m512i lo = _mm512_and_si512(x, low);
    const __m512i hi = _mm512_and_si512(_mm512_srli_epi64(x, 4), low);
    const __m512i pc = _mm512_add_epi8(_mm512_shuffle_epi8(lut, lo),
                                       _mm512_shuffle_epi8(lut, hi));
    acc = _mm512_add_epi64(acc, _mm512_sad_epu8(pc, _mm512_setzero_si512()));
  }
  std::uint64_t h = _mm512_reduce_add_epi64(acc);
  for (; w < W; ++w) h += __builtin_popcountll(a[w] ^ b[w]);
  return h;
}
#else
#define HAVE_AVX512 0
void PlanesAdd512(std::uint64_t*, const std::uint64_t*, std::size_t, int) {}
std::uint64_t Hamming512(const std::uint64_t*, const std::uint64_t*, std::size_t) { return 0; }
#endif

}  // namespace

int main(int argc, char** argv) {
  const bool use512 = HAVE_AVX512 && argc > 1 && std::string(argv[1]) == "avx512";
  const std::uint32_t Pmax = 32;
  std::mt19937_64 rng(42);
  volatile std::uint64_t sink = 0;

  std::printf("# mode=%s\n", use512 ? "avx512" : "scalar");

  for (const std::uint32_t D : {512u, 1024u, 8192u}) {
    const std::size_t W = D / 64;
    for (const std::uint64_t m : {32ul, 128ul, 2048ul, 8192ul}) {
      // witness facts + E maintenance (both paths), timed separately
      std::vector<SimFact> wit(m);
      for (auto& f : wit) f = {rng() % 60 + 1000, rng() % 60000, rng() % 60000};
      HdcEncoder enc({D, 7});
      std::vector<Hypervector> wv;
      wv.reserve(m);
      for (const auto& f : wit) {
        const std::uint64_t args[2] = {f.a0, f.a1};
        wv.push_back(enc.EncodeFact(f.pred, args, 2));
      }
      // E maintenance: bit planes
      double t0 = Now();
      std::vector<std::uint64_t> epl(kEvPlanes * W, 0);
      for (const auto& v : wv) {
        if (use512) PlanesAdd512(epl.data(), v.words().data(), W, kEvPlanes);
        else PlanesAdd(epl.data(), v.words().data(), W, kEvPlanes);
      }
      const Hypervector tie = Hypervector::FromHash(0x71E, D);
      std::vector<std::uint64_t> ep(W);
      PlanesMajority(epl.data(), m, W, kEvPlanes, tie.words().data(),
                     ep.data());
      const double e_maint_bit_ms = (Now() - t0) * 1e3;
      // E maintenance: int16 accumulator + Packed (current impl)
      t0 = Now();
      EvidenceVector E16(D);
      for (const auto& v : wv) E16.Witness(v);
      const Hypervector ep16 = E16.Packed();
      const double e_maint_i16_ms = (Now() - t0) * 1e3;
      // sanity: the two E packings should agree on most coords (ties aside)
      std::uint64_t agree_bits =
          D - HammingScalar(ep.data(), ep16.words().data(), W);

      const std::uint64_t C = 10000;
      std::vector<std::vector<SimFact>> cands(C);
      for (auto& c : cands)
        for (std::uint32_t i = 0; i < Pmax; ++i) {
          if (rng() % 10 < 3) c.push_back(wit[rng() % m]);
          else c.push_back({rng() % 60 + 1000, rng() % 60000, rng() % 60000});
        }
      // pre-encode candidate premise facts (same as the memoized system:
      // encoding is cached per distinct fact; scoring cost excludes it)
      std::vector<std::vector<Hypervector>> cvec(C);
      for (std::size_t i = 0; i < C; ++i)
        for (const auto& f : cands[i]) {
          const std::uint64_t args[2] = {f.a0, f.a1};
          cvec[i].push_back(enc.EncodeFact(f.pred, args, 2));
        }
      std::unordered_set<std::uint64_t> ws;
      for (const auto& f : wit) ws.insert(FactKey(f));

      for (const std::uint32_t P : {2u, 4u, 8u, 16u, 32u}) {
        // exact: P hash lookups
        double best_x = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
          t0 = Now();
          for (const auto& c : cands) {
            std::uint64_t h = 0;
            for (std::uint32_t i = 0; i < P; ++i) h += ws.count(FactKey(c[i]));
            sink = sink + h;
          }
          best_x = std::min(best_x, (Now() - t0) / C * 1e9);
        }
        // bit path: planes per candidate + hamming vs ep
        double best_b = 1e30;
        std::vector<std::uint64_t> pl(kMaxPlanes * W);
        std::vector<std::uint64_t> vm(W);
        for (int rep = 0; rep < 5; ++rep) {
          t0 = Now();
          for (std::size_t i = 0; i < C; ++i) {
            PlanesClear(pl, W);
            for (std::uint32_t j = 0; j < P; ++j) {
              if (use512) PlanesAdd512(pl.data(), cvec[i][j].words().data(), W, kMaxPlanes);
              else PlanesAdd(pl.data(), cvec[i][j].words().data(), W, kMaxPlanes);
            }
            PlanesMajority(pl.data(), P, W, kMaxPlanes, tie.words().data(), vm.data());
            const std::uint64_t h = use512 ? Hamming512(vm.data(), ep.data(), W)
                                           : HammingScalar(vm.data(), ep.data(), W);
            sink = sink + h;
          }
          best_b = std::min(best_b, (Now() - t0) / C * 1e9);
        }
        std::printf("D=%-5u m=%-5lu P=%-2u exact=%8.1fns bit=%8.1fns ratio=%8.2f\n",
                    D, m, P, best_x, best_b, best_b / best_x);
      }
      std::printf("D=%-5u m=%-5lu E-maint: bitplanes=%.2fms int16=%.2fms "
                  "(amortized@10k cands: %.1fns / %.1fns per cand) Epack agree=%.3f\n",
                  D, m, e_maint_bit_ms, e_maint_i16_ms,
                  e_maint_bit_ms * 1e6 / C, e_maint_i16_ms * 1e6 / C,
                  agree_bits / double(D));
      std::fflush(stdout);
    }
  }
  return 0;
}
