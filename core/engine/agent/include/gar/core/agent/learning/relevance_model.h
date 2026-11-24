#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "gar/core/hdc/dense_vector.h"

namespace gar::core::engine::agent::learning {

struct RelevanceFeatures {
  const gar::core::hdc::DenseVector* action = nullptr;
  const gar::core::hdc::DenseVector* goal = nullptr;
  double handcrafted_score = 0.0;
  // Explicit action-goal similarity. The additive hidden layer cannot compute
  // the v_action . v_goal interaction on its own, so goal-instance matching
  // (which lives in this cosine) must be supplied as a feature.
  double cosine = 0.0;
};

struct RelevanceModelMetadata {
  std::uint32_t dim = 0;
  std::uint32_t hidden = 32;
  std::uint64_t hdc_seed = 0;
  std::uint64_t graph_fingerprint = 0;
  double label_gamma = 0.5;
  std::size_t negatives_per_positive = 4;
  std::uint64_t seed = 42;
  std::vector<std::string> trained_on;
};

class RelevanceModel {
 public:
  struct RelevanceProjection {
    std::vector<double> hidden_input;
  };

  struct Parameters {
    std::vector<double> action_weights;
    std::vector<double> goal_weights;
    std::vector<double> score_weights;
    std::vector<double> cosine_weights;
    std::vector<double> hidden_bias;
    std::vector<double> output_weights;
    double output_bias = 0.0;
  };

  RelevanceModel(RelevanceModelMetadata metadata, Parameters parameters);
  RelevanceModel(const RelevanceModel& other)
      : metadata_(other.metadata_), parameters_(other.parameters_) {}
  RelevanceModel& operator=(const RelevanceModel& other) {
    metadata_ = other.metadata_;
    parameters_ = other.parameters_;
    return *this;
  }
  RelevanceModel(RelevanceModel&& other) noexcept
      : metadata_(std::move(other.metadata_)),
        parameters_(std::move(other.parameters_)) {}
  RelevanceModel& operator=(RelevanceModel&& other) noexcept {
    metadata_ = std::move(other.metadata_);
    parameters_ = std::move(other.parameters_);
    return *this;
  }

  static RelevanceModel Initialize(const RelevanceModelMetadata& metadata);
  static RelevanceModel Load(const std::filesystem::path& meta_path);
  void Save(const std::filesystem::path& meta_path) const;

  [[nodiscard]] double Predict(const RelevanceFeatures& features) const;
  [[nodiscard]] std::vector<double> PredictBatch(
      const std::vector<RelevanceFeatures>& features) const;
  [[nodiscard]] RelevanceProjection ProjectAction(
      const gar::core::hdc::DenseVector& action) const;
  [[nodiscard]] RelevanceProjection ProjectGoal(
      const gar::core::hdc::DenseVector& goal) const;
  [[nodiscard]] double PredictProjected(const RelevanceProjection& action,
                                        const RelevanceProjection& goal,
                                        double handcrafted_score,
                                        double cosine) const;

  [[nodiscard]] const RelevanceModelMetadata& metadata() const {
    return metadata_;
  }
  [[nodiscard]] std::uint64_t infer_calls() const {
    return infer_calls_.load();
  }
  [[nodiscard]] double infer_ns() const { return infer_ns_.load(); }

 private:
  struct Evaluation {
    std::vector<double> hidden;
    double logit = 0.0;
    double output = 0.0;
  };

  Evaluation Evaluate(const RelevanceFeatures& features) const;
  RelevanceProjection Project(const gar::core::hdc::DenseVector& vector,
                              const std::vector<double>& weights) const;
  std::size_t ParameterCount() const;
  void Validate() const;

  RelevanceModelMetadata metadata_;
  Parameters parameters_;
  mutable std::atomic<std::uint64_t> infer_calls_{0};
  mutable std::atomic<double> infer_ns_{0.0};

  friend class RelevanceTrainer;
};

}  // namespace gar::core::engine::agent::learning
