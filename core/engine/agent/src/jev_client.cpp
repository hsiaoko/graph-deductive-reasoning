// HTTP transport + JSON for the Jev clients. Network code is a minimal
// POSIX-socket HTTP/1.1 client (local endpoints only: http://host:port,
// Content-Length responses, Connection: close); JSON uses the vendored
// nlohmann/json (third_party/nlohmann, v3.11.2, MIT). Keeping both here
// means no libcurl dependency and a transport-free public header.

#include "gar/engine/agent/jev_client.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace gar::core::engine {
namespace agent {

namespace {

struct Endpoint {
  std::string host;
  std::uint16_t port = 80;
};

Endpoint ParseEndpoint(const std::string& endpoint) {
  // Accepts "http://host:port" (or bare "host:port"); TLS is out of scope.
  std::string s = endpoint;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  if (s.rfind("https://", 0) == 0) {
    throw std::runtime_error("jev: https endpoints unsupported (local "
                             "plain-HTTP only)");
  }
  const auto slash = s.find('/');
  if (slash != std::string::npos) s = s.substr(0, slash);
  const auto colon = s.rfind(':');
  if (colon == std::string::npos) {
    throw std::runtime_error("jev: endpoint missing port: " + endpoint);
  }
  Endpoint ep;
  ep.host = s.substr(0, colon);
  ep.port = static_cast<std::uint16_t>(std::stoul(s.substr(colon + 1)));
  if (ep.host.empty()) throw std::runtime_error("jev: empty host");
  return ep;
}

// Blocking HTTP/1.1 POST with send/recv timeouts; returns the response
// body on HTTP 200, throws otherwise. Connection: close semantics — reads
// until EOF, no chunked decoding (the local Jev service answers with
// Content-Length).
std::string HttpPost(const Endpoint& ep, const std::string& path,
                     const std::string& body, std::uint32_t timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(ep.port);
  if (getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res) != 0) {
    throw std::runtime_error("jev: cannot resolve " + ep.host);
  }
  const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    throw std::runtime_error("jev: socket() failed");
  }
  const timeval tv{static_cast<time_t>(timeout_ms / 1000),
                   static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    const int e = errno;
    freeaddrinfo(res);
    close(fd);
    throw std::runtime_error("jev: connect failed: " + std::string(strerror(e)));
  }
  freeaddrinfo(res);

  std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + ep.host +
                    "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
  std::size_t sent = 0;
  while (sent < req.size()) {
    const ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
    if (n <= 0) {
      close(fd);
      throw std::runtime_error("jev: send failed (timeout or reset)");
    }
    sent += static_cast<std::size_t>(n);
  }
  std::string resp;
  char buf[8192];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) {
      close(fd);
      throw std::runtime_error("jev: recv failed (timeout or reset)");
    }
    resp.append(buf, static_cast<std::size_t>(n));
    if (resp.size() > (1u << 22)) {  // 4 MB sanity cap
      close(fd);
      throw std::runtime_error("jev: response too large");
    }
  }
  close(fd);

  const auto eol = resp.find("\r\n");
  const auto status = eol == std::string::npos
                          ? 0
                          : std::atoi(resp.substr(9, eol).c_str());
  if (status != 200) {
    throw std::runtime_error("jev: HTTP status " + std::to_string(status));
  }
  const auto hdr_end = resp.find("\r\n\r\n");
  if (hdr_end == std::string::npos) {
    throw std::runtime_error("jev: malformed HTTP response");
  }
  return resp.substr(hdr_end + 4);
}

// Parses a Jev Choice response: {"choice": <key>, "probabilities": {<key>:
// p, ...} | [p, ...], "confidence": c}. probabilities are re-aligned to
// `options` (missing keys read 0).
JevChoiceResult ParseChoice(const std::string& body,
                            const std::vector<std::string>& options) {
  JevChoiceResult out;
  out.probabilities.assign(options.size(), 0.0);
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("jev: bad JSON: ") + e.what());
  }
  if (j.contains("choice")) {
    out.choice = j["choice"].is_string() ? j["choice"].get<std::string>()
                                         : j["choice"].dump();
  }
  if (j.contains("confidence") && j["confidence"].is_number()) {
    out.confidence = j["confidence"].get<double>();
  }
  if (j.contains("probabilities")) {
    const auto& probs = j["probabilities"];
    if (probs.is_object()) {
      for (std::size_t i = 0; i < options.size(); ++i) {
        if (probs.contains(options[i]) &&
            probs[options[i]].is_number()) {
          out.probabilities[i] = probs[options[i]].get<double>();
        }
      }
    } else if (probs.is_array()) {
      for (std::size_t i = 0; i < options.size() && i < probs.size(); ++i) {
        if (probs[i].is_number()) out.probabilities[i] = probs[i].get<double>();
      }
    }
  }
  return out;
}

// Reads "x/y" from a criteria line ("witnessed: x/y" marker); ratio in
// [0,1], 0 when the marker is absent.
double WitnessedRatio(const std::string& criteria) {
  const auto pos = criteria.find("witnessed:");
  if (pos == std::string::npos) return 0.0;
  const char* p = criteria.c_str() + pos + 11;
  char* end = nullptr;
  const double x = std::strtod(p, &end);
  if (end == p || *end != '/') return 0.0;
  const double y = std::strtod(end + 1, nullptr);
  return y > 0 ? x / y : 0.0;
}

}  // namespace

JevChoiceResult HttpJevClient::Choice(
    const std::string& state, const std::string& instructions,
    const std::vector<std::string>& options,
    const std::vector<std::string>& criteria) {
  const Endpoint ep = ParseEndpoint(endpoint_);
  nlohmann::json body;
  body["state"] = state;
  nlohmann::json opts = nlohmann::json::object();
  for (std::size_t i = 0; i < options.size(); ++i) {
    opts[options[i]] = i < criteria.size() ? criteria[i] : "";
  }
  body["questions"] = {{"pick",
                        {{"type", "choice"},
                         {"instructions", instructions},
                         {"options", opts}}}};
  return ParseChoice(HttpPost(ep, "/systemOne", body.dump(), timeout_ms_),
                     options);
}

JevChoiceResult MockJevClient::Choice(
    const std::string& state, const std::string& instructions,
    const std::vector<std::string>& options,
    const std::vector<std::string>& criteria) {
  (void)state;
  (void)instructions;
  JevChoiceResult out;
  out.probabilities.assign(options.size(), 0.0);
  // Deterministic per (seed, option key): a small multiplicative noise in
  // [0.95, 1.05) on top of the witnessed ratio.
  double best = -1.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string& crit = i < criteria.size() ? criteria[i] : "";
    std::uint64_t h = seed_;
    for (const char c : options[i]) h = (h ^ c) * 0x100000001B3ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    const double noise = 0.95 + 0.10 * (h % 1000) / 1000.0;
    const double p = (WitnessedRatio(crit) + 1e-6) * noise;
    out.probabilities[i] = p;
    sum += p;
    if (p > best) {
      best = p;
      out.choice = options[i];
      out.confidence = p;  // normalized below
    }
  }
  if (sum > 0) {
    for (auto& p : out.probabilities) p /= sum;
    out.confidence = best / sum;
  }
  return out;
}

}  // namespace agent
}  // namespace gar::core::engine
