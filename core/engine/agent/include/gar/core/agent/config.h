#pragma once

namespace gar::core::engine::agent {

enum class MatchEngine {
  kCpu,
  kGpu,
};

enum class SearchDirection {
  kForward,
  kBackward,
  // Backward-first with a bounded call budget; falls back to forward AdaDeduce
  // when backward fails (e.g. central predicates whose reverse subgoal search
  // explodes). Captures backward's proof-directed search reduction while
  // preserving forward completeness.
  kHybrid,
  // Learned goal-level gate over the hybrid: a classifier predicts backward
  // success from cone size and predicate degree, routing to backward only when
  // predicted to succeed (skips futile backward on central predicates).
  kLearnedStrategy,
};

enum class PolicyKind {
  kDeterministic,
  kRandom,
  kUniformRandom,
  kHybrid,
  kJev,
  kLearned,
  kAdaptive,
  kReplay,
};

enum class ScorerKind {
  kExact,
  kNull,
  kHdc,
};

enum class StarOrder {
  kInput,
  kGoal,
  // Goal-conditioned choice between input and goal order, decided by a learned
  // goal-level logistic model (ExecConfig::star_order_model_path). Pure forward
  // selection; no backward search is involved.
  kLearned,
};

inline const char* MatchEngineName(MatchEngine engine) {
  return engine == MatchEngine::kCpu ? "cpu" : "gpu";
}

inline const char* SearchDirectionName(SearchDirection direction) {
  switch (direction) {
    case SearchDirection::kForward:
      return "forward";
    case SearchDirection::kBackward:
      return "backward";
    case SearchDirection::kHybrid:
      return "hybrid";
    case SearchDirection::kLearnedStrategy:
      return "learned-strategy";
  }
  return "forward";
}

inline const char* PolicyName(PolicyKind policy) {
  switch (policy) {
    case PolicyKind::kDeterministic: return "deterministic";
    case PolicyKind::kRandom: return "random";
    case PolicyKind::kUniformRandom: return "uniform-random";
    case PolicyKind::kHybrid: return "hybrid";
    case PolicyKind::kJev: return "jev";
    case PolicyKind::kLearned: return "learned";
    case PolicyKind::kAdaptive: return "adaptive";
    case PolicyKind::kReplay: return "replay";
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
  switch (order) {
    case StarOrder::kInput: return "input";
    case StarOrder::kGoal: return "goal";
    case StarOrder::kLearned: return "learned";
  }
  return "input";
}

}  // namespace gar::core::engine::agent
