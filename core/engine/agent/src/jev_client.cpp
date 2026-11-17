// HTTP transport + JSON for the LLM clients. Local Jev uses a minimal
// POSIX-socket HTTP/1.1 client; OpenAI-compatible endpoints use libcurl.
// JSON uses the vendored nlohmann/json (third_party/nlohmann, v3.11.2, MIT),
// keeping transport details out of the public header.

#include "gar/core/agent/jev_client.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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
  const auto hdr_end = resp.find("\r\n\r\n");
  if (status != 200) {
    std::string message = "jev: HTTP status " + std::to_string(status);
    if (hdr_end != std::string::npos) {
      message += ": " + resp.substr(hdr_end + 4, 2048);
    }
    throw std::runtime_error(message);
  }
  if (hdr_end == std::string::npos) {
    throw std::runtime_error("jev: malformed HTTP response");
  }
  return resp.substr(hdr_end + 4);
}

// Parses both a legacy Choice response and LLM2Jev's answers.pick wrapper.
// Probabilities are re-aligned to `options` (missing keys read 0).
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
  if (j.contains("answers") && j["answers"].is_object() &&
      j["answers"].contains("pick")) {
    j = j["answers"]["pick"];
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

double MockScore(const std::string& criteria) {
  const auto pos = criteria.find("guide_score:");
  if (pos == std::string::npos) return WitnessedRatio(criteria);
  const char* value = criteria.c_str() + pos + 12;
  char* end = nullptr;
  const double score = std::strtod(value, &end);
  return end == value ? 0.0 : score;
}

std::size_t WriteCurlResponse(char* data, std::size_t size,
                              std::size_t count, void* user_data) {
  const std::size_t bytes = size * count;
  auto* response = static_cast<std::string*>(user_data);
  if (response->size() + bytes > (1u << 22)) return 0;
  response->append(data, bytes);
  return bytes;
}

std::string OpenAiEndpoint(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  const std::string suffix = "/v1/chat/completions";
  if (base_url.size() >= suffix.size() &&
      base_url.compare(base_url.size() - suffix.size(), suffix.size(),
                       suffix) == 0) {
    return base_url;
  }
  if (base_url.size() >= 3 &&
      base_url.compare(base_url.size() - 3, 3, "/v1") == 0) {
    return base_url + "/chat/completions";
  }
  return base_url + suffix;
}

std::string OpenAiPost(const std::string& url, const std::string& body,
                       const std::string& api_key, std::uint32_t timeout_ms) {
  static std::once_flag initialized;
  std::call_once(initialized, []() {
    const CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (status != CURLE_OK) {
      throw std::runtime_error("openai: curl_global_init failed");
    }
  });
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(
      curl_easy_init(), &curl_easy_cleanup);
  if (!curl) throw std::runtime_error("openai: curl_easy_init failed");

  const std::string authorization = "Authorization: Bearer " + api_key;
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      curl_slist_append(nullptr, "Content-Type: application/json"),
      &curl_slist_free_all);
  if (!headers) throw std::runtime_error("openai: allocate headers failed");
  curl_slist* extended =
      curl_slist_append(headers.get(), authorization.c_str());
  if (extended == nullptr) {
    throw std::runtime_error("openai: allocate authorization failed");
  }
  if (extended != headers.get()) {
    headers.release();
    headers.reset(extended);
  }

  std::string response;
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_ms));
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCurlResponse);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  const CURLcode status = curl_easy_perform(curl.get());
  if (status != CURLE_OK) {
    throw std::runtime_error(std::string("openai: request failed: ") +
                             curl_easy_strerror(status));
  }
  long http_status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
  if (http_status != 200) {
    throw std::runtime_error("openai: HTTP status " +
                             std::to_string(http_status));
  }
  return response;
}

}  // namespace

JevChoiceResult HttpJevClient::Choice(
    const std::string& state, const std::string& instructions,
    const std::vector<std::string>& options,
    const std::vector<std::string>& criteria) {
  const Endpoint ep = ParseEndpoint(endpoint_);
  nlohmann::json body;
  if (!model_.empty()) {
    body["model"] = model_;
    body["include_choice_candidates"] = false;
  }
  body["state"] = state;
  nlohmann::json opts = nlohmann::json::object();
  for (std::size_t i = 0; i < options.size(); ++i) {
    opts[options[i]] = i < criteria.size() ? criteria[i] : "";
  }
  body["questions"] = {{"pick",
                        {{"type", "choice"},
                         {"instructions", instructions},
                         {"criteria", opts}}}};
  const std::string path = model_.empty() ? "/systemOne" : "/v1/systemone";
  return ParseChoice(HttpPost(ep, path, body.dump(), timeout_ms_), options);
}

JevChoiceResult OpenAiLlmClient::Choice(
    const std::string& state, const std::string& instructions,
    const std::vector<std::string>& options,
    const std::vector<std::string>& criteria) {
  const char* api_key = std::getenv("DEEPSEEK_API_KEY");
  if (api_key == nullptr || *api_key == '\0') {
    throw std::runtime_error("openai: DEEPSEEK_API_KEY is not set");
  }
  nlohmann::json opts = nlohmann::json::object();
  for (std::size_t i = 0; i < options.size(); ++i) {
    opts[options[i]] = i < criteria.size() ? criteria[i] : "";
  }
  nlohmann::json user{{"state", state}, {"options", opts}};
  nlohmann::json body{
      {"model", model_},
      {"messages",
       {{{"role", "system"},
         {"content", instructions +
                         " Return one JSON object with keys choice, "
                         "probabilities, and confidence. probabilities must "
                         "map every supplied option key to a nonnegative "
                         "number. Do not use Markdown."}},
        {{"role", "user"}, {"content", user.dump()}}}},
      {"response_format", {{"type", "json_object"}}},
      {"temperature", 0.0},
      {"max_tokens", 8192},
      {"stream", false}};
  const std::string response =
      OpenAiPost(OpenAiEndpoint(base_url_), body.dump(), api_key, timeout_ms_);
  nlohmann::json outer;
  try {
    outer = nlohmann::json::parse(response);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("openai: bad response JSON: ") +
                             e.what());
  }
  if (!outer.contains("choices") || !outer["choices"].is_array() ||
      outer["choices"].empty()) {
    throw std::runtime_error("openai: response has no choices");
  }
  const auto& choice = outer["choices"][0];
  if (!choice.contains("message") || !choice["message"].contains("content") ||
      !choice["message"]["content"].is_string()) {
    throw std::runtime_error("openai: response has no message content");
  }
  std::string content = choice["message"]["content"].get<std::string>();
  const std::size_t first = content.find('{');
  const std::size_t last = content.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last < first) {
    throw std::runtime_error("openai: message content is not a JSON object");
  }
  content = content.substr(first, last - first + 1);
  return ParseChoice(content, options);
}

LlmChoiceResult MockLlmClient::Choice(
    const std::string& state, const std::string& instructions,
    const std::vector<std::string>& options,
    const std::vector<std::string>& criteria) {
  (void)state;
  (void)instructions;
  JevChoiceResult out;
  out.probabilities.assign(options.size(), 0.0);
  // Deterministic per (seed, option key): a small multiplicative noise in
  // [0.95, 1.05) on top of the witnessed ratio or explicit guide score.
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
    const double p = (MockScore(crit) + 1e-6) * noise;
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
