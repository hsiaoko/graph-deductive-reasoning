#include "gar/core/agent/learning/relevance_model.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace gar::core::engine::agent::learning {
namespace {

using Json = nlohmann::json;

// SIMD dot product for the action/goal projection (the per-fact cost bottleneck
// of the learned policy). Function-level target attributes + runtime dispatch so
// no global -march flag is required. FP reassociation differs from scalar in the
// last bits, which is immaterial for ML inference.
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
double DotAvx2(const double* a, const double* b, std::size_t n) {
  __m256d acc = _mm256_setzero_pd();
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    acc = _mm256_add_pd(acc, _mm256_mul_pd(_mm256_loadu_pd(a + i),
                                           _mm256_loadu_pd(b + i)));
  }
  alignas(32) double tmp[4];
  _mm256_store_pd(tmp, acc);
  double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < n; ++i) sum += a[i] * b[i];
  return sum;
}

__attribute__((target("avx512f")))
double DotAvx512(const double* a, const double* b, std::size_t n) {
  __m512d acc = _mm512_setzero_pd();
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    acc = _mm512_add_pd(acc, _mm512_mul_pd(_mm512_loadu_pd(a + i),
                                           _mm512_loadu_pd(b + i)));
  }
  double sum = _mm512_reduce_add_pd(acc);
  for (; i < n; ++i) sum += a[i] * b[i];
  return sum;
}
#endif

double Dot(const double* a, const double* b, std::size_t n) {
#if defined(__x86_64__) || defined(_M_X64)
  if (__builtin_cpu_supports("avx512f")) return DotAvx512(a, b, n);
  if (__builtin_cpu_supports("avx2")) return DotAvx2(a, b, n);
#endif
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) sum += a[i] * b[i];
  return sum;
}

// Mixed-precision projection dot: double model weights against float fact
// vectors (DenseVector is float to halve encode bandwidth). Widen the float
// lanes to double in-register so the weight bandwidth (small, cached) stays
// double while the fact bandwidth (the per-fact bottleneck) is halved.
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
double DotMixedAvx2(const double* w, const float* v, std::size_t n) {
  __m256d acc = _mm256_setzero_pd();
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256d vd = _mm256_cvtps_pd(_mm_loadu_ps(v + i));
    acc = _mm256_add_pd(acc, _mm256_mul_pd(_mm256_loadu_pd(w + i), vd));
  }
  alignas(32) double tmp[4];
  _mm256_store_pd(tmp, acc);
  double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < n; ++i) sum += w[i] * v[i];
  return sum;
}

__attribute__((target("avx512f")))
double DotMixedAvx512(const double* w, const float* v, std::size_t n) {
  __m512d acc = _mm512_setzero_pd();
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m512d vd = _mm512_cvtps_pd(_mm256_loadu_ps(v + i));
    acc = _mm512_add_pd(acc, _mm512_mul_pd(_mm512_loadu_pd(w + i), vd));
  }
  double sum = _mm512_reduce_add_pd(acc);
  for (; i < n; ++i) sum += w[i] * v[i];
  return sum;
}
#endif

double DotMixed(const double* w, const float* v, std::size_t n) {
#if defined(__x86_64__) || defined(_M_X64)
  if (__builtin_cpu_supports("avx512f")) return DotMixedAvx512(w, v, n);
  if (__builtin_cpu_supports("avx2")) return DotMixedAvx2(w, v, n);
#endif
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) sum += w[i] * v[i];
  return sum;
}

void WriteVector(std::ofstream* out, const std::vector<double>& values) {
  out->write(reinterpret_cast<const char*>(values.data()),
             static_cast<std::streamsize>(values.size() * sizeof(double)));
}

void ReadVector(std::ifstream* in, std::vector<double>* values) {
  in->read(reinterpret_cast<char*>(values->data()),
           static_cast<std::streamsize>(values->size() * sizeof(double)));
}

}  // namespace

RelevanceModel::RelevanceModel(RelevanceModelMetadata metadata,
                               Parameters parameters)
    : metadata_(std::move(metadata)), parameters_(std::move(parameters)) {
  Validate();
}

std::size_t RelevanceModel::ParameterCount() const {
  return parameters_.action_weights.size() + parameters_.goal_weights.size() +
         parameters_.score_weights.size() + parameters_.cosine_weights.size() +
         parameters_.hidden_bias.size() + parameters_.output_weights.size() + 1;
}

void RelevanceModel::Validate() const {
  if (metadata_.dim == 0 || metadata_.hidden == 0) {
    throw std::invalid_argument("relevance model dimensions must be positive");
  }
  if (!(metadata_.label_gamma > 0.0 && metadata_.label_gamma <= 1.0) ||
      !std::isfinite(metadata_.label_gamma)) {
    throw std::invalid_argument("relevance model label_gamma must be in (0,1]");
  }
  const std::size_t matrix_size =
      static_cast<std::size_t>(metadata_.dim) * metadata_.hidden;
  const std::size_t hidden = metadata_.hidden;
  if (parameters_.action_weights.size() != matrix_size ||
      parameters_.goal_weights.size() != matrix_size ||
      parameters_.score_weights.size() != hidden ||
      parameters_.cosine_weights.size() != hidden ||
      parameters_.hidden_bias.size() != hidden ||
      parameters_.output_weights.size() != hidden) {
    throw std::invalid_argument(
        "relevance model parameter shape does not match metadata");
  }
  for (const auto* values :
       {&parameters_.action_weights, &parameters_.goal_weights,
        &parameters_.score_weights, &parameters_.cosine_weights,
        &parameters_.hidden_bias, &parameters_.output_weights}) {
    for (double value : *values) {
      if (!std::isfinite(value)) {
        throw std::invalid_argument(
            "relevance model parameters must be finite");
      }
    }
  }
  if (!std::isfinite(parameters_.output_bias)) {
    throw std::invalid_argument("relevance model output bias must be finite");
  }
}

RelevanceModel RelevanceModel::Initialize(
    const RelevanceModelMetadata& metadata) {
  Parameters parameters;
  const std::size_t matrix_size =
      static_cast<std::size_t>(metadata.dim) * metadata.hidden;
  parameters.action_weights.resize(matrix_size);
  parameters.goal_weights.resize(matrix_size);
  parameters.score_weights.resize(metadata.hidden);
  parameters.cosine_weights.resize(metadata.hidden);
  parameters.hidden_bias.resize(metadata.hidden, 0.0);
  parameters.output_weights.resize(metadata.hidden);
  std::mt19937_64 rng(metadata.seed);
  std::normal_distribution<double> input_weight(
      0.0, std::sqrt(1.0 / static_cast<double>(metadata.dim)));
  for (double& value : parameters.action_weights) value = input_weight(rng);
  for (double& value : parameters.goal_weights) value = input_weight(rng);
  std::normal_distribution<double> score_weight(0.0, 0.1);
  for (double& value : parameters.score_weights) value = score_weight(rng);
  for (double& value : parameters.cosine_weights) value = score_weight(rng);
  std::normal_distribution<double> output_weight(
      0.0, std::sqrt(1.0 / static_cast<double>(metadata.hidden)));
  for (double& value : parameters.output_weights) value = output_weight(rng);
  return RelevanceModel(metadata, std::move(parameters));
}

RelevanceModel::Evaluation RelevanceModel::Evaluate(
    const RelevanceFeatures& features) const {
  Evaluation evaluation;
  const std::size_t hidden = metadata_.hidden;
  const std::size_t dim = metadata_.dim;
  evaluation.hidden.resize(hidden);
  const auto& action = features.action->values();
  const auto& goal = features.goal->values();
  for (std::size_t h = 0; h < hidden; ++h) {
    const double* action_weights = parameters_.action_weights.data() + h * dim;
    const double* goal_weights = parameters_.goal_weights.data() + h * dim;
    double value = parameters_.hidden_bias[h] +
                   parameters_.score_weights[h] * features.handcrafted_score +
                   parameters_.cosine_weights[h] * features.cosine;
    for (std::size_t d = 0; d < dim; ++d) {
      value += action_weights[d] * action[d] + goal_weights[d] * goal[d];
    }
    evaluation.hidden[h] = std::tanh(value);
  }
  double output = parameters_.output_bias;
  for (std::size_t h = 0; h < hidden; ++h) {
    output += parameters_.output_weights[h] * evaluation.hidden[h];
  }
  evaluation.logit = output;
  evaluation.output = output >= 0.0
                          ? 1.0 / (1.0 + std::exp(-output))
                          : std::exp(output) / (1.0 + std::exp(output));
  return evaluation;
}

RelevanceModel::RelevanceProjection RelevanceModel::Project(
    const gar::core::hdc::DenseVector& vector,
    const std::vector<double>& weights) const {
  if (vector.dim() != metadata_.dim) {
    throw std::invalid_argument("invalid relevance model features");
  }
  RelevanceProjection projection;
  projection.hidden_input.resize(metadata_.hidden);
  const auto& values = vector.values();
  const std::size_t dim = metadata_.dim;
  for (std::size_t h = 0; h < metadata_.hidden; ++h) {
    const double* row = weights.data() + h * dim;
    projection.hidden_input[h] = DotMixed(row, values.data(), dim);
  }
  return projection;
}

RelevanceModel::RelevanceProjection RelevanceModel::ProjectAction(
    const gar::core::hdc::DenseVector& action) const {
  return Project(action, parameters_.action_weights);
}

RelevanceModel::RelevanceProjection RelevanceModel::ProjectGoal(
    const gar::core::hdc::DenseVector& goal) const {
  return Project(goal, parameters_.goal_weights);
}

double RelevanceModel::PredictProjected(const RelevanceProjection& action,
                                        const RelevanceProjection& goal,
                                        double handcrafted_score,
                                        double cosine) const {
  if (action.hidden_input.size() != metadata_.hidden ||
      goal.hidden_input.size() != metadata_.hidden ||
      !std::isfinite(handcrafted_score) || !std::isfinite(cosine)) {
    throw std::invalid_argument("invalid relevance model projections");
  }
  const auto started = std::chrono::steady_clock::now();
  double output = parameters_.output_bias;
  for (std::size_t h = 0; h < metadata_.hidden; ++h) {
    const double value = parameters_.hidden_bias[h] +
                         parameters_.score_weights[h] * handcrafted_score +
                         parameters_.cosine_weights[h] * cosine +
                         action.hidden_input[h] + goal.hidden_input[h];
    output += parameters_.output_weights[h] * std::tanh(value);
  }
  const double prediction = output >= 0.0
                                ? 1.0 / (1.0 + std::exp(-output))
                                : std::exp(output) / (1.0 + std::exp(output));
  infer_ns_.fetch_add(std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - started)
                          .count());
  infer_calls_.fetch_add(1);
  return prediction;
}

double RelevanceModel::Predict(const RelevanceFeatures& features) const {
  if (features.action == nullptr || features.goal == nullptr ||
      features.action->dim() != metadata_.dim ||
      features.goal->dim() != metadata_.dim ||
      !std::isfinite(features.handcrafted_score)) {
    throw std::invalid_argument("invalid relevance model features");
  }
  const auto started = std::chrono::steady_clock::now();
  const double prediction = Evaluate(features).output;
  infer_ns_.fetch_add(std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - started)
                          .count());
  infer_calls_.fetch_add(1);
  return prediction;
}

std::vector<double> RelevanceModel::PredictBatch(
    const std::vector<RelevanceFeatures>& features) const {
  std::vector<double> predictions;
  predictions.reserve(features.size());
  for (const RelevanceFeatures& item : features)
    predictions.push_back(Predict(item));
  return predictions;
}

void RelevanceModel::Save(const std::filesystem::path& meta_path) const {
  Validate();
  if (!meta_path.parent_path().empty()) {
    std::filesystem::create_directories(meta_path.parent_path());
  }
  const Json meta{{"format", "gar_relevance_v1"},
                  {"feature_schema", "action_goal_relevance_v1"},
                  {"model", "mlp_tanh_sigmoid_v1"},
                  {"dim", metadata_.dim},
                  {"hidden", metadata_.hidden},
                  {"hdc_seed", metadata_.hdc_seed},
                  {"graph_fingerprint", metadata_.graph_fingerprint},
                  {"label_gamma", metadata_.label_gamma},
                  {"negatives_per_positive", metadata_.negatives_per_positive},
                  {"seed", metadata_.seed},
                  {"trained_on", metadata_.trained_on}};
  std::ofstream out(meta_path);
  if (!out)
    throw std::runtime_error("cannot write relevance model: " +
                             meta_path.string());
  out << meta.dump(2) << '\n';
  out.close();
  std::filesystem::path bin_path = meta_path;
  bin_path.replace_extension(".bin");
  std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
  if (!bin)
    throw std::runtime_error("cannot write relevance weights: " +
                             bin_path.string());
  WriteVector(&bin, parameters_.action_weights);
  WriteVector(&bin, parameters_.goal_weights);
  WriteVector(&bin, parameters_.score_weights);
  WriteVector(&bin, parameters_.cosine_weights);
  WriteVector(&bin, parameters_.hidden_bias);
  WriteVector(&bin, parameters_.output_weights);
  bin.write(reinterpret_cast<const char*>(&parameters_.output_bias),
            sizeof(double));
  if (!bin) throw std::runtime_error("failed to write relevance weights");
}

RelevanceModel RelevanceModel::Load(const std::filesystem::path& meta_path) {
  std::ifstream in(meta_path);
  if (!in)
    throw std::runtime_error("cannot read relevance model: " +
                             meta_path.string());
  Json meta;
  try {
    meta = Json::parse(in);
  } catch (const Json::parse_error&) {
    throw std::invalid_argument("unsupported relevance model format");
  }
  if (!meta.is_object() || !meta.contains("format") ||
      !meta.contains("feature_schema") || !meta.contains("model") ||
      meta.at("format").get<std::string>() != "gar_relevance_v1" ||
      meta.at("feature_schema").get<std::string>() !=
          "action_goal_relevance_v1" ||
      meta.at("model").get<std::string>() != "mlp_tanh_sigmoid_v1") {
    throw std::invalid_argument("unsupported relevance model format");
  }
  RelevanceModelMetadata metadata;
  metadata.dim = meta.at("dim").get<std::uint32_t>();
  metadata.hidden = meta.at("hidden").get<std::uint32_t>();
  metadata.hdc_seed = meta.at("hdc_seed").get<std::uint64_t>();
  metadata.graph_fingerprint =
      meta.at("graph_fingerprint").get<std::uint64_t>();
  metadata.label_gamma = meta.at("label_gamma").get<double>();
  metadata.negatives_per_positive =
      meta.at("negatives_per_positive").get<std::size_t>();
  metadata.seed = meta.at("seed").get<std::uint64_t>();
  metadata.trained_on = meta.at("trained_on").get<std::vector<std::string>>();
  Parameters parameters;
  const std::size_t matrix_size =
      static_cast<std::size_t>(metadata.dim) * metadata.hidden;
  parameters.action_weights.resize(matrix_size);
  parameters.goal_weights.resize(matrix_size);
  parameters.score_weights.resize(metadata.hidden);
  parameters.cosine_weights.resize(metadata.hidden);
  parameters.hidden_bias.resize(metadata.hidden);
  parameters.output_weights.resize(metadata.hidden);
  std::filesystem::path bin_path = meta_path;
  bin_path.replace_extension(".bin");
  std::ifstream bin(bin_path, std::ios::binary | std::ios::ate);
  if (!bin)
    throw std::runtime_error("cannot read relevance weights: " +
                             bin_path.string());
  const RelevanceModel shape_check(metadata, parameters);
  const auto expected_bytes = static_cast<std::streamoff>(
      (shape_check.ParameterCount()) * sizeof(double));
  if (bin.tellg() != expected_bytes) {
    throw std::invalid_argument(
        "relevance weight file has an incompatible size");
  }
  bin.seekg(0);
  ReadVector(&bin, &parameters.action_weights);
  ReadVector(&bin, &parameters.goal_weights);
  ReadVector(&bin, &parameters.score_weights);
  ReadVector(&bin, &parameters.cosine_weights);
  ReadVector(&bin, &parameters.hidden_bias);
  ReadVector(&bin, &parameters.output_weights);
  bin.read(reinterpret_cast<char*>(&parameters.output_bias), sizeof(double));
  if (!bin) throw std::runtime_error("truncated relevance weights");
  return RelevanceModel(metadata, std::move(parameters));
}

}  // namespace gar::core::engine::agent::learning
