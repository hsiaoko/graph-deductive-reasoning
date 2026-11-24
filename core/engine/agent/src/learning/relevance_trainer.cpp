#include "gar/core/agent/learning/relevance_trainer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gar::core::engine::agent::learning {
namespace {

void Zero(RelevanceModel::Parameters* parameters) {
  for (auto* values : {&parameters->action_weights, &parameters->goal_weights,
                       &parameters->score_weights, &parameters->cosine_weights,
                       &parameters->hidden_bias, &parameters->output_weights}) {
    std::fill(values->begin(), values->end(), 0.0);
  }
  parameters->output_bias = 0.0;
}

void Update(const RelevanceModel::Parameters& gradient, double learning_rate,
            double l2, RelevanceModel::Parameters* parameters) {
  auto update = [](const std::vector<double>& gradient_values, double rate,
                   double penalty, std::vector<double>* values) {
    for (std::size_t i = 0; i < values->size(); ++i) {
      (*values)[i] -= rate * (gradient_values[i] + penalty * (*values)[i]);
    }
  };
  update(gradient.action_weights, learning_rate, l2,
         &parameters->action_weights);
  update(gradient.goal_weights, learning_rate, l2, &parameters->goal_weights);
  update(gradient.score_weights, learning_rate, l2, &parameters->score_weights);
  update(gradient.cosine_weights, learning_rate, l2,
         &parameters->cosine_weights);
  update(gradient.hidden_bias, learning_rate, 0.0, &parameters->hidden_bias);
  update(gradient.output_weights, learning_rate, l2,
         &parameters->output_weights);
  parameters->output_bias -= learning_rate * gradient.output_bias;
}

}  // namespace

RelevanceTrainingReport RelevanceTrainer::Train(
    const std::vector<VectorRelevanceExample>& examples,
    RelevanceModel* model) const {
  if (model == nullptr || examples.empty()) {
    throw std::invalid_argument(
        "relevance training requires a model and examples");
  }
  if (config_.epochs == 0 || config_.batch_size == 0 ||
      !(config_.learning_rate > 0.0) || !(config_.l2 >= 0.0) ||
      !std::isfinite(config_.learning_rate) || !std::isfinite(config_.l2)) {
    throw std::invalid_argument("invalid relevance training configuration");
  }
  const std::uint32_t dim = model->metadata_.dim;
  for (const auto& example : examples) {
    if (example.action.dim() != dim || example.goal.dim() != dim ||
        !std::isfinite(example.handcrafted_score) ||
        !std::isfinite(example.target) || example.target < 0.0 ||
        example.target > 1.0) {
      throw std::invalid_argument("invalid vector relevance example");
    }
  }

  RelevanceModel::Parameters gradient;
  const std::size_t matrix_size =
      static_cast<std::size_t>(dim) * model->metadata_.hidden;
  gradient.action_weights.resize(matrix_size);
  gradient.goal_weights.resize(matrix_size);
  gradient.score_weights.resize(model->metadata_.hidden);
  gradient.cosine_weights.resize(model->metadata_.hidden);
  gradient.hidden_bias.resize(model->metadata_.hidden);
  gradient.output_weights.resize(model->metadata_.hidden);
  std::vector<std::size_t> order(examples.size());
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(config_.seed);
  RelevanceTrainingReport report;
  report.examples = examples.size();

  if (config_.ranking) {
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> pos, neg;
    for (std::size_t i = 0; i < examples.size(); ++i) {
      if (examples[i].target > 0.0) {
        pos[examples[i].goal_key].push_back(i);
      } else {
        neg[examples[i].goal_key].push_back(i);
      }
    }
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (const auto& [key, pidx] : pos) {
      const auto it = neg.find(key);
      if (it == neg.end()) continue;
      for (std::size_t p : pidx) {
        for (std::size_t n : it->second) pairs.emplace_back(p, n);
      }
    }
    report.pairs = pairs.size();
    if (pairs.empty()) {
      throw std::invalid_argument(
          "ranking training needs positive-negative pairs within a goal");
    }
    const auto accumulate = [&](std::size_t idx, double delta) {
      const auto& example = examples[idx];
      const RelevanceFeatures features{&example.action, &example.goal,
                                       example.handcrafted_score,
                                       example.cosine};
      const auto ev = model->Evaluate(features);
      gradient.output_bias += delta;
      for (std::size_t h = 0; h < model->metadata_.hidden; ++h) {
        gradient.output_weights[h] += delta * ev.hidden[h];
        const double hd = delta * model->parameters_.output_weights[h] *
                          (1.0 - ev.hidden[h] * ev.hidden[h]);
        gradient.hidden_bias[h] += hd;
        gradient.score_weights[h] += hd * example.handcrafted_score;
        gradient.cosine_weights[h] += hd * example.cosine;
        double* ag = gradient.action_weights.data() + h * dim;
        double* gg = gradient.goal_weights.data() + h * dim;
        const auto& av = example.action.values();
        const auto& gv = example.goal.values();
        for (std::size_t d = 0; d < dim; ++d) {
          ag[d] += hd * av[d];
          gg[d] += hd * gv[d];
        }
      }
    };
    for (std::uint32_t epoch = 0; epoch < config_.epochs; ++epoch) {
      std::shuffle(pairs.begin(), pairs.end(), rng);
      std::uint64_t correct = 0;
      for (std::size_t begin = 0; begin < pairs.size();
           begin += config_.batch_size) {
        const std::size_t end =
            std::min(pairs.size(), begin + config_.batch_size);
        const double scale = 1.0 / static_cast<double>(end - begin);
        Zero(&gradient);
        for (std::size_t i = begin; i < end; ++i) {
          const auto [p, n] = pairs[i];
          const RelevanceFeatures fp{&examples[p].action, &examples[p].goal,
                                     examples[p].handcrafted_score,
                                     examples[p].cosine};
          const RelevanceFeatures fn{&examples[n].action, &examples[n].goal,
                                     examples[n].handcrafted_score,
                                     examples[n].cosine};
          const double margin =
              model->Evaluate(fp).logit - model->Evaluate(fn).logit;
          if (margin > 0.0) ++correct;
          const double g = 1.0 / (1.0 + std::exp(margin)) * scale;
          accumulate(p, -g);
          accumulate(n, g);
        }
        Update(gradient, config_.learning_rate, config_.l2, &model->parameters_);
      }
      report.epoch_pairwise_accuracy.push_back(
          static_cast<double>(correct) / static_cast<double>(pairs.size()));
      report.epoch_mse.push_back(0.0);
    }
    model->Validate();
    return report;
  }

  for (std::uint32_t epoch = 0; epoch < config_.epochs; ++epoch) {
    std::shuffle(order.begin(), order.end(), rng);
    double squared_error = 0.0;
    for (std::size_t begin = 0; begin < order.size();
         begin += config_.batch_size) {
      const std::size_t end =
          std::min(order.size(), begin + config_.batch_size);
      const double batch_scale = 1.0 / static_cast<double>(end - begin);
      Zero(&gradient);
      for (std::size_t position = begin; position < end; ++position) {
        const auto& example = examples[order[position]];
        const RelevanceFeatures features{&example.action, &example.goal,
                                         example.handcrafted_score,
                                         example.cosine};
        const RelevanceModel::Evaluation evaluation = model->Evaluate(features);
        const double error = evaluation.output - example.target;
        squared_error += error * error;
        const double output_delta = 2.0 * error * evaluation.output *
                                    (1.0 - evaluation.output) * batch_scale;
        gradient.output_bias += output_delta;
        for (std::size_t h = 0; h < model->metadata_.hidden; ++h) {
          gradient.output_weights[h] += output_delta * evaluation.hidden[h];
          const double hidden_delta =
              output_delta * model->parameters_.output_weights[h] *
              (1.0 - evaluation.hidden[h] * evaluation.hidden[h]);
          gradient.hidden_bias[h] += hidden_delta;
          gradient.score_weights[h] += hidden_delta * example.handcrafted_score;
          gradient.cosine_weights[h] += hidden_delta * example.cosine;
          double* action_gradient = gradient.action_weights.data() + h * dim;
          double* goal_gradient = gradient.goal_weights.data() + h * dim;
          const auto& action = example.action.values();
          const auto& goal = example.goal.values();
          for (std::size_t d = 0; d < dim; ++d) {
            action_gradient[d] += hidden_delta * action[d];
            goal_gradient[d] += hidden_delta * goal[d];
          }
        }
      }
      Update(gradient, config_.learning_rate, config_.l2, &model->parameters_);
    }
    report.epoch_mse.push_back(squared_error /
                               static_cast<double>(examples.size()));
  }
  model->Validate();
  return report;
}

}  // namespace gar::core::engine::agent::learning
