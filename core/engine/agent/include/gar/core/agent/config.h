#pragma once

namespace gar::core::engine::agent {

enum class MatchEngine {
  kCpu,
  kGpu,
};

enum class SearchDirection {
  kForward,
  kBackward,
};

enum class PolicyKind {
  kDeterministic,
  kRandom,
  kHybrid,
  kJev,
  kLearned,
  kAdaptive,
  kReplay,
  kQHdc,
  kQNumeric,
};

enum class ScorerKind {
  kExact,
  kNull,
  kHdc,
};

enum class StarOrder {
  kInput,
  kGoal,
};

inline const char* MatchEngineName(MatchEngine engine) {
  return engine == MatchEngine::kCpu ? "cpu" : "gpu";
}

inline const char* SearchDirectionName(SearchDirection direction) {
  return direction == SearchDirection::kForward ? "forward" : "backward";
}

inline const char* PolicyName(PolicyKind policy) {
  switch (policy) {
    case PolicyKind::kDeterministic: return "deterministic";
    case PolicyKind::kRandom: return "random";
    case PolicyKind::kHybrid: return "hybrid";
    case PolicyKind::kJev: return "jev";
    case PolicyKind::kLearned: return "learned";
    case PolicyKind::kAdaptive: return "adaptive";
    case PolicyKind::kReplay: return "replay";
    case PolicyKind::kQHdc: return "q-hdc";
    case PolicyKind::kQNumeric: return "q-numeric";
  }
  return "unknown";
}

inline const char* ScorerName(ScorerKind scorer) {
  switch (scorer) {
    case ScorerKind::kExact: return "exact";
    case ScorerKind::kNull: return "null";
    case ScorerKind::kHdc: return "hdc";
  }
  return "unknown";
}

inline const char* StarOrderName(StarOrder order) {
  return order == StarOrder::kInput ? "input" : "goal";
}

}  // namespace gar::core::engine::agent
