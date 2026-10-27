// Phase 4 prototype: does HDC similarity between subgoals predict outcome
// agreement? Subgoal = (relation, src, dst); outcome = derivable (in the
// held-out chase closure) or not. Encoded with the exact HdcEncoder
// semantics used by the agent (pred symbol = splitmix(FNV(name))|1<<63,
// vertex symbol = dense entity id, arity 2).
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gar/hdc/encoder.h"
#include "gar/hdc/hypervector.h"

namespace {
std::uint64_t Fnv1a64(const std::string& s) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  for (const unsigned char c : s) { h = (h ^ c) * 0x100000001B3ull; }
  return h;
}
}

int main(int argc, char** argv) {
  const char* closure = argv[1];
  const std::uint32_t D = argc > 2 ? std::stoul(argv[2]) : 8192;
  // relations to keep (top closure contributors + the hitfix chain ones)
  const std::vector<std::string> keep = {
      "/people/person/nationality",
      "/award/award_winner/awards_won./award/award_honor/award_winner",
      "/film/film/country",
      "/people/person/place_of_birth",
      "/award/award_nominee/award_nominations./award/award_nomination/nominated_for",
      "/film/actor/film./film/performance/film",
      "/film/film/personal_appearances./film/personal_film_appearance/person",
      "/award/award_category/winners./award/award_honor/award_winner"};
  std::unordered_map<std::string, std::uint32_t> rel_id;
  for (std::size_t i = 0; i < keep.size(); ++i) rel_id[keep[i]] = i;

  std::unordered_map<std::string, std::uint32_t> ent_id;
  auto eid = [&](const std::string& s) {
    auto [it, added] = ent_id.emplace(s, ent_id.size());
    return it->second;
  };
  // edges per relation
  std::vector<std::unordered_set<std::uint64_t>> edges(keep.size());
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> elist(keep.size());
  FILE* f = std::fopen(closure, "r");
  if (!f) return 1;
  char* line = nullptr;
  size_t cap = 0;
  std::uint64_t nread = 0;
  while (getline(&line, &cap, f) > 0) {
    if (line[0] == '#') continue;
    if ((++nread & 0xFFFFF) == 0) std::fprintf(stderr, "\r%.1fM rows", nread / 1e6);
    char* t1 = std::strchr(line, '\t');
    if (!t1) continue;
    *t1 = 0;
    char* t2 = std::strchr(t1 + 1, '\t');
    if (!t2) continue;
    *t2 = 0;
    char* t3 = std::strchr(t2 + 1, '\t');
    if (!t3) continue;
    *t3 = 0;
    const auto it = rel_id.find(t1 + 1);
    if (it == rel_id.end()) continue;
    const std::uint32_t r = it->second;
    const std::uint32_t s = eid(line), d = eid(t2 + 1);
    if (edges[r].insert((std::uint64_t(s) << 32) | d).second) {
      elist[r].emplace_back(s, d);
    }
  }
  std::fclose(f);
  std::fprintf(stderr, "\nentities=%zu\n", ent_id.size());

  // subgoals with outcomes
  struct SG { gar::hdc::Hypervector hv; bool pos; std::uint32_t rel; std::uint32_t s, d; };
  std::vector<SG> sgs;
  gar::hdc::HdcEncoder enc({D, 0});
  std::mt19937_64 rng(123);
  for (std::size_t r = 0; r < keep.size(); ++r) {
    if (elist[r].size() < 200) continue;
    const auto& el = elist[r];
    const std::uint64_t psym = gar::hdc::SplitMix64Next(*(new uint64_t(Fnv1a64(keep[r])))) ;
    // (leak is fine for a bench; but do it properly:)
    std::uint64_t tmp = Fnv1a64(keep[r]);
    const std::uint64_t ps = gar::hdc::SplitMix64Next(tmp) | (1ull << 63);
    (void)psym;
    auto encode = [&](std::uint32_t s, std::uint32_t d) {
      const std::uint64_t args[2] = {s, d};
      return enc.EncodeFact(ps, args, 2);
    };
    const std::size_t cap2 = 1200;
    std::shuffle(elist[r].begin(), elist[r].end(), rng);
    std::size_t taken = 0;
    for (const auto& [s, d] : elist[r]) {
      if (taken >= cap2) break;
      sgs.push_back(SG{encode(s, d), true, std::uint32_t(r), s, d});
      ++taken;
    }
    // negatives: same src pool, random dst from the relation's dst pool
    std::vector<std::uint32_t> dsts;
    for (const auto& [s, d] : el) dsts.push_back(d);
    std::shuffle(dsts.begin(), dsts.end(), rng);
    std::size_t negs = 0;
    for (std::size_t i = 0; i < el.size() * 4 && negs < cap2; ++i) {
      const std::uint32_t s = el[rng() % el.size()].first;
      const std::uint32_t d = dsts[rng() % dsts.size()];
      if (edges[r].count((std::uint64_t(s) << 32) | d)) continue;
      sgs.push_back(SG{encode(s, d), false, std::uint32_t(r), s, d});
      ++negs;
    }
  }
  std::fprintf(stderr, "subgoals=%zu\n", sgs.size());

  // pair sampling: cosine band -> outcome agreement
  struct Band { double lo, hi; std::uint64_t n = 0, agree = 0, same_rel = 0, agree_sr = 0, same_src = 0, agree_ss = 0; };
  std::vector<Band> bands = {{0, .05}, {.05, .15}, {.15, .30}, {.30, .50}, {.50, .70}, {.70, 1.01}};
  const std::uint64_t NP = 4000000;
  for (std::uint64_t k = 0; k < NP; ++k) {
    const auto& a = sgs[rng() % sgs.size()];
    const auto& b = sgs[rng() % sgs.size()];
    const double c = gar::hdc::Hypervector::Cosine(a.hv, b.hv);
    const bool ag = a.pos == b.pos;
    for (auto& bd : bands) {
      if (c >= bd.lo && c < bd.hi) {
        ++bd.n;
        bd.agree += ag;
        if (a.rel == b.rel) { ++bd.same_rel; bd.agree_sr += ag; }
        if (a.rel == b.rel && a.s == b.s) { ++bd.same_src; bd.agree_ss += ag; }
        break;
      }
    }
  }
  std::uint64_t npos = 0;
  for (const auto& s : sgs) npos += s.pos;
  const double p = double(npos) / sgs.size();
  std::printf("# subgoals=%zu pos_rate=%.3f baseline_agree=%.3f dim=%u\n",
              sgs.size(), p, p * p + (1 - p) * (1 - p), D);
  std::printf("%-12s %-10s %-10s %-12s %-12s\n", "cos_band", "pairs", "agree",
              "same_rel_n", "same_rel_agree");
  for (const auto& bd : bands) {
    std::printf("[%.2f,%2.2f)  n=%-9llu agree=%.3f  samerel_n=%-9llu agree=%.3f  samesrc_n=%-8llu agree=%.3f\n",
                bd.lo, bd.hi, (unsigned long long)bd.n,
                bd.n ? double(bd.agree) / bd.n : 0.0,
                (unsigned long long)bd.same_rel,
                bd.same_rel ? double(bd.agree_sr) / bd.same_rel : 0.0,
                (unsigned long long)bd.same_src,
                bd.same_src ? double(bd.agree_ss) / bd.same_src : 0.0);
  }
  return 0;
}
