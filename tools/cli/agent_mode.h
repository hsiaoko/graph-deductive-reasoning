#pragma once

// gar_reason --mode agent: policy-guided goal deduction (paper §3-§5).
// Argument parsing, execution and output
// live here; the engine itself is engine/agent/. The
// --verify-against-closure cross-check composes gar::core::engine::deduction at
// the app layer — the agent library never sees its sibling engine.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "full_deduce_cli.h"  // FullDeduceArgs + loaders + closure_output.h
#include "gar/core/agent/executor.h"
#include "gar/core/agent/fact_encoder.h"
#include "gar/core/agent/learning/learned_policy.h"
#include "gar/core/agent/learning/relevance_model.h"
#include "gar/core/agent/reasoning_agent.h"
#include "gar/core/agent/value_model.h"
#include "gar/core/analysis/dependency_graph.h"
#include "gar/core/chase/cpu_chase_engine.h"
#include "gar/core/hdc/const.h"

namespace gar_apps {

struct AgentModeArgs {
  std::string goal_spec;  // "rel_name(src,dst)"; '_' = open argument
  gar::core::engine::agent::MatchEngine engine =
      gar::core::engine::agent::MatchEngine::kCpu;
  gar::core::engine::agent::SearchDirection search_direction =
      gar::core::engine::agent::SearchDirection::kForward;
  std::uint32_t backward_max_depth = 64;
  std::uint64_t backward_max_calls = 100000;
  std::uint64_t bidirectional_forward_budget = 2000;
  std::uint32_t bidirectional_rounds = 8;
  std::string strategy_model_path;
  std::uint64_t budget = 100;
  std::uint32_t batch_size = 1;
  gar::core::engine::agent::PolicyKind policy =
      gar::core::engine::agent::PolicyKind::kDeterministic;
  gar::core::engine::agent::ScorerKind scorer =
      gar::core::engine::agent::ScorerKind::kExact;
  // Longest-waiting fair scheduling interval (0=off).
  std::uint32_t fair_interval = 0;
  std::uint64_t revival_threshold = 0;
  gar::core::engine::agent::StarOrder star_order =
      gar::core::engine::agent::StarOrder::kInput;
  std::string star_order_model_path;
  std::uint32_t hdc_dim = gar::core::hdc::kHdcDefaultDim;
  std::uint64_t seed = 42;
  // Seed decoupling (--policy-seed / --hdc-seed; both fall back to --seed).
  // The HDC encoder uses the hdc seed; every policy-side RNG
  // (RandomizedPolicy, JEV mock) uses the policy seed.
  std::optional<std::uint64_t> policy_seed;
  std::optional<std::uint64_t> hdc_seed;
  [[nodiscard]] std::uint64_t PolicySeed() const {
    return policy_seed.value_or(seed);
  }
  [[nodiscard]] std::uint64_t HdcSeed() const {
    return hdc_seed.value_or(seed);
  }
  std::uint32_t max_candidates_per_rule = 256;
  gar::core::engine::agent::HandcraftedConfig score;
  std::string predicate_prior_path;
  bool verify_against_closure = false;
  // Ablation: --no-dg removes dependency-graph guidance (d(.) table all
  // zeros: no candidate hard-filter, rel_fam neutral, price = w0). The DG
  // itself is still built (failure classification reads its in-degrees).
  bool no_dg = false;
  // Jev (LLM decision model) policy knobs; --jev-mock swaps the HTTP client
  // for the network-free MockJevClient (development/regression support).
  gar::core::engine::agent::JevConfig jev;
  bool jev_mock = false;
  bool llm_guide = false;
  bool llm_mock = false;
  bool paper_guide = false;
  double guide_lambda = 1.0;
  std::string paper_guide_response;
  std::string paper_guide_cache_dir;
  double guide_strength = 1.0;
  std::size_t guide_max_options = 255;
  double guide_min_cost = 0.0;
  std::string llm_backend = "openai";  // openai|jev
  std::string llm_base_url = "https://api.deepseek.com";
  std::string llm_model = "deepseek-chat";
  std::uint32_t llm_timeout_ms = 120000;
  double wall_limit_sec = 0.0;
  std::string relevance_model;
  double learned_handcrafted_weight = 0.0;
  double learned_relevance_weight = 1.0;
  std::uint32_t learned_prefilter = 0;
  std::string proof_star_model;
  double proof_star_weight = 1.0;
  double proof_star_min_cost = 0.0;
  std::string decision_trace_path;
  std::uint64_t decision_sample_stride = 0;
  std::uint32_t decision_alternative_count = 0;
  std::string policy_trace_path;
  std::uint32_t policy_trace_negative_count = 256;
  std::string replay_actions_path;
  std::vector<gar::core::engine::agent::ReplayAction> replay_actions;
  // Multi-goal execution: independent goals share one process and DG, while
  // each worker owns its graph delta state.
  std::string tasks_path;
  std::string ground_truth_dir;
  std::uint32_t workers = 1;
};

inline gar::core::engine::agent::MatchEngine ParseMatchEngine(
    const std::string& value) {
  using gar::core::engine::agent::MatchEngine;
  if (value == "cpu") return MatchEngine::kCpu;
  if (value == "gpu") return MatchEngine::kGpu;
  throw std::invalid_argument("unknown --engine: " + value + " (cpu|gpu)");
}

inline gar::core::engine::agent::SearchDirection ParseSearchDirection(
    const std::string& value) {
  using gar::core::engine::agent::SearchDirection;
  if (value == "forward") return SearchDirection::kForward;
  if (value == "backward") return SearchDirection::kBackward;
  if (value == "hybrid") return SearchDirection::kHybrid;
  if (value == "learned-strategy") return SearchDirection::kLearnedStrategy;
  if (value == "bidirectional") return SearchDirection::kBidirectional;
  throw std::invalid_argument("unknown --search-direction: " + value +
                              " (forward|backward|hybrid|learned-strategy)");
}

inline gar::core::engine::agent::PolicyKind ParsePolicyKind(
    const std::string& value) {
  using gar::core::engine::agent::PolicyKind;
  if (value == "deterministic") return PolicyKind::kDeterministic;
  if (value == "random") return PolicyKind::kRandom;
  if (value == "uniform-random") return PolicyKind::kUniformRandom;
  if (value == "hybrid") return PolicyKind::kHybrid;
  if (value == "jev") return PolicyKind::kJev;
  if (value == "learned") return PolicyKind::kLearned;
  if (value == "adaptive") return PolicyKind::kAdaptive;
  if (value == "replay") return PolicyKind::kReplay;
  throw std::invalid_argument("unknown --policy: " + value);
}

inline gar::core::engine::agent::ScorerKind ParseScorerKind(
    const std::string& value) {
  using gar::core::engine::agent::ScorerKind;
  if (value == "exact") return ScorerKind::kExact;
  if (value == "null") return ScorerKind::kNull;
  if (value == "hdc") return ScorerKind::kHdc;
  throw std::invalid_argument("unknown --scorer: " + value +
                              " (exact|hdc|null)");
}

inline gar::core::engine::agent::StarOrder ParseStarOrder(
    const std::string& value) {
  using gar::core::engine::agent::StarOrder;
  if (value == "input") return StarOrder::kInput;
  if (value == "goal") return StarOrder::kGoal;
  if (value == "learned") return StarOrder::kLearned;
  throw std::invalid_argument("unknown --star-order: " + value +
                              " (input|goal|learned)");
}

inline gar::core::engine::agent::ExecConfig MakeExecConfig(
    const AgentModeArgs& args) {
  gar::core::engine::agent::ExecConfig config;
  config.budget = args.budget;
  config.batch_size = args.batch_size;
  config.engine = args.engine;
  config.match_cap_per_rule = args.max_candidates_per_rule;
  config.wall_limit_sec = args.wall_limit_sec;
  config.fair_interval = args.fair_interval;
  config.revival_threshold = args.revival_threshold;
  config.star_order = args.star_order;
  config.star_order_model_path = args.star_order_model_path;
  config.guide_strength = args.llm_guide ? args.guide_strength : 0.0;
  config.proof_star_weight =
      args.proof_star_model.empty() ? 0.0 : args.proof_star_weight;
  config.proof_star_min_cost = args.proof_star_min_cost;
  config.decision_trace_path = args.decision_trace_path;
  config.decision_sample_stride = args.decision_sample_stride;
  config.decision_alternative_count = args.decision_alternative_count;
  config.policy_trace_path = args.policy_trace_path;
  config.policy_trace_negative_count = args.policy_trace_negative_count;
  config.backward_max_depth = args.backward_max_depth;
  config.backward_max_calls = args.backward_max_calls;
  config.bidirectional_forward_budget = args.bidirectional_forward_budget;
  config.bidirectional_rounds = args.bidirectional_rounds;
  config.strategy_model_path = args.strategy_model_path;
  config.seed = args.PolicySeed();
  return config;
}

// Parses one agent-mode option; returns false when `k` is not one.
template <typename NextFn>
bool ParseAgentModeOption(const std::string& k, NextFn&& next,
                          AgentModeArgs* a) {
  if (k == "--goal")
    a->goal_spec = next();
  else if (k == "--tasks")
    a->tasks_path = next();
  else if (k == "--workers")
    a->workers = std::stoul(next());
  else if (k == "--gt")
    a->ground_truth_dir = next();
  else if (k == "--engine")
    a->engine = ParseMatchEngine(next());
  else if (k == "--search-direction")
    a->search_direction = ParseSearchDirection(next());
  else if (k == "--backward-max-depth")
    a->backward_max_depth = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--backward-max-calls")
    a->backward_max_calls = std::stoull(next());
  else if (k == "--bidirectional-forward-budget")
    a->bidirectional_forward_budget = std::stoull(next());
  else if (k == "--bidirectional-rounds")
    a->bidirectional_rounds = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--strategy-model")
    a->strategy_model_path = next();
  else if (k == "--budget")
    a->budget = std::stoull(next());
  else if (k == "--batch-size") {
    a->batch_size = static_cast<std::uint32_t>(std::stoul(next()));
    if (a->batch_size == 0) {
      throw std::invalid_argument("--batch-size must be positive");
    }
  } else if (k == "--policy")
    a->policy = ParsePolicyKind(next());
  else if (k == "--scorer")
    a->scorer = ParseScorerKind(next());
  else if (k == "--hdc-dim")
    a->hdc_dim = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--seed")
    a->seed = std::stoull(next());
  else if (k == "--policy-seed")
    a->policy_seed = std::stoull(next());
  else if (k == "--hdc-seed")
    a->hdc_seed = std::stoull(next());
  else if (k == "--max-candidates-per-rule")
    a->max_candidates_per_rule = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--alpha")
    a->score.alpha = std::stod(next());
  else if (k == "--beta" || k == "--nu")
    a->score.beta = std::stod(next());
  else if (k == "--lambda")
    a->score.cost_weight = std::stod(next());
  else if (k == "--gamma")
    a->score.gamma = std::stod(next());
  else if (k == "--predicate-prior")
    a->predicate_prior_path = next();
  else if (k == "--nu2")
    a->score.nu2 = std::stod(next());
  else if (k == "--use-confidence")
    a->score.use_confidence = true;
  else if (k == "--no-confidence")
    a->score.use_confidence = false;
  else if (k == "--no-dg")
    a->no_dg = true;
  else if (k == "--verify-against-closure")
    a->verify_against_closure = true;
  else if (k == "--jev-endpoint")
    a->jev.endpoint = next();
  else if (k == "--jev-timeout-ms")
    a->jev.timeout_ms = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--jev-top-k")
    a->jev.top_k = static_cast<std::size_t>(std::stoull(next()));
  else if (k == "--llm-guide")
    a->llm_guide = true;
  else if (k == "--llm-mock")
    a->llm_mock = true;
  else if (k == "--paper-guide")
    a->paper_guide = true;
  else if (k == "--paper-guide-response")
    a->paper_guide_response = next();
  else if (k == "--paper-guide-cache-dir")
    a->paper_guide_cache_dir = next();
  else if (k == "--guide-lambda") {
    a->guide_lambda = std::stod(next());
    if (a->guide_lambda < 0.0 || a->guide_lambda > 1.0) {
      throw std::invalid_argument("--guide-lambda must be in [0,1]");
    }
  } else if (k == "--llm-backend") {
    a->llm_backend = next();
    if (a->llm_backend != "openai" && a->llm_backend != "jev") {
      throw std::invalid_argument("unknown --llm-backend: " + a->llm_backend +
                                  " (openai|jev)");
    }
  } else if (k == "--llm-base-url")
    a->llm_base_url = next();
  else if (k == "--llm-model")
    a->llm_model = next();
  else if (k == "--llm-timeout-ms")
    a->llm_timeout_ms = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--guide-strength") {
    a->guide_strength = std::stod(next());
    if (a->guide_strength < 0.0) {
      throw std::invalid_argument("--guide-strength must be nonnegative");
    }
  } else if (k == "--guide-max-options") {
    a->guide_max_options = std::stoull(next());
    if (a->guide_max_options == 0) {
      throw std::invalid_argument("--guide-max-options must be positive");
    }
  } else if (k == "--guide-min-cost") {
    a->guide_min_cost = std::stod(next());
    if (a->guide_min_cost < 0.0) {
      throw std::invalid_argument("--guide-min-cost must be nonnegative");
    }
  } else if (k == "--wall-limit")
    a->wall_limit_sec = std::stod(next());
  else if (k == "--fair-interval")
    a->fair_interval = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--revival-threshold")
    a->revival_threshold = std::stoull(next());
  else if (k == "--star-order")
    a->star_order = ParseStarOrder(next());
  else if (k == "--star-order-model")
    a->star_order_model_path = next();
  else if (k == "--relevance-model")
    a->relevance_model = next();
  else if (k == "--learned-handcrafted-weight")
    a->learned_handcrafted_weight = std::stod(next());
  else if (k == "--learned-relevance-weight")
    a->learned_relevance_weight = std::stod(next());
  else if (k == "--learned-prefilter")
    a->learned_prefilter = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--proof-star-model")
    a->proof_star_model = next();
  else if (k == "--proof-star-weight") {
    a->proof_star_weight = std::stod(next());
    if (a->proof_star_weight < 0.0) {
      throw std::invalid_argument("--proof-star-weight must be nonnegative");
    }
  } else if (k == "--proof-star-min-cost") {
    a->proof_star_min_cost = std::stod(next());
    if (a->proof_star_min_cost < 0.0) {
      throw std::invalid_argument("--proof-star-min-cost must be nonnegative");
    }
  } else if (k == "--decision-trace")
    a->decision_trace_path = next();
  else if (k == "--decision-stride")
    a->decision_sample_stride = std::stoull(next());
  else if (k == "--decision-alternatives")
    a->decision_alternative_count =
        static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--policy-trace")
    a->policy_trace_path = next();
  else if (k == "--policy-trace-negatives")
    a->policy_trace_negative_count =
        static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--replay-actions")
    a->replay_actions_path = next();
  else if (k == "--jev-sample")
    a->jev.sample = true;
  else if (k == "--jev-mock")
    a->jev_mock = true;
  else
    return false;
  return true;
}

// Parses "rel_name(src,dst)": the relation name resolves to a base slot
// through the graph's relation dictionary; arguments are entity names
// resolved through the entity dictionary, or '_' for an open variable.
inline gar::core::engine::agent::Goal ParseGoalSpec(
    const std::string& spec, const gar::core::graph::Graph& g) {
  const auto lp = spec.find('(');
  const auto rp = spec.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
    throw std::invalid_argument("bad --goal (want rel_name(src,dst)): " + spec);
  }
  const std::string rel = spec.substr(0, lp);
  const std::string args = spec.substr(lp + 1, rp - lp - 1);
  const auto comma = args.find(',');
  if (comma == std::string::npos) {
    throw std::invalid_argument("bad --goal (want two arguments): " + spec);
  }
  const std::string a0 = args.substr(0, comma);
  const std::string a1 = args.substr(comma + 1);

  gar::core::engine::agent::Goal goal;
  bool found = false;
  for (std::uint32_t r = 0; r < g.num_relations(); ++r) {
    if (g.RelationName(r) == rel) {
      goal.slot = r;
      found = true;
      break;
    }
  }
  if (!found) {
    throw std::invalid_argument("unknown relation in --goal: " + rel);
  }
  auto resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (name == "_") return std::nullopt;
    for (std::uint32_t v = 0; v < g.num_entities(); ++v) {
      if (g.EntityName(v) == name) return v;
    }
    throw std::invalid_argument("unknown entity in --goal: " + name);
  };
  goal.args = {resolve(a0), resolve(a1)};
  return goal;
}

inline std::vector<gar::core::engine::agent::ReplayAction> ReadReplayActions(
    const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open replay actions: " + path);
  std::vector<gar::core::engine::agent::ReplayAction> actions;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::size_t id_end = line.find('\t');
    const std::size_t rule_end = line.find('\t', id_end + 1);
    if (id_end == std::string::npos || rule_end == std::string::npos) {
      throw std::invalid_argument("bad replay action row");
    }
    gar::core::engine::agent::ReplayAction action;
    action.action_id = std::stoull(line.substr(0, id_end));
    action.motif.rule = static_cast<std::uint32_t>(
        std::stoul(line.substr(id_end + 1, rule_end - id_end - 1)));
    std::size_t begin = rule_end + 1;
    while (begin <= line.size()) {
      const std::size_t comma = line.find(',', begin);
      const std::size_t end = comma == std::string::npos ? line.size() : comma;
      action.motif.embedding.push_back(static_cast<std::uint32_t>(
          std::stoul(line.substr(begin, end - begin))));
      if (comma == std::string::npos) break;
      begin = comma + 1;
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

// Builds the agent composition root from typed CLI configuration. Shared by
// gar_reason and gar_analyze so every entry point uses identical wiring.
inline gar::core::engine::agent::ReasoningAgent MakeReasoningAgent(
    const AgentModeArgs& args, const gar::core::analysis::DependencyGraph* dg,
    const gar::core::engine::agent::Goal* goal = nullptr,
    const gar::core::graph::Graph* graph = nullptr,
    const std::vector<gar::GARule>* rules = nullptr,
    const std::vector<std::string>* rule_ids = nullptr,
    gar::core::engine::agent::JevPolicy** jev_out = nullptr,
    gar::core::engine::agent::LlmStarGuide** guide_out = nullptr,
    gar::core::engine::agent::PaperGuide** paper_guide_out = nullptr) {
  namespace agent = gar::core::engine::agent;
  const gar::core::hdc::HdcEncoder encoder({args.hdc_dim, args.HdcSeed()});
  agent::ReasoningAgent::Components comps;
  comps.cost_model = std::make_unique<agent::UnitCostModel>();
  switch (args.scorer) {
    case agent::ScorerKind::kExact:
      comps.scorer = std::make_unique<agent::ExactScorer>(
          args.score, comps.cost_model.get());
      break;
    case agent::ScorerKind::kNull:
      comps.scorer = std::make_unique<agent::NullScorer>();
      break;
    case agent::ScorerKind::kHdc:
      comps.scorer = std::make_unique<agent::HdcScorer>(
          args.score, comps.cost_model.get(), encoder);
      break;
  }
  if (args.policy == agent::PolicyKind::kJev) {
    if (goal == nullptr || graph == nullptr || rules == nullptr) {
      throw std::invalid_argument(
          "--policy jev needs goal/graph/rules "
          "context (MakeReasoningAgent)");
    }
    std::unique_ptr<agent::JevClient> client;
    if (args.jev_mock) {
      client = std::make_unique<agent::MockJevClient>(args.PolicySeed());
    } else {
      client = std::make_unique<agent::HttpJevClient>(args.jev.endpoint,
                                                      args.jev.timeout_ms);
    }
    auto jev = std::make_unique<agent::JevPolicy>(
        args.jev, std::move(client), goal, graph, dg, rules, rule_ids,
        static_cast<double>(args.budget));
    if (jev_out != nullptr) *jev_out = jev.get();
    comps.policy = std::move(jev);
  } else if (args.policy == agent::PolicyKind::kReplay) {
    if (args.replay_actions.empty()) {
      throw std::invalid_argument("--policy replay requires --replay-actions");
    }
    if (args.batch_size != 1) {
      throw std::invalid_argument(
          "--policy replay currently requires --batch-size 1");
    }
    comps.policy = std::make_unique<agent::ReplayPolicy>(args.replay_actions);
  } else if (args.policy == agent::PolicyKind::kRandom) {
    comps.policy = std::make_unique<agent::RandomizedPolicy>(args.PolicySeed());
  } else if (args.policy == agent::PolicyKind::kUniformRandom) {
    comps.policy = std::make_unique<agent::UniformRandomPolicy>(args.PolicySeed());
  } else if (args.policy == agent::PolicyKind::kHybrid) {
    comps.policy = std::make_unique<agent::HybridNeuroSymbolicPolicy>();
  } else if (args.policy == agent::PolicyKind::kLearned ||
             args.policy == agent::PolicyKind::kAdaptive) {
    if (goal == nullptr || graph == nullptr || rules == nullptr) {
      throw std::invalid_argument(
          "learned policies need goal/graph/rules context");
    }
    if (args.relevance_model.empty()) {
      throw std::invalid_argument(std::string("--policy ") +
                                  agent::PolicyName(args.policy) +
                                  " requires --relevance-model PATH");
    }
    if (args.policy == agent::PolicyKind::kAdaptive && !args.llm_guide) {
      throw std::invalid_argument("--policy adaptive requires --llm-guide");
    }
    if (args.scorer != agent::ScorerKind::kExact) {
      throw std::invalid_argument("learned policies require --scorer exact");
    }
    if (args.search_direction != agent::SearchDirection::kForward) {
      throw std::invalid_argument("learned policies require forward deduction");
    }
    auto model = std::make_shared<agent::learning::RelevanceModel>(
        agent::learning::RelevanceModel::Load(args.relevance_model));
    const auto& metadata = model->metadata();
    if (metadata.dim != args.hdc_dim || metadata.hdc_seed != args.HdcSeed() ||
        metadata.graph_fingerprint != agent::GraphFingerprint(*graph)) {
      throw std::invalid_argument(
          "relevance model does not match the graph or HDC configuration");
    }
    if (!std::isfinite(args.learned_handcrafted_weight) ||
        !std::isfinite(args.learned_relevance_weight) ||
        args.learned_handcrafted_weight < 0.0 ||
        args.learned_relevance_weight < 0.0 ||
        (args.learned_handcrafted_weight == 0.0 &&
         args.learned_relevance_weight == 0.0)) {
      throw std::invalid_argument("learned weights must be finite and nonnegative");
    }
    agent::FactEncoder fact_encoder(encoder, graph, &dg->table());
    auto learned = std::make_unique<agent::learning::LearnedPolicy>(
        model.get(), fact_encoder, *goal, rules,
        args.learned_handcrafted_weight, args.learned_relevance_weight,
        args.learned_prefilter > 0);
    if (args.policy == agent::PolicyKind::kAdaptive) {
      comps.policy = std::make_unique<agent::AdaptivePolicy>(
          std::make_unique<agent::DeterministicPolicy>(), std::move(learned));
    } else {
      comps.policy = std::move(learned);
    }
    comps.relevance_model = std::move(model);
  } else {
    comps.policy = std::make_unique<agent::DeterministicPolicy>();
  }
  if (!args.proof_star_model.empty()) {
    auto model = std::make_shared<agent::ValueModel>();
    if (!model->Load(args.proof_star_model)) {
      throw std::runtime_error("failed to load proof-star model: " +
                               args.proof_star_model);
    }
    if (model->kind() != "numeric") {
      throw std::invalid_argument("proof-star model must be numeric");
    }
    comps.proof_star_model = std::move(model);
  }
  if (args.llm_guide) {
    if (goal == nullptr || graph == nullptr || rules == nullptr) {
      throw std::invalid_argument("--llm-guide needs goal/graph/rules context");
    }
    std::unique_ptr<agent::LlmClient> client;
    if (args.llm_mock) {
      client = std::make_unique<agent::MockLlmClient>(args.PolicySeed());
    } else if (args.llm_backend == "jev") {
      client = std::make_unique<agent::HttpJevClient>(
          args.llm_base_url, args.llm_timeout_ms, args.llm_model);
    } else {
      client = std::make_unique<agent::OpenAiLlmClient>(
          args.llm_base_url, args.llm_model, args.llm_timeout_ms);
    }
    auto guide = std::make_unique<agent::LlmStarGuide>(
        std::move(client), goal, graph, dg, rules, rule_ids,
        args.guide_max_options, args.guide_min_cost);
    if (args.policy == agent::PolicyKind::kAdaptive) {
      static_cast<agent::AdaptivePolicy*>(comps.policy.get())
          ->SetGuide(guide.get());
    }
    if (guide_out != nullptr) *guide_out = guide.get();
    comps.star_guide = std::move(guide);
  }
  if (args.paper_guide) {
    if (goal == nullptr || graph == nullptr || rules == nullptr) {
      throw std::invalid_argument(
          "--paper-guide needs goal/graph/rules context");
    }
    std::unique_ptr<agent::LlmClient> client;
    if (args.llm_mock) {
      auto mock = std::make_unique<agent::MockLlmClient>(args.PolicySeed());
      if (!args.paper_guide_response.empty()) {
        std::ifstream input(args.paper_guide_response);
        if (!input) {
          throw std::runtime_error("cannot read paper guide response: " +
                                   args.paper_guide_response);
        }
        const std::string response((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        mock->SetGuideResponse(response);
      }
      client = std::move(mock);
    } else {
      if (!args.paper_guide_response.empty()) {
        throw std::invalid_argument(
            "--paper-guide-response requires --llm-mock");
      }
      if (args.llm_backend == "jev") {
        client = std::make_unique<agent::HttpJevClient>(
            args.llm_base_url, args.llm_timeout_ms, args.llm_model);
      } else {
        client = std::make_unique<agent::OpenAiLlmClient>(
            args.llm_base_url, args.llm_model, args.llm_timeout_ms);
      }
    }
    if (!args.paper_guide_cache_dir.empty()) {
      const std::string goal_key =
          graph->RelationName(goal->slot) + "(" +
          (goal->args[0] ? graph->EntityName(*goal->args[0]) : "_") + "," +
          (goal->args[1] ? graph->EntityName(*goal->args[1]) : "_") + ")";
      std::uint64_t hash = 14695981039346656037ULL;
      for (const unsigned char c : goal_key) {
        hash = (hash ^ c) * 1099511628211ULL;
      }
      const auto directory = std::filesystem::path(args.paper_guide_cache_dir);
      std::filesystem::create_directories(directory);
      const auto path = directory / (std::to_string(hash) + ".json");
      client = std::make_unique<agent::CachedLlmClient>(path.string(),
                                                        std::move(client));
    }
    auto guide = std::make_unique<agent::PaperGuide>(
        std::move(client), goal, graph, dg, rules, rule_ids);
    comps.policy = std::make_unique<agent::GuidedPolicy>(
        std::move(comps.policy), guide.get(), goal, graph, dg, rules, rule_ids,
        args.guide_lambda, args.PolicySeed());
    if (paper_guide_out != nullptr) *paper_guide_out = guide.get();
    comps.paper_guide = std::move(guide);
  }
  if (args.search_direction == agent::SearchDirection::kBackward) {
    comps.executor = std::make_unique<agent::BackwardExecutor>();
  } else if (args.search_direction == agent::SearchDirection::kHybrid) {
    comps.executor = std::make_unique<agent::HybridExecutor>();
  } else if (args.search_direction == agent::SearchDirection::kLearnedStrategy) {
    comps.executor = std::make_unique<agent::LearnedStrategyExecutor>();
  } else if (args.search_direction == agent::SearchDirection::kBidirectional) {
    comps.executor = std::make_unique<agent::BidirectionalExecutor>();
  } else {
    comps.executor = std::make_unique<agent::AdaDeduceExecutor>();
  }
  return agent::ReasoningAgent(std::move(comps), dg, encoder, !args.no_dg);
}

// Renders a derived fact as "rel(src,dst)" with entity names.
inline std::string FormatFact(const gar::core::graph::Graph& g,
                              const gar::core::graph::DerivedEdge& e) {
  return g.SlotName(e.slot) + "(" + g.EntityName(e.src) + "," +
         g.EntityName(e.dst) + ")";
}

inline int RunAgentMode(const FullDeduceArgs& io, const AgentModeArgs& args) {
  namespace agent = gar::core::engine::agent;

  // ---- load (same sources as the full mode) ----
  gar::core::graph::Graph g =
      gar::core::graph::LoadGraph(io.csr_dir, /*with_inverses=*/true);
  std::cerr << "[gar_reason] |V|=" << g.num_entities()
            << " |R|=" << g.num_relations() << " |E|=" << g.num_edges() << "\n";
  std::vector<std::filesystem::path> rule_dirs;
  for (const auto& e : std::filesystem::directory_iterator(io.rules_root)) {
    if (e.is_directory() && std::filesystem::exists(e.path() / "rule.yaml")) {
      rule_dirs.push_back(e.path());
    }
  }
  std::sort(rule_dirs.begin(), rule_dirs.end());
  std::vector<gar::GARule> rules;
  std::vector<std::string> rule_ids;
  for (const auto& d : rule_dirs) {
    rules.push_back(gar::RuleLoader::Load(d));
    rule_ids.push_back(d.filename().string());
  }
  std::cerr << "[gar_reason] " << rules.size() << " rules\n";
  gar::core::graph::VertexAttributes attrs =
      gar::core::graph::LoadVertexAttributes(io.csr_dir);
  std::cerr << "[gar_reason] " << attrs.columns.size()
            << " numeric attribute columns\n";
  auto ml_models = LoadMlModelRegistry(io.ml_config);

  const agent::Goal goal = ParseGoalSpec(args.goal_spec, g);
  std::cerr << "[gar_reason] goal: " << args.goal_spec << " (slot " << goal.slot
            << ")\n";

  // ---- dependency graph (once per rule set; d(.) is per goal) ----
  gar::core::analysis::PredicateTable table;
  gar::core::analysis::DependencyGraph dg;
  const auto dg_t0 = std::chrono::steady_clock::now();
  dg.Build(rules, &table);
  std::cerr << "[gar_reason] dependency graph: " << dg.NumPredicates()
            << " predicates, " << dg.NumArcs() << " arcs (built in "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             dg_t0)
                   .count()
            << "s)\n";

  // ---- components (composition root wiring) ----
  AgentModeArgs effective_args = args;
  if (!args.replay_actions_path.empty()) {
    effective_args.replay_actions = ReadReplayActions(args.replay_actions_path);
  }
  if (!args.predicate_prior_path.empty()) {
    std::ifstream prior_in(args.predicate_prior_path);
    if (!prior_in)
      throw std::runtime_error("cannot read predicate prior: " +
                               args.predicate_prior_path);
    std::string line;
    while (std::getline(prior_in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto tab = line.find('\t');
      if (tab == std::string::npos) continue;
      const std::uint32_t id = table.Find(line.substr(0, tab));
      if (id == gar::core::analysis::kInvalidPredicate) continue;
      effective_args.score.predicate_prior[id] = std::stod(line.substr(tab + 1));
    }
  }
  agent::JevPolicy* jev = nullptr;
  agent::LlmStarGuide* guide = nullptr;
  agent::PaperGuide* paper_guide = nullptr;
  agent::ReasoningAgent rag = MakeReasoningAgent(
      effective_args, &dg, &goal, &g, &rules, &rule_ids,
      args.policy == agent::PolicyKind::kJev ? &jev : nullptr,
      args.llm_guide ? &guide : nullptr,
      args.paper_guide ? &paper_guide : nullptr);
  agent::AgentState state(g, attrs.columns.empty() ? nullptr : &attrs, rules,
                          &ml_models);
  const agent::ExecConfig ec = MakeExecConfig(args);

  std::cerr << "[gar_reason] running: engine="
            << agent::MatchEngineName(args.engine) << " direction="
            << agent::SearchDirectionName(args.search_direction)
            << " policy=" << agent::PolicyName(args.policy)
            << " scorer=" << agent::ScorerName(args.scorer)
            << " budget=" << args.budget << " hdc_dim=" << args.hdc_dim
            << " seed=" << args.seed << " policy_seed=" << args.PolicySeed()
            << " hdc_seed=" << args.HdcSeed() << " llm_guide=" << args.llm_guide
            << "\n";
  const auto run_t0 = std::chrono::steady_clock::now();
  const agent::AgentResult res = rag.Run(&state, goal, ec);
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0)
          .count();

  double cum = 0.0;
  for (std::size_t i = 0; i < res.chain.size(); ++i) {
    const auto& s = res.chain[i];
    cum += s.cost;
    std::cerr << "[gar_reason] step " << i + 1 << ": " << rule_ids[s.motif.rule]
              << " ok=" << (s.success ? 1 : 0) << " cost=" << s.cost
              << " cum=" << cum
              << (s.derived ? " derived=" + FormatFact(g, *s.derived) : "")
              << "\n";
  }
  std::cerr << "[gar_reason] done: steps=" << res.chain.size()
            << " derived=" << res.derived.size() << " cost=" << res.logical_cost
            << " goal_derived=" << (res.goal_derived ? "true" : "false")
            << " termination=" << res.termination << " wall=" << wall << "s\n";

  // ---- optional soundness cross-check against the full closure (L4
  // composition: the agent library itself never sees
  // gar::core::engine::deduction) ----
  bool sound = true;
  if (args.verify_against_closure) {
    const auto v_t0 = std::chrono::steady_clock::now();
    gar::core::graph::Graph g2 =
        gar::core::graph::LoadGraph(io.csr_dir, /*with_inverses=*/true);
    gar::core::engine::chase::CpuChaseEngine closure_engine(
        g2, attrs.columns.empty() ? nullptr : &attrs);
    const gar::core::engine::chase::ChaseConfig closure_cfg;
    const auto closure = closure_engine.Run(rules, closure_cfg);
    for (const auto& e : res.derived) {
      if (g2.RelationPairSet(e.slot).count(
              gar::core::graph::PackPair(e.src, e.dst)) == 0) {
        sound = false;
        std::cerr << "[gar_reason] SOUNDNESS VIOLATION: " << FormatFact(g, e)
                  << " not in closure\n";
      }
    }
    std::cerr << "[gar_reason] verify-against-closure: soundness="
              << (sound ? "pass" : "fail") << " (closure derived "
              << closure.derived.size() << " facts in "
              << std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - v_t0)
                     .count()
              << "s)\n";
  }

  // ---- outputs ----
  std::filesystem::create_directories(io.out_root);
  {
    std::ofstream f(io.out_root + "/chain.tsv");
    f << "# step\trule_id\trule_name\tembedding\tsuccess\tcost\tcum_cost"
         "\tderived_fact\n";
    double acc = 0.0;
    for (std::size_t i = 0; i < res.chain.size(); ++i) {
      const auto& s = res.chain[i];
      acc += s.cost;
      const gar::GARule& r = rules[s.motif.rule];
      f << i + 1 << '\t' << rule_ids[s.motif.rule] << '\t' << r.name << '\t';
      for (std::size_t u = 0; u < s.motif.embedding.size(); ++u) {
        if (u) f << ",";
        f << g.EntityName(s.motif.embedding[u]);
      }
      f << '\t' << (s.success ? 1 : 0) << '\t' << s.cost << '\t' << acc << '\t'
        << (s.derived ? FormatFact(g, *s.derived) : "-") << "\n";
    }
  }
  WriteClosureEdges(io.out_root, g, rule_ids, res.derived);
  if (res.stats) {
    std::ofstream f(io.out_root + "/deduce_trace.tsv");
    f << "# seq\tevent\n";
    for (std::size_t i = 0; i < res.stats->trace.size(); ++i) {
      f << i + 1 << '\t' << res.stats->trace[i] << "\n";
    }
  }
  {
    std::ofstream f(io.out_root + "/summary.yaml");
    f << "mode: agent\n"
      << "goal: \"" << args.goal_spec << "\"\n"
      << "goal_slot: " << goal.slot << "\n"
      << "batch_size: " << args.batch_size << "\n"
      << "revival_threshold: " << args.revival_threshold << "\n"
      << "engine: " << agent::MatchEngineName(args.engine) << "\n"
      << "search_direction: "
      << agent::SearchDirectionName(args.search_direction) << "\n"
      << "policy: " << agent::PolicyName(args.policy) << "\n"
      << "scorer: " << agent::ScorerName(args.scorer) << "\n"
      << "star_order: " << agent::StarOrderName(args.star_order) << "\n"
      << "llm_guide: " << (args.llm_guide ? "true" : "false") << "\n"
      << "paper_guide: " << (args.paper_guide ? "true" : "false") << "\n"
      << "paper_guide_cache_dir: " << args.paper_guide_cache_dir << "\n"
      << "guide_lambda: " << args.guide_lambda << "\n"
      << "guide_strength: " << args.guide_strength << "\n"
      << "guide_max_options: " << args.guide_max_options << "\n"
      << "guide_min_cost: " << args.guide_min_cost << "\n"
      << "llm_backend: " << args.llm_backend << "\n"
      << "llm_base_url: " << args.llm_base_url << "\n"
      << "llm_model: " << args.llm_model << "\n"
      << "hdc_dim: " << args.hdc_dim << "\n"
      << "relevance_model: " << args.relevance_model << "\n"
      << "learned_handcrafted_weight: " << args.learned_handcrafted_weight << "\n"
      << "learned_relevance_weight: " << args.learned_relevance_weight << "\n"
      << "learned_prefilter: " << args.learned_prefilter << "\n"
      << "seed: " << args.seed << "\n"
      << "policy_seed: " << args.PolicySeed() << "\n"
      << "hdc_seed: " << args.HdcSeed() << "\n"
      << "steps: " << res.chain.size() << "\n"
      << "derived: " << res.derived.size() << "\n"
      << "logical_cost: " << res.logical_cost << "\n"
      << "execution_cost_ms: " << res.execution_cost_ms << "\n"
      << "goal_derived: " << (res.goal_derived ? "true" : "false") << "\n"
      << "budget_exhausted: " << (res.budget_exhausted ? "true" : "false")
      << "\n"
      << "termination: " << res.termination << "\n";
    if (res.stats) {
      // AdaDeduceExecutor instrumentation (flat fields; the event trace is
      // written to deduce_trace.tsv below).
      f << "select_events: " << res.stats->select_events << "\n"
        << "fair_select_events: " << res.stats->fair_select_events << "\n"
        << "evaluate_events: " << res.stats->evaluate_events << "\n"
        << "suspend_events: " << res.stats->suspend_events << "\n"
        << "revive_events: " << res.stats->revive_events << "\n"
        << "prune_events: " << res.stats->prune_events << "\n"
        << "derive_events: " << res.stats->derive_events << "\n"
        << "candidate_caps: " << res.stats->candidate_caps << "\n"
        << "ready_peak: " << res.stats->ready_peak << "\n"
        << "suspended_peak: " << res.stats->suspended_peak << "\n"
        << "generation_events: " << res.stats->generation_events << "\n"
        << "match_embeddings: " << res.stats->match_embeddings << "\n"
        << "match_head_known: " << res.stats->match_head_known << "\n"
        << "match_duplicate: " << res.stats->match_duplicate << "\n"
        << "match_new: " << res.stats->match_new << "\n"
        << "delta_invocations: " << res.stats->delta_invocations << "\n"
        << "delta_batch_edges: " << res.stats->delta_batch_edges << "\n"
        << "delta_match_requests: " << res.stats->delta_match_requests << "\n"
        << "ready_insertions: " << res.stats->ready_insertions << "\n"
        << "revival_passes: " << res.stats->revival_passes << "\n"
        << "score_calls: " << res.stats->score_calls << "\n"
        << "scored_candidates: " << res.stats->scored_candidates << "\n"
        << "search_cost: " << res.stats->search_cost << "\n"
        << "proof_cost: " << res.stats->proof_cost << "\n"
        << "prepare_wall_ms: " << res.stats->prepare_wall_ms << "\n"
        << "generation_wall_ms: " << res.stats->generation_wall_ms << "\n"
        << "selection_wall_ms: " << res.stats->selection_wall_ms << "\n"
        << "score_wall_ms: " << res.stats->score_wall_ms << "\n"
        << "ready_remove_wall_ms: " << res.stats->ready_remove_wall_ms << "\n"
        << "evaluation_wall_ms: " << res.stats->evaluation_wall_ms << "\n"
        << "derivation_wall_ms: " << res.stats->derivation_wall_ms << "\n"
        << "delta_wall_ms: " << res.stats->delta_wall_ms << "\n"
        << "delta_match_wall_ms: " << res.stats->delta_match_wall_ms << "\n"
        << "delta_complete_wall_ms: " << res.stats->delta_complete_wall_ms
        << "\n"
        << "partial_complete_wall_ms: " << res.stats->partial_complete_wall_ms
        << "\n"
        << "partial_grow_wall_ms: " << res.stats->partial_grow_wall_ms << "\n"
        << "revival_wall_ms: " << res.stats->revival_wall_ms << "\n";
    }
    if (!args.proof_star_model.empty() && res.stats) {
      f << "proof_star:\n"
        << "  weight: " << args.proof_star_weight << "\n"
        << "  min_cost: " << args.proof_star_min_cost << "\n"
        << "  estimated_cost: " << res.stats->proof_star_estimated_cost << "\n"
        << "  active: " << (res.stats->proof_star_active ? "true" : "false")
        << "\n";
    }
    if (jev != nullptr) {
      // §5.3: decision-model calls are control overhead — wall-clock, not
      // charged to the budget.
      f << "control_overhead:\n"
        << "  calls: " << jev->calls() << "\n"
        << "  wall_ms: " << jev->total_wall_ms() << "\n"
        << "  fallbacks: " << jev->fallbacks() << "\n";
    }
    if (guide != nullptr) {
      f << "guide_overhead:\n"
        << "  calls: " << guide->calls() << "\n"
        << "  wall_ms: " << guide->total_wall_ms() << "\n"
        << "  fallbacks: " << guide->fallbacks() << "\n"
        << "  reachable: " << guide->reachable() << "\n"
        << "  options: " << guide->options() << "\n"
        << "  active: " << (guide->active() ? "true" : "false") << "\n"
        << "  estimated_cost: " << guide->estimated_cost() << "\n";
    }
    if (paper_guide != nullptr) {
      f << "paper_guide_overhead:\n"
        << "  calls: " << paper_guide->calls() << "\n"
        << "  wall_ms: " << paper_guide->total_wall_ms() << "\n"
        << "  fallbacks: " << paper_guide->fallbacks() << "\n"
        << "  rejected: " << paper_guide->rejected() << "\n"
        << "  active: " << (paper_guide->active() ? "true" : "false") << "\n";
    }
    if (args.verify_against_closure) {
      f << "soundness: " << (sound ? "pass" : "fail") << "\n";
    }
  }
  if (paper_guide != nullptr && !paper_guide->raw_response().empty()) {
    std::ofstream raw(io.out_root + "/paper_guide.json");
    raw << paper_guide->raw_response();
  }

  // ---- stdout summary ----
  std::cout << "===== gar_reason agent: goal " << args.goal_spec << " =====\n"
            << "goal_derived: " << (res.goal_derived ? "true" : "false")
            << "  steps: " << res.chain.size()
            << "  derived: " << res.derived.size()
            << "  logical_cost: " << res.logical_cost
            << "  termination: " << res.termination << "\n";
  for (std::size_t i = 0;
       i < res.chain.size() && i < static_cast<std::size_t>(io.print_n); ++i) {
    const auto& s = res.chain[i];
    std::cout << "  " << i + 1 << ". " << rule_ids[s.motif.rule]
              << (s.success ? " OK " : " FAIL ")
              << (s.derived ? FormatFact(g, *s.derived) : "-") << "\n"
              << "     " << rules[s.motif.rule].name << "\n";
  }
  // A completed search is a successful run; "goal not derived within the
  // budget" is reported in summary.yaml, not via the exit code.
  return 0;
}

}  // namespace gar_apps
