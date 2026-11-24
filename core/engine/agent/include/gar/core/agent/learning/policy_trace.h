#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "gar/core/agent/fact.h"
#include "gar/core/agent/goal.h"
#include "gar/core/agent/scorer.h"
#include "gar/core/hdc/encoder.h"

namespace gar::core::engine::agent::learning {

struct CandidateSnapshot {
  std::uint64_t decision_id = 0;
  std::uint64_t action_id = 0;
  Fact consequent;
  double handcrafted_score = 0.0;
  bool selected = false;
};

struct DerivationRecord {
  std::uint64_t action_id = 0;
  Fact consequent;
  std::vector<Fact> premises;
};

struct PolicyTrace {
  Goal goal;
  gar::core::hdc::HdcEncoder::Config hdc;
  HandcraftedConfig scorer;
  std::uint64_t graph_fingerprint = 0;
  std::vector<CandidateSnapshot> candidates;
  std::vector<DerivationRecord> derivations;
  bool goal_derived = false;
};

void WritePolicyTrace(const std::filesystem::path& path,
                      const PolicyTrace& trace);
PolicyTrace ReadPolicyTrace(const std::filesystem::path& path);

}  // namespace gar::core::engine::agent::learning
