#include "gar/analysis/goal_sampler.h"

#include <random>

namespace gar::core::analysis {

std::vector<std::uint32_t> GoalSampler::DerivableSlots() const {
  std::vector<std::uint32_t> out;
  for (std::uint32_t r = 0; r < g_.num_relations(); ++r) {
    const std::uint32_t p = dg_.table().Find(g_.RelationName(r));
    if (p != kInvalidPredicate && dg_.InDegree(p) > 0) out.push_back(r);
  }
  return out;
}

std::vector<GoalCandidate> GoalSampler::Sample(std::uint32_t per_slot,
                                               std::uint64_t seed) const {
  std::mt19937_64 rng(seed);
  std::vector<GoalCandidate> out;
  for (const std::uint32_t slot : DerivableSlots()) {
    const auto& pairs = g_.RelationPairs(slot);
    const std::size_t m = pairs.size();
    if (m == 0) continue;
    if (m <= per_slot) {
      for (const std::uint64_t p : pairs) {
        out.push_back({slot, static_cast<std::uint32_t>(p >> 32),
                       static_cast<std::uint32_t>(p)});
      }
      continue;
    }
    // Distinct uniform indices with rejection (per_slot << m).
    std::vector<std::uint64_t> picked;
    picked.reserve(per_slot);
    while (picked.size() < per_slot) {
      const std::uint64_t idx = rng() % m;
      bool dup = false;
      for (const std::uint64_t q : picked) {
        if (q == idx) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      picked.push_back(idx);
      const std::uint64_t p = pairs[idx];
      out.push_back({slot, static_cast<std::uint32_t>(p >> 32),
                     static_cast<std::uint32_t>(p)});
    }
  }
  return out;
}

}  // namespace gar::core::analysis
