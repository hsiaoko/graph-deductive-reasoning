#pragma once

// JevClient: client of the Jev System One decision model (paper §5.3 — a
// local HTTP endpoint; "rank, not invent": candidate generation and all
// logical checks stay symbolic, the model only ranks). One call = one
// state text + one Choice question.
//
// The header exposes ONLY the interface: HTTP transport, JSON
// (de)serialization and the network itself live in jev_client.cpp, so the
// HTTP implementation and MockJevClient stay interchangeable for callers.

#include <cstdint>
#include <string>
#include <vector>

namespace gar::core::engine {
namespace agent {

struct JevChoiceResult {
  std::string choice;                 // the chosen option key
  std::vector<double> probabilities;  // aligned with `options`
  double confidence = 0.0;
};

class JevClient {
 public:
  virtual ~JevClient() = default;
  // `options` are opaque keys (we pass "0".."k-1"); `criteria` is the
  // per-option profile text, parallel to `options`.
  virtual JevChoiceResult Choice(const std::string& state,
                                 const std::string& instructions,
                                 const std::vector<std::string>& options,
                                 const std::vector<std::string>& criteria) = 0;
};

// HTTP client for a locally deployed Jev endpoint (http://host:port, plain
// HTTP/1.1 — no TLS, no chunked encoding needed against a local service).
// Throws std::runtime_error on connect/timeout/status/parse failures; the
// caller (JevPolicy) treats any throw as a fallback trigger.
class HttpJevClient final : public JevClient {
 public:
  HttpJevClient(std::string endpoint, std::uint32_t timeout_ms)
      : endpoint_(std::move(endpoint)), timeout_ms_(timeout_ms) {}

  JevChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override;

 private:
  std::string endpoint_;
  std::uint32_t timeout_ms_;
};

// MockJevClient: a network-free stand-in for development and regression —
// THE support point of the verification suite, not a throwaway. It scores
// each option by the "witnessed: x/y" ratio found in its criteria text
// (higher ratio -> higher probability), multiplied by a fixed-seed noise
// factor, so results are fully deterministic per (seed, inputs).
class MockJevClient final : public JevClient {
 public:
  explicit MockJevClient(std::uint64_t seed) : seed_(seed) {}

  JevChoiceResult Choice(const std::string& state,
                         const std::string& instructions,
                         const std::vector<std::string>& options,
                         const std::vector<std::string>& criteria) override;

 private:
  std::uint64_t seed_;
};

}  // namespace agent
}  // namespace gar::core::engine
