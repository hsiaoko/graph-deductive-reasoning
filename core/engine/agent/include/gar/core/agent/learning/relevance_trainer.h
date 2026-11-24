#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gar/core/agent/learning/relevance_model.h"

namespace gar::core::engine::agent::learning {

struct VectorRelevanceExample {
  gar::core::hdc::DenseVector action;
  gar::core::hdc::DenseVector goal;
  double handcrafted_score = 0.0;
  double target = 0.0;
  std::uint64_t goal_key = 0;
  double cosine = 0.0;
};

struct RelevanceTrainingConfig {
  std::uint32_t epochs = 8;
  std::size_t batch_size = 32;
  double learning_rate = 0.001;
  double l2 = 0.00001;
  std::uint64_t seed = 42;
  // Pairwise ranking loss within each goal (positive must outscore negative),
  // instead of MSE regression on the target. Aligns training with the policy's
  // top-k selection and is more sample-efficient than absolute-value regression.
  bool ranking = false;
};

struct RelevanceTrainingReport {
  std::vector<double> epoch_mse;
  std::vector<double> epoch_pairwise_accuracy;
  std::uint64_t examples = 0;
  std::uint64_t pairs = 0;
};

class RelevanceTrainer {
 public:
  explicit RelevanceTrainer(RelevanceTrainingConfig config) : config_(config) {}

  RelevanceTrainingReport Train(
      const std::vector<VectorRelevanceExample>& examples,
      RelevanceModel* model) const;

 private:
  RelevanceTrainingConfig config_;
};

}  // namespace gar::core::engine::agent::learning
