#include "gar/core/agent/learning/policy_trace.h"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace gar::core::engine::agent::learning {
namespace {

using Json = nlohmann::json;

Json ToJson(const Fact& fact) {
  return Json{{"kind", static_cast<std::uint32_t>(fact.kind)},
              {"pred", fact.pred},
              {"args", {fact.args[0], fact.args[1]}},
              {"arity", fact.arity}};
}

Fact ParseFact(const Json& value) {
  const auto kind = value.at("kind").get<std::uint32_t>();
  if (kind > static_cast<std::uint32_t>(FactKind::kMl)) {
    throw std::invalid_argument("policy trace contains an invalid fact kind");
  }
  Fact fact;
  fact.kind = static_cast<FactKind>(kind);
  fact.pred = value.at("pred").get<std::uint32_t>();
  const auto& args = value.at("args");
  if (!args.is_array() || args.size() != 2) {
    throw std::invalid_argument("policy trace fact args must contain two ids");
  }
  fact.args = {args[0].get<std::uint32_t>(), args[1].get<std::uint32_t>()};
  fact.arity = value.at("arity").get<std::uint32_t>();
  if (fact.arity > 2) {
    throw std::invalid_argument("policy trace fact arity exceeds two");
  }
  return fact;
}

Json ToJson(const Goal& goal) {
  return Json{{"slot", goal.slot},
              {"args",
               {goal.args[0] ? Json(*goal.args[0]) : Json(nullptr),
                goal.args[1] ? Json(*goal.args[1]) : Json(nullptr)}}};
}

Goal ParseGoal(const Json& value) {
  Goal goal;
  goal.slot = value.at("slot").get<std::uint32_t>();
  const auto& args = value.at("args");
  if (!args.is_array() || args.size() != 2) {
    throw std::invalid_argument(
        "policy trace goal args must contain two entries");
  }
  for (std::size_t i = 0; i < 2; ++i) {
    if (!args[i].is_null()) goal.args[i] = args[i].get<std::uint32_t>();
  }
  return goal;
}

Json ToJson(const CandidateSnapshot& candidate) {
  return Json{{"decision_id", candidate.decision_id},
              {"action_id", candidate.action_id},
              {"consequent", ToJson(candidate.consequent)},
              {"handcrafted_score", candidate.handcrafted_score},
              {"selected", candidate.selected}};
}

Json ToJson(const DerivationRecord& derivation) {
  Json premises = Json::array();
  for (const Fact& premise : derivation.premises)
    premises.push_back(ToJson(premise));
  return Json{{"action_id", derivation.action_id},
              {"consequent", ToJson(derivation.consequent)},
              {"premises", std::move(premises)}};
}

}  // namespace

void WritePolicyTrace(const std::filesystem::path& path,
                      const PolicyTrace& trace) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  Json candidates = Json::array();
  for (const CandidateSnapshot& candidate : trace.candidates) {
    candidates.push_back(ToJson(candidate));
  }
  Json derivations = Json::array();
  for (const DerivationRecord& derivation : trace.derivations) {
    derivations.push_back(ToJson(derivation));
  }
  const Json root{{"format", "gar_policy_trace_v1"},
                  {"goal", ToJson(trace.goal)},
                  {"hdc", {{"dim", trace.hdc.dim}, {"seed", trace.hdc.seed}}},
                  {"scorer",
                   {{"alpha", trace.scorer.alpha},
                    {"beta", trace.scorer.beta},
                    {"gamma", trace.scorer.gamma},
                    {"cost_weight", trace.scorer.cost_weight},
                    {"nu2", trace.scorer.nu2},
                    {"use_confidence", trace.scorer.use_confidence}}},
                  {"graph_fingerprint", trace.graph_fingerprint},
                  {"goal_derived", trace.goal_derived},
                  {"candidates", std::move(candidates)},
                  {"derivations", std::move(derivations)}};
  std::ofstream out(path);
  if (!out)
    throw std::runtime_error("cannot write policy trace: " + path.string());
  out << root.dump(2) << '\n';
  if (!out)
    throw std::runtime_error("failed to write policy trace: " + path.string());
}

PolicyTrace ReadPolicyTrace(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read policy trace: " + path.string());
  const Json root = Json::parse(in);
  if (root.at("format").get<std::string>() != "gar_policy_trace_v1") {
    throw std::invalid_argument("unsupported policy trace format: " +
                                path.string());
  }
  PolicyTrace trace;
  trace.goal = ParseGoal(root.at("goal"));
  trace.hdc.dim = root.at("hdc").at("dim").get<std::uint32_t>();
  trace.hdc.seed = root.at("hdc").at("seed").get<std::uint64_t>();
  if (trace.hdc.dim == 0) {
    throw std::invalid_argument("policy trace HDC dimension must be positive");
  }
  const auto& scorer = root.at("scorer");
  trace.scorer.alpha = scorer.at("alpha").get<double>();
  trace.scorer.beta = scorer.at("beta").get<double>();
  trace.scorer.gamma = scorer.at("gamma").get<double>();
  trace.scorer.cost_weight = scorer.at("cost_weight").get<double>();
  trace.scorer.nu2 = scorer.at("nu2").get<double>();
  trace.scorer.use_confidence = scorer.at("use_confidence").get<bool>();
  for (double value :
       {trace.scorer.alpha, trace.scorer.beta, trace.scorer.gamma,
        trace.scorer.cost_weight, trace.scorer.nu2}) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("policy trace scorer weights must be finite");
    }
  }
  trace.graph_fingerprint = root.at("graph_fingerprint").get<std::uint64_t>();
  trace.goal_derived = root.at("goal_derived").get<bool>();
  for (const auto& value : root.at("candidates")) {
    CandidateSnapshot candidate;
    candidate.decision_id = value.at("decision_id").get<std::uint64_t>();
    candidate.action_id = value.at("action_id").get<std::uint64_t>();
    candidate.consequent = ParseFact(value.at("consequent"));
    candidate.handcrafted_score = value.at("handcrafted_score").get<double>();
    if (!std::isfinite(candidate.handcrafted_score)) {
      throw std::invalid_argument(
          "policy trace candidate score must be finite");
    }
    candidate.selected = value.at("selected").get<bool>();
    trace.candidates.push_back(std::move(candidate));
  }
  for (const auto& value : root.at("derivations")) {
    DerivationRecord derivation;
    derivation.action_id = value.at("action_id").get<std::uint64_t>();
    derivation.consequent = ParseFact(value.at("consequent"));
    for (const auto& premise : value.at("premises")) {
      derivation.premises.push_back(ParseFact(premise));
    }
    trace.derivations.push_back(std::move(derivation));
  }
  return trace;
}

}  // namespace gar::core::engine::agent::learning
