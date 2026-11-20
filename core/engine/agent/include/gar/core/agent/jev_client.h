#pragma once

// Generic decision-model interface. Candidate generation and all logical
// checks stay symbolic; the model only ranks supplied options.
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace gar::core::engine {
namespace agent {

struct LlmChoiceResult {
  std::string choice;
  std::vector<double> probabilities;  // aligned with `options`
  double confidence = 0.0;
};

class LlmClient {
 public:
  virtual ~LlmClient() = default;
  // `options` are opaque keys; `criteria` contains parallel profile text.
  virtual LlmChoiceResult Choice(const std::string& state,
                                 const std::string& instructions,
                                 const std::vector<std::string>& options,
                                 const std::vector<std::string>& criteria) = 0;
  // Returns the restricted guide JSON. Choice-only backends reject this call.
  virtual std::string Guide(const std::string& state,
                            const std::string& instructions) {
    (void)state;
    (void)instructions;
    throw std::runtime_error("guide generation is unsupported by this backend");
  }
};

// Reuses a real guide response across independent runs of the same goal.
class CachedLlmClient final : public LlmClient {
 public:
  CachedLlmClient(std::string response_path,
                  std::unique_ptr<LlmClient> fallback)
      : response_path_(std::move(response_path)),
        fallback_(std::move(fallback)) {}

  LlmChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override {
    return fallback_->Choice(state, instructions, options, criteria);
  }
  std::string Guide(const std::string& state,
                    const std::string& instructions) override;

 private:
  std::string response_path_;
  std::unique_ptr<LlmClient> fallback_;
};

// HTTP client for local Jev and System One-compatible endpoints.
class HttpJevClient final : public LlmClient {
 public:
  HttpJevClient(std::string endpoint, std::uint32_t timeout_ms,
                std::string model = {})
      : endpoint_(std::move(endpoint)),
        timeout_ms_(timeout_ms),
        model_(std::move(model)) {}

  LlmChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override;

 private:
  std::string endpoint_;
  std::uint32_t timeout_ms_;
  std::string model_;
};

// OpenAiLlmClient adapts OpenAI-compatible chat completions to Choice. The
// API key is read from DEEPSEEK_API_KEY at call time and is never logged.
class OpenAiLlmClient final : public LlmClient {
 public:
  OpenAiLlmClient(std::string base_url, std::string model,
                  std::uint32_t timeout_ms)
      : base_url_(std::move(base_url)),
        model_(std::move(model)),
        timeout_ms_(timeout_ms) {}

  LlmChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override;
  std::string Guide(const std::string& state,
                    const std::string& instructions) override;

 private:
  std::string base_url_;
  std::string model_;
  std::uint32_t timeout_ms_;
};

// Network-free deterministic regression client. It reads guide_score when
// present and the witnessed x/y ratio otherwise.
class MockLlmClient final : public LlmClient {
 public:
  explicit MockLlmClient(std::uint64_t seed) : seed_(seed) {}

  LlmChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override;
  std::string Guide(const std::string& state,
                    const std::string& instructions) override;
  void SetGuideResponse(std::string response) {
    guide_response_ = std::move(response);
  }

 private:
  std::uint64_t seed_;
  std::string guide_response_ = "{\"rules\":[]}";
};

using JevChoiceResult = LlmChoiceResult;
using JevClient = LlmClient;
using MockJevClient = MockLlmClient;

}  // namespace agent
}  // namespace gar::core::engine
