// HDC vs exact scoring microbenchmark + approximation quality.
// Standalone on gar::hdc (no graph deps). See experiments/hdc_fair/NOTES.md.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gar/hdc/encoder.h"
#include "gar/hdc/evidence.h"

using gar::hdc::EvidenceVector;
using gar::hdc::HdcEncoder;
using gar::hdc::Hypervector;

namespace {

struct SimFact {  // arity-2 fact: predicate symbol + two argument symbols
  std::uint64_t pred, a0, a1;
};

std::uint64_t FactKey(const SimFact& f) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](std::uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ull;
  };
  mix(f.pred);
  mix(f.a0);
  mix(f.a1);
  return h;
}

double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------- scoring paths (mirror HdcScorer semantics) -------------

// exact: hash lookups
struct ExactPath {
  std::unordered_set<std::uint64_t> ws;
  double Score(const SimFact*, std::uint32_t, double, std::uint32_t* h) const {
    *h = 0;
    return 0.0;
  }
  void Witness(const SimFact& f) { ws.insert(FactKey(f)); }
  double Evd(const SimFact* p, std::uint32_t n) const {
    std::uint64_t h = 0;
    for (std::uint32_t i = 0; i < n; ++i) h += ws.count(FactKey(p[i]));
    return n ? static_cast<double>(h) / n : 0.0;
  }
};

// hdc current: rebuild Packed(E) per cosine; encode each fact twice
struct HdcCurrent {
  EvidenceVector E;
  const HdcEncoder& enc;
  explicit HdcCurrent(std::uint32_t dim, const HdcEncoder& e)
      : E(dim), enc(e) {}
  void Witness(const SimFact& f) {
    const std::uint64_t args[2] = {f.a0, f.a1};
    E.Witness(enc.EncodeFact(f.pred, args, 2));
  }
  double Evd(const SimFact*, std::uint32_t) const { return 0.0; }
  double Score(const SimFact* p, std::uint32_t n, double tau,
               std::uint32_t* hits) const {
    EvidenceVector v_mu(E.Dim());
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint64_t args[2] = {p[i].a0, p[i].a1};
      v_mu.Witness(enc.EncodeFact(p[i].pred, args, 2));
    }
    const double evd = E.Cosine(v_mu.Packed());
    std::uint32_t h = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint64_t args[2] = {p[i].a0, p[i].a1};
      h += E.Cosine(enc.EncodeFact(p[i].pred, args, 2)) >= tau;
    }
    *hits = h;
    return evd;
  }
};

// hdc optimized: Packed(E) cached; fact vectors cached; encode once
struct HdcOpt {
  EvidenceVector E;
  const HdcEncoder& enc;
  Hypervector packedE;
  bool dirty = true;
  std::unordered_map<std::uint64_t, Hypervector> fact_cache;
  std::uint64_t cache_hits = 0;
  explicit HdcOpt(std::uint32_t dim, const HdcEncoder& e)
      : E(dim), enc(e), packedE(dim) {}
  const Hypervector& Encode(const SimFact& f) {
    const std::uint64_t key = FactKey(f);
    const auto it = fact_cache.find(key);
    if (it != fact_cache.end()) {
      ++cache_hits;
      return it->second;
    }
    const std::uint64_t args[2] = {f.a0, f.a1};
    return fact_cache.emplace(key, enc.EncodeFact(f.pred, args, 2))
        .first->second;
  }
  void Witness(const SimFact& f) {
    E.Witness(Encode(f));
    dirty = true;
  }
  double Evd(const SimFact*, std::uint32_t) const { return 0.0; }
  const Hypervector& PackedE() {
    if (dirty) {
      packedE = E.Packed();
      dirty = false;
    }
    return packedE;
  }
  double Score(const SimFact* p, std::uint32_t n, double tau,
               std::uint32_t* hits) {
    const Hypervector& pe = PackedE();
    EvidenceVector v_mu(E.Dim());
    for (std::uint32_t i = 0; i < n; ++i) v_mu.Witness(Encode(p[i]));
    const double evd = Hypervector::Cosine(pe, v_mu.Packed());
    std::uint32_t h = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      h += Hypervector::Cosine(pe, Encode(p[i])) >= tau;
    }
    *hits = h;
    return evd;
  }
};

// Kendall tau-b (subsampled)
double KendallTau(const std::vector<double>& a, const std::vector<double>& b,
                  std::uint64_t seed) {
  (void)seed;
  const std::size_t n = a.size();
  const std::size_t cap = std::min<std::size_t>(n, 2000);
  const std::size_t stride = std::max<std::size_t>(1, n / cap);
  std::int64_t conc = 0, disc = 0;
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < n && idx.size() < cap; i += stride) idx.push_back(i);
  const std::size_t k = idx.size();
  for (std::size_t i = 0; i < k; ++i) {
    for (std::size_t j = i + 1; j < k; ++j) {
      const double da = a[idx[i]] - a[idx[j]], db = b[idx[i]] - b[idx[j]];
      const double s = da * db;
      if (s > 0) ++conc;
      else if (s < 0) ++disc;
    }
  }
  const double denom = static_cast<double>(conc + disc);
  return denom == 0 ? 1.0 : static_cast<double>(conc - disc) / denom;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint32_t D = argc > 1 ? std::stoul(argv[1]) : 8192;
  const std::uint32_t P = 4;           // premises per candidate
  const double tau = 0.15;
  std::mt19937_64 rng(42);

  std::printf("# dim=%u premises/cand=%u tau=%.2f\n", D, P, tau);
  std::printf("%-7s %-8s %-12s %-12s %-12s %-10s %-10s %-8s %-8s %-9s\n",
              "bundle", "cands", "exact_us", "hdcCur_us", "hdcOpt_us",
              "speedup", "curSpd", "tau_evd", "top1", "hit_agree");

  for (const std::uint64_t m : {8ul, 32ul, 128ul, 512ul, 2048ul, 8192ul}) {
    for (const std::uint64_t C : {1000ul, 10000ul, 100000ul}) {
      // witness facts: pred from 60 symbols, args from 60k entities
      std::vector<SimFact> wit(m);
      for (auto& f : wit) {
        f = {rng() % 60 + 1000, rng() % 60000, rng() % 60000};
      }
      // candidates: 30% premises drawn from the witness set (hits exist)
      std::vector<std::vector<SimFact>> cands(C, std::vector<SimFact>(P));
      for (auto& c : cands) {
        for (auto& f : c) {
          if (rng() % 10 < 3) {
            f = wit[rng() % m];
          } else {
            f = {rng() % 60 + 1000, rng() % 60000, rng() % 60000};
          }
        }
      }
      std::fprintf(stderr, "[cell m=%lu C=%lu] gen\n", m, C);
      HdcEncoder enc({D, 7});
      ExactPath xp;
      HdcCurrent hc(D, enc);
      HdcOpt ho(D, enc);
      for (const auto& f : wit) {
        xp.Witness(f);
        hc.Witness(f);
        ho.Witness(f);
      }
      // timing: choose repeats so each cell ~0.2-2s
      volatile std::uint64_t sink = 0;
      auto time_path = [&](auto& path, bool is_exact) {
        std::uint64_t reps = 1;
        double t = 0;
        for (;;) {
          const double t0 = Now();
          for (std::uint64_t r = 0; r < reps; ++r) {
            for (const auto& c : cands) {
              if (is_exact) {
                sink = sink + std::uint64_t(1e9 * path.Evd(c.data(), P));
              } else {
                std::uint32_t h;
                sink = sink + std::uint64_t(1e9 * path.Score(c.data(), P, tau, &h)) + h;
              }
            }
          }
          t = (Now() - t0) / reps;
          if (t * reps > 0.25 || reps > (1ull << 26)) break;
          reps *= 4;
        }
        return t / C * 1e6;  // us per candidate
      };
      std::fprintf(stderr, "[cell m=%lu C=%lu] time exact\n", m, C);
      const double t_exact = time_path(xp, true);
      std::fprintf(stderr, "[cell m=%lu C=%lu] time cur\n", m, C);
      const double t_cur = time_path(hc, false);
      const double t_opt = time_path(ho, false);
      std::fprintf(stderr, "[cell m=%lu C=%lu] quality\n", m, C);
      // quality: exact evd vs hdc-opt evd (+ hit agreement)
      std::vector<double> e_evd, h_evd;
      std::uint64_t hit_eq = 0;
      for (const auto& c : cands) {
        e_evd.push_back(xp.Evd(c.data(), P));
        std::uint32_t hh;
        const double e = ho.Score(c.data(), P, tau, &hh);
        h_evd.push_back(e);
        std::uint32_t he = 0;
        for (const auto& f : c) he += xp.ws.count(FactKey(f));
        hit_eq += (hh == he);
      }
      const double kt = KendallTau(e_evd, h_evd, 5);
      const std::size_t te =
          std::max_element(e_evd.begin(), e_evd.end()) - e_evd.begin();
      const std::size_t th =
          std::max_element(h_evd.begin(), h_evd.end()) - h_evd.begin();
      const bool top1 = te == th || e_evd[te] == e_evd[th];
      std::printf("%-7lu %-8lu %-12.3f %-12.3f %-12.3f %-10.2f %-10.2f %-8.3f %-8d %-9.3f\n",
                  m, C, t_exact, t_cur, t_opt, t_exact / t_opt, t_cur / t_opt,
                  kt, static_cast<int>(top1), hit_eq / double(C));
      std::fflush(stdout);
    }
  }
  return 0;
}
