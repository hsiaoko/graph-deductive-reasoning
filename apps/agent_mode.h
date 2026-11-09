#pragma once

// gar_reason --mode agent: policy-guided goal deduction (paper §3-§5).
// Argument parsing, execution and output
// live here; the engine itself is engine/agent/. The
// --verify-against-closure cross-check composes engine::deduction at the
// app layer — the agent library never sees its sibling engine.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "full_deduce_cli.h"  // FullDeduceArgs + loaders + closure_output.h
#include "gar/analysis/dependency_graph.h"
#include "gar/engine/agent/executor.h"
#include "gar/engine/agent/reasoning_agent.h"
#include "gar/engine/deduction/cpu_chase_engine.h"
#include "gar/engine/agent/value_model.h"
#include "gar/hdc/const.h"

namespace gar_apps {

struct AgentModeArgs {
  std::string goal_spec;  // "rel_name(src,dst)"; '_' = open argument
  std::uint64_t budget = 100;
  std::string policy = "deterministic";   // deterministic|random|hybrid|jev|q-hdc|q-numeric
  std::string scorer = "exact";           // exact|hdc|null
  std::string executor = "greedy";        // greedy|bnb|backward|fixpoint
  // FixpointExecutor: longest-waiting fair scheduling interval (0=off).
  std::uint32_t fair_interval = 0;
  std::string fixpoint_star_order = "input";  // input|goal
  // Reasoning direction switch (architecture decoupling; unset = legacy
  // --executor behaviour, bit-identical). forward: greedy|bnb only.
  // backward: backward executor = full discipline; backward+greedy =
  // degenerate single-path arm (first OR choice only, no backtracking);
  // backward+bnb = reserved for Phase 2 (error).
  std::string direction;                  // ""|forward|backward
  std::uint32_t top_k = 0;                // bnb: 0 = exact (no truncation)
  std::uint32_t hdc_dim = gar::hdc::kHdcDefaultDim;
  std::uint64_t seed = 42;
  // Seed decoupling (--policy-seed / --hdc-seed; both fall back to --seed,
  // so omitting them reproduces legacy runs bit-for-bit). The HDC encoder
  // uses the hdc seed; every policy-side RNG (RandomizedPolicy, backward
  // --backward-order random/hybrid shuffles, JEV mock) uses the policy seed.
  std::optional<std::uint64_t> policy_seed;
  std::optional<std::uint64_t> hdc_seed;
  [[nodiscard]] std::uint64_t PolicySeed() const {
    return policy_seed.value_or(seed);
  }
  [[nodiscard]] std::uint64_t HdcSeed() const { return hdc_seed.value_or(seed); }
  // Backward subgoal recursion cap (--max-subgoal-depth; default 4 keeps
  // legacy behaviour). Both backward executors read ExecConfig's copy.
  std::uint32_t max_subgoal_depth = 4;
  std::uint32_t max_candidates_per_rule = 256;
  // BackwardExecutor open-target collect cap (default 8 = legacy).
  std::uint32_t open_collect_cap = 8;
  gar::engine::agent::HandcraftedConfig score;
  bool verify_against_closure = false;
  // Ablation: --no-dg removes dependency-graph guidance (d(.) table all
  // zeros: no candidate hard-filter, rel_fam neutral, price = w0). The DG
  // itself is still built (failure classification reads its in-degrees).
  bool no_dg = false;
  // Jev (LLM decision model) policy knobs; --jev-mock swaps the HTTP client
  // for the network-free MockJevClient (development/regression support).
  gar::engine::agent::JevConfig jev;
  bool jev_mock = false;
  // bnb branch value model: --bnb-priority utility|hdc-value plus
  // --value-model PATH (required when hdc-value). Ordering only; default
  // utility reproduces the stock best-first behaviour bit-for-bit.
  std::string bnb_priority = "utility";
  // backward attempt ordering: heuristic (default, bit-identical legacy)
  // or hdc-value (needs --value-model PATH).
  std::string backward_priority = "heuristic";
  // BackwardBnB lower bound flavour (only meaningful with
  // --direction backward --executor bnb).
  std::string backward_bnb_lb = "predicate";  // predicate|instance
  std::string backward_bnb_enum = "eager";    // eager|lazy
  // BackwardExecutor candidate ordering (policy analogue for backward):
  // score (default, bit-identical) | random (seeded shuffle) | hybrid
  // (seeded shuffle inside the top-8 score window).
  std::string backward_order = "score";
  // Full-extension mode for open-sided backward goals (loop to fixpoint).
  bool backward_full_enum = false;
  // Greedy analogue for open-sided goals; --wall-limit gives a clean
  // in-process stop (partial results still written) at N seconds.
  bool forward_full_enum = false;
  double wall_limit_sec = 0.0;
  std::string value_model;
  gar::engine::agent::HdcBackend hdc_value_backend =
      gar::engine::agent::HdcBackend::kBitLut;
  std::string fixpoint_q_trace_path;
};

// Parses one agent-mode option; returns false when `k` is not one.
template <typename NextFn>
bool ParseAgentModeOption(const std::string& k, NextFn&& next,
                          AgentModeArgs* a) {
  if (k == "--goal") a->goal_spec = next();
  else if (k == "--budget") a->budget = std::stoull(next());
  else if (k == "--policy") a->policy = next();
  else if (k == "--scorer") a->scorer = next();
  else if (k == "--executor") a->executor = next();
  else if (k == "--direction") a->direction = next();
  else if (k == "--top-k")
    a->top_k = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--hdc-dim") a->hdc_dim = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--seed") a->seed = std::stoull(next());
  else if (k == "--policy-seed") a->policy_seed = std::stoull(next());
  else if (k == "--hdc-seed") a->hdc_seed = std::stoull(next());
  else if (k == "--max-subgoal-depth")
    a->max_subgoal_depth = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--open-collect-cap")
    a->open_collect_cap = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--max-candidates-per-rule")
    a->max_candidates_per_rule = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--alpha") a->score.alpha = std::stod(next());
  else if (k == "--beta") a->score.beta = std::stod(next());
  else if (k == "--gamma") a->score.gamma = std::stod(next());
  else if (k == "--nu") a->score.nu = std::stod(next());
  else if (k == "--nu2") a->score.nu2 = std::stod(next());
  else if (k == "--tau-hdc") a->score.tau_hdc = std::stod(next());
  else if (k == "--no-confidence") a->score.use_confidence = false;
  else if (k == "--no-dg") a->no_dg = true;
  else if (k == "--verify-against-closure") a->verify_against_closure = true;
  else if (k == "--jev-endpoint") a->jev.endpoint = next();
  else if (k == "--jev-timeout-ms")
    a->jev.timeout_ms = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--jev-top-k")
    a->jev.top_k = static_cast<std::size_t>(std::stoull(next()));
  else if (k == "--bnb-priority") a->bnb_priority = next();
  else if (k == "--backward-priority") a->backward_priority = next();
  else if (k == "--backward-bnb-lb") a->backward_bnb_lb = next();
  else if (k == "--backward-bnb-enum") a->backward_bnb_enum = next();
  else if (k == "--backward-full-enum") a->backward_full_enum = true;
  else if (k == "--forward-full-enum") a->forward_full_enum = true;
  else if (k == "--wall-limit") a->wall_limit_sec = std::stod(next());
  else if (k == "--fair-interval")
    a->fair_interval = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--fixpoint-star-order") {
    a->fixpoint_star_order = next();
    if (a->fixpoint_star_order != "input" &&
        a->fixpoint_star_order != "goal") {
      throw std::invalid_argument("unknown --fixpoint-star-order: " +
                                  a->fixpoint_star_order + " (input|goal)");
    }
  }
  else if (k == "--backward-order") {
    a->backward_order = next();
    if (a->backward_order != "score" && a->backward_order != "random" &&
        a->backward_order != "hybrid") {
      throw std::invalid_argument("unknown --backward-order: " +
                                  a->backward_order +
                                  " (score|random|hybrid)");
    }
  }
  else if (k == "--value-model") a->value_model = next();
  else if (k == "--hdc-value-backend") {
    const std::string backend = next();
    if (backend == "original") {
      a->hdc_value_backend = gar::engine::agent::HdcBackend::kOriginal;
    } else if (backend == "simd") {
      a->hdc_value_backend = gar::engine::agent::HdcBackend::kSimd;
    } else if (backend == "bit-lut") {
      a->hdc_value_backend = gar::engine::agent::HdcBackend::kBitLut;
    } else {
      throw std::invalid_argument(
          "unknown --hdc-value-backend: " + backend +
          " (original|simd|bit-lut)");
    }
  }
  else if (k == "--fixpoint-q-trace") a->fixpoint_q_trace_path = next();
  else if (k == "--jev-sample") a->jev.sample = true;
  else if (k == "--jev-mock") a->jev_mock = true;
  else return false;
  return true;
}

// Parses "rel_name(src,dst)": the relation name resolves to a base slot
// through the graph's relation dictionary; arguments are entity names
// resolved through the entity dictionary, or '_' for an open variable.
inline gar::engine::agent::Goal ParseGoalSpec(const std::string& spec,
                                              const gar::graph::TripleGraph& g) {
  const auto lp = spec.find('(');
  const auto rp = spec.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
    throw std::invalid_argument("bad --goal (want rel_name(src,dst)): " +
                                spec);
  }
  const std::string rel = spec.substr(0, lp);
  const std::string args = spec.substr(lp + 1, rp - lp - 1);
  const auto comma = args.find(',');
  if (comma == std::string::npos) {
    throw std::invalid_argument("bad --goal (want two arguments): " + spec);
  }
  const std::string a0 = args.substr(0, comma);
  const std::string a1 = args.substr(comma + 1);

  gar::engine::agent::Goal goal;
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
  auto resolve = [&](const std::string& name)
      -> std::optional<std::uint32_t> {
    if (name == "_") return std::nullopt;
    for (std::uint32_t v = 0; v < g.num_entities(); ++v) {
      if (g.EntityName(v) == name) return v;
    }
    throw std::invalid_argument("unknown entity in --goal: " + name);
  };
  goal.args = {resolve(a0), resolve(a1)};
  return goal;
}

// Builds the agent composition root from CLI-flavored args. Shared by
// gar_reason --mode agent and gar_analyze verify (identical defaults and
// wiring in both). When args.policy == "jev", a JevPolicy is built from
// `jev_ctx` (goal/graph/rules/rule_ids — the policy needs them for the
// state text) and *jev_out is set for control-overhead reporting.
inline gar::engine::agent::ReasoningAgent MakeReasoningAgent(
    const AgentModeArgs& args, const gar::analysis::DependencyGraph* dg,
    const gar::engine::agent::Goal* goal = nullptr,
    const gar::graph::TripleGraph* graph = nullptr,
    const std::vector<gar::GARule>* rules = nullptr,
    const std::vector<std::string>* rule_ids = nullptr,
    gar::engine::agent::JevPolicy** jev_out = nullptr) {
  namespace agent = gar::engine::agent;
  const gar::hdc::HdcEncoder encoder({args.hdc_dim, args.HdcSeed()});
  agent::ReasoningAgent::Components comps;
  comps.cost_model = std::make_unique<agent::UnitCostModel>();
  if (args.scorer == "exact") {
    comps.scorer = std::make_unique<agent::ExactScorer>(args.score,
                                                        comps.cost_model.get());
  } else if (args.scorer == "null") {
    comps.scorer = std::make_unique<agent::NullScorer>();
  } else if (args.scorer == "hdc") {
    comps.scorer = std::make_unique<agent::HdcScorer>(args.score,
                                                      comps.cost_model.get(),
                                                      encoder);
  } else {
    throw std::invalid_argument("unknown --scorer: " + args.scorer +
                                " (exact|hdc|null)");
  }
  if (args.policy == "jev") {
    if (goal == nullptr || graph == nullptr || rules == nullptr) {
      throw std::invalid_argument("--policy jev needs goal/graph/rules "
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
  } else if (args.policy == "random") {
    comps.policy = std::make_unique<agent::RandomizedPolicy>(args.PolicySeed());
  } else if (args.policy == "hybrid") {
    comps.policy = std::make_unique<agent::HybridNeuroSymbolicPolicy>();
  } else if (args.policy == "q-hdc" || args.policy == "q-numeric") {
    if (args.value_model.empty()) {
      throw std::invalid_argument("--policy " + args.policy +
                                  " requires --value-model PATH");
    }
    if (args.scorer != "exact") {
      throw std::invalid_argument("Q policies require --scorer exact");
    }
    auto vm = std::make_shared<agent::ValueModel>();
    vm->SetHdcBackend(args.hdc_value_backend);
    if (!vm->Load(args.value_model)) {
      throw std::runtime_error("failed to load value model: " +
                               args.value_model);
    }
    const std::string want = args.policy == "q-hdc" ? "hdc" : "numeric";
    if (vm->kind() != want) {
      throw std::runtime_error("--policy " + args.policy + " got a " +
                               vm->kind() + " model: " + args.value_model);
    }
    comps.policy = std::make_unique<agent::LearnedQPolicy>(vm.get());
    comps.value_model = std::move(vm);
  } else {
    comps.policy = std::make_unique<agent::DeterministicPolicy>();
  }
  // --direction resolution (unset = legacy --executor behaviour).
  std::string executor = args.executor;
  bool backward_no_backtrack = false;
  if (!args.direction.empty()) {
    if (args.direction == "forward") {
      if (args.executor == "backward") {
        throw std::invalid_argument(
            "--direction forward and --executor backward are mutually "
            "exclusive");
      }
    } else if (args.direction == "backward") {
      if (args.executor == "bnb") {
        executor = "backward-bnb";  // Phase 2: AND-OR branch-and-bound
        if (args.backward_bnb_lb != "predicate" &&
            args.backward_bnb_lb != "instance") {
          throw std::invalid_argument("unknown --backward-bnb-lb: " +
                                      args.backward_bnb_lb);
        }
        if (args.backward_bnb_enum != "eager" &&
            args.backward_bnb_enum != "lazy") {
          throw std::invalid_argument("unknown --backward-bnb-enum: " +
                                      args.backward_bnb_enum);
        }
      } else if (args.executor == "backward") {
        executor = "backward";  // full discipline (DFS + backtracking)
      } else {
        executor = "backward";         // degenerate single-path arm rides
        backward_no_backtrack = true;  // the backward executor, OR = first
                                       // choice only, no pass-2 fallback
      }
    } else {
      throw std::invalid_argument("unknown --direction: " + args.direction);
    }
  }
  const bool q_policy =
      args.policy == "q-hdc" || args.policy == "q-numeric";
  if ((q_policy || !args.fixpoint_q_trace_path.empty()) &&
      executor != "fixpoint") {
    throw std::invalid_argument(
        "Q policies and --fixpoint-q-trace require --executor fixpoint");
  }
  if (!args.fixpoint_q_trace_path.empty() && args.scorer != "exact") {
    throw std::invalid_argument("--fixpoint-q-trace requires --scorer exact");
  }
  if (executor == "bnb") {
    comps.executor = std::make_unique<agent::BranchBoundExecutor>();
    if (args.bnb_priority == "hdc-value" ||
        args.bnb_priority == "numeric-value") {
      if (args.value_model.empty()) {
        throw std::invalid_argument("--bnb-priority " + args.bnb_priority +
                                    " requires --value-model PATH");
      }
      auto vm = std::make_shared<agent::ValueModel>();
      vm->SetHdcBackend(args.hdc_value_backend);
      if (!vm->Load(args.value_model)) {
        throw std::runtime_error("failed to load value model: " +
                                 args.value_model);
      }
      // Arm/format consistency: hdc-value = hdc_value_v1, numeric-value =
      // numeric_ridge_v1 (a mismatch is almost certainly a wrong path).
      const std::string want =
          args.bnb_priority == "hdc-value" ? "hdc" : "numeric";
      if (vm->kind() != want) {
        throw std::runtime_error("--bnb-priority " + args.bnb_priority +
                                 " got a " + vm->kind() + " model: " +
                                 args.value_model);
      }
      comps.value_model = std::move(vm);
    } else if (args.bnb_priority != "utility") {
      throw std::invalid_argument("unknown --bnb-priority: " +
                                  args.bnb_priority);
    }
  } else if (executor == "backward-bnb") {
    comps.executor = std::make_unique<agent::BackwardBnBExecutor>();
  } else if (executor == "backward") {
    comps.executor = std::make_unique<agent::BackwardExecutor>();
    comps.backward_no_backtrack = backward_no_backtrack;
    if (args.backward_priority == "hdc-value") {
      if (args.value_model.empty()) {
        throw std::invalid_argument(
            "--backward-priority hdc-value requires --value-model PATH");
      }
      auto vm = std::make_shared<agent::ValueModel>();
      vm->SetHdcBackend(args.hdc_value_backend);
      if (!vm->Load(args.value_model)) {
        throw std::runtime_error("failed to load value model: " +
                                 args.value_model);
      }
      comps.value_model = std::move(vm);
    } else if (args.backward_priority != "heuristic") {
      throw std::invalid_argument("unknown --backward-priority: " +
                                  args.backward_priority);
    }
  } else if (executor == "fixpoint") {
    comps.executor = std::make_unique<agent::FixpointExecutor>();
  } else {
    comps.executor = std::make_unique<agent::GreedyExecutor>();
  }
  return agent::ReasoningAgent(std::move(comps), dg, encoder,
                               !args.no_dg);
}

// Renders a derived fact as "rel(src,dst)" with entity names.
inline std::string FormatFact(const gar::graph::TripleGraph& g,
                              const gar::graph::DerivedEdge& e) {
  return g.SlotName(e.slot) + "(" + g.EntityName(e.src) + "," +
         g.EntityName(e.dst) + ")";
}

inline int RunAgentMode(const FullDeduceArgs& io, const AgentModeArgs& args) {
  namespace agent = gar::engine::agent;

  // ---- load (same sources as the full mode) ----
  gar::graph::TripleGraph g =
      gar::bridge::LoadTripleGraph(io.csr_dir, /*with_inverses=*/true);
  std::cerr << "[gar_reason] |V|=" << g.num_entities()
            << " |R|=" << g.num_relations() << " |E|=" << g.num_edges()
            << "\n";
  std::vector<std::filesystem::path> rule_dirs;
  for (const auto& e : std::filesystem::directory_iterator(io.rules_root)) {
    if (e.is_directory() &&
        std::filesystem::exists(e.path() / "rule.yaml")) {
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
  gar::graph::VertexAttrs attrs = gar::bridge::LoadVertexAttrs(io.csr_dir);
  std::cerr << "[gar_reason] " << attrs.columns.size()
            << " numeric attribute columns\n";

  const agent::Goal goal = ParseGoalSpec(args.goal_spec, g);
  std::cerr << "[gar_reason] goal: " << args.goal_spec << " (slot "
            << goal.slot << ")\n";

  // ---- dependency graph (once per rule set; d(.) is per goal) ----
  gar::analysis::PredicateTable table;
  gar::analysis::DependencyGraph dg;
  const auto dg_t0 = std::chrono::steady_clock::now();
  dg.Build(rules, &table);
  std::cerr << "[gar_reason] dependency graph: " << dg.NumPredicates()
            << " predicates, " << dg.NumArcs() << " arcs (built in "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             dg_t0)
                   .count()
            << "s)\n";

  // ---- components (composition root wiring) ----
  agent::JevPolicy* jev = nullptr;
  agent::ReasoningAgent rag = MakeReasoningAgent(
      args, &dg, &goal, &g, &rules, &rule_ids,
      args.policy == "jev" ? &jev : nullptr);
  agent::AgentState state(g, attrs.columns.empty() ? nullptr : &attrs, rules);
  agent::ExecConfig ec;
  ec.budget = args.budget;
  ec.match_cap_per_rule = args.max_candidates_per_rule;
  ec.open_collect_cap = args.open_collect_cap;
  ec.top_k = args.top_k;
  ec.utility_gamma = args.score.gamma;
  ec.backward_bnb_instance_lb = (args.backward_bnb_lb == "instance");
  ec.backward_bnb_lazy_enum = (args.backward_bnb_enum == "lazy");
  ec.backward_order = args.backward_order;
  ec.backward_full_enum = args.backward_full_enum;
  ec.forward_full_enum = args.forward_full_enum;
  ec.wall_limit_sec = args.wall_limit_sec;
  ec.fair_interval = args.fair_interval;
  ec.fixpoint_star_order = args.fixpoint_star_order;
  ec.fixpoint_q_trace_path = args.fixpoint_q_trace_path;
  ec.seed = args.PolicySeed();
  ec.max_subgoal_depth = args.max_subgoal_depth;

  std::cerr << "[gar_reason] running: policy=" << args.policy
            << " scorer=" << args.scorer << " executor=" << args.executor
            << " budget=" << args.budget << " hdc_dim=" << args.hdc_dim
            << " seed=" << args.seed << " policy_seed=" << args.PolicySeed()
            << " hdc_seed=" << args.HdcSeed();
  if (args.executor == "bnb") std::cerr << " top_k=" << args.top_k;
  std::cerr << "\n";
  const auto run_t0 = std::chrono::steady_clock::now();
  const agent::AgentResult res = rag.Run(&state, goal, ec);
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - run_t0)
          .count();

  double cum = 0.0;
  for (std::size_t i = 0; i < res.chain.size(); ++i) {
    const auto& s = res.chain[i];
    cum += s.cost;
    std::cerr << "[gar_reason] step " << i + 1 << ": "
              << rule_ids[s.motif.rule] << " ok=" << (s.success ? 1 : 0)
              << " cost=" << s.cost << " cum=" << cum
              << (s.derived ? " derived=" + FormatFact(g, *s.derived) : "")
              << "\n";
  }
  std::cerr << "[gar_reason] done: steps=" << res.chain.size()
            << " derived=" << res.derived.size() << " cost=" << res.total_cost
            << " goal_derived=" << (res.goal_derived ? "true" : "false")
            << " termination=" << res.termination << " wall=" << wall << "s\n";
  if (res.fwd_stats) {
    std::cerr << "[gar_reason] forward-incremental: rule_selections="
              << res.fwd_stats->rule_selections
              << " rules_initialized=" << res.fwd_stats->rules_initialized
              << " delta_enums=" << res.fwd_stats->delta_enums
              << " embeddings=" << res.fwd_stats->embeddings_generated
              << " reused=" << res.fwd_stats->candidates_reused
              << " capped=" << res.fwd_stats->capped_enums
              << " match_ms=" << res.fwd_stats->match_wall_ms
              << " apply_ms=" << res.fwd_stats->apply_wall_ms
              << " score_ms=" << res.fwd_stats->score_wall_ms << "\n";
  }

  // ---- optional soundness cross-check against the full closure (L4
  // composition: the agent library itself never sees engine::deduction) ----
  bool sound = true;
  if (args.verify_against_closure) {
    const auto v_t0 = std::chrono::steady_clock::now();
    gar::graph::TripleGraph g2 =
        gar::bridge::LoadTripleGraph(io.csr_dir, /*with_inverses=*/true);
    gar::engine::deduction::CpuChaseEngine closure_engine(
        g2, attrs.columns.empty() ? nullptr : &attrs);
    const gar::engine::deduction::ChaseConfig closure_cfg;
    const auto closure = closure_engine.Run(rules, closure_cfg);
    for (const auto& e : res.derived) {
      if (g2.RelationPairSet(e.slot)
              .count(gar::graph::PackPair(e.src, e.dst)) == 0) {
        sound = false;
        std::cerr << "[gar_reason] SOUNDNESS VIOLATION: "
                  << FormatFact(g, e) << " not in closure\n";
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
      f << '\t' << (s.success ? 1 : 0) << '\t' << s.cost << '\t' << acc
        << '\t'
        << (s.derived ? FormatFact(g, *s.derived) : "-") << "\n";
    }
  }
  WriteClosureEdges(io.out_root, g, rule_ids, res.derived);
  if (res.fx_stats) {
    std::ofstream f(io.out_root + "/fixpoint_trace.tsv");
    f << "# seq\tevent\n";
    for (std::size_t i = 0; i < res.fx_stats->trace.size(); ++i) {
      f << i + 1 << '\t' << res.fx_stats->trace[i] << "\n";
    }
  }
  {
    std::ofstream f(io.out_root + "/summary.yaml");
    f << "mode: agent\n"
      << "goal: \"" << args.goal_spec << "\"\n"
      << "goal_slot: " << goal.slot << "\n"
      << "policy: " << args.policy << "\n"
      << "scorer: " << args.scorer << "\n"
      << "executor: " << args.executor << "\n"
      << "fixpoint_star_order: " << args.fixpoint_star_order << "\n"
      << "hdc_dim: " << args.hdc_dim << "\n"
      << "hdc_value_backend: "
      << gar::engine::agent::HdcBackendName(args.hdc_value_backend) << "\n"
      << "seed: " << args.seed << "\n"
      << "policy_seed: " << args.PolicySeed() << "\n"
      << "hdc_seed: " << args.HdcSeed() << "\n"
      << "max_subgoal_depth: " << args.max_subgoal_depth << "\n"
      << "steps: " << res.chain.size() << "\n"
      << "derived: " << res.derived.size() << "\n"
      << "total_cost: " << res.total_cost << "\n"
      << "goal_derived: " << (res.goal_derived ? "true" : "false") << "\n"
      << "budget_exhausted: " << (res.budget_exhausted ? "true" : "false")
      << "\n"
      << "termination: " << res.termination << "\n"
      << "utility: " << res.utility << "\n"
      << "expansions: " << res.expansions << "\n"
      << "first_goal_expansions: " << res.first_goal_expansions << "\n"
      << "first_goal_wall_ms: " << res.first_goal_wall_ms << "\n"
      << "certified_optimal: " << (res.certified_optimal ? "true" : "false")
      << "\n"
      << "certified_no_chain: "
      << (res.certified_no_chain ? "true" : "false") << "\n"
      << "anytime_fallback: " << (res.anytime_fallback ? "true" : "false")
      << "\n";
    if (res.fwd_stats) {
      // Incremental-greedy instrumentation (flat fields).
      f << "fwd_rule_selections: " << res.fwd_stats->rule_selections << "\n"
        << "fwd_rules_initialized: " << res.fwd_stats->rules_initialized
        << "\n"
        << "fwd_delta_enums: " << res.fwd_stats->delta_enums << "\n"
        << "fwd_embeddings_generated: " << res.fwd_stats->embeddings_generated
        << "\n"
        << "fwd_candidates_reused: " << res.fwd_stats->candidates_reused
        << "\n"
        << "fwd_capped_enums: " << res.fwd_stats->capped_enums << "\n"
        << "fwd_match_wall_ms: " << res.fwd_stats->match_wall_ms << "\n"
        << "fwd_apply_wall_ms: " << res.fwd_stats->apply_wall_ms << "\n"
        << "fwd_score_wall_ms: " << res.fwd_stats->score_wall_ms << "\n";
    }
    if (res.fx_stats) {
      // FixpointExecutor instrumentation (flat fields; the event trace is
      // written to fixpoint_trace.tsv below).
      f << "fx_select_events: " << res.fx_stats->select_events << "\n"
        << "fx_fair_select_events: " << res.fx_stats->fair_select_events
        << "\n"
        << "fx_evaluate_events: " << res.fx_stats->evaluate_events << "\n"
        << "fx_suspend_events: " << res.fx_stats->suspend_events << "\n"
        << "fx_revive_events: " << res.fx_stats->revive_events << "\n"
        << "fx_prune_events: " << res.fx_stats->prune_events << "\n"
        << "fx_derive_events: " << res.fx_stats->derive_events << "\n"
        << "fx_candidate_caps: " << res.fx_stats->candidate_caps << "\n"
        << "fx_ready_peak: " << res.fx_stats->ready_peak << "\n"
        << "fx_suspended_peak: " << res.fx_stats->suspended_peak << "\n"
        << "fx_generation_events: " << res.fx_stats->generation_events << "\n"
        << "fx_match_embeddings: " << res.fx_stats->match_embeddings << "\n"
        << "fx_ready_insertions: " << res.fx_stats->ready_insertions << "\n"
        << "fx_revival_passes: " << res.fx_stats->revival_passes << "\n"
        << "fx_score_calls: " << res.fx_stats->score_calls << "\n"
        << "fx_scored_candidates: " << res.fx_stats->scored_candidates << "\n"
        << "fx_search_cost: " << res.fx_stats->search_cost << "\n"
        << "fx_proof_cost: " << res.fx_stats->proof_cost << "\n"
        << "fx_prepare_wall_ms: " << res.fx_stats->prepare_wall_ms << "\n"
        << "fx_generation_wall_ms: " << res.fx_stats->generation_wall_ms << "\n"
        << "fx_selection_wall_ms: " << res.fx_stats->selection_wall_ms << "\n"
        << "fx_score_wall_ms: " << res.fx_stats->score_wall_ms << "\n"
        << "fx_ready_remove_wall_ms: " << res.fx_stats->ready_remove_wall_ms << "\n"
        << "fx_evaluation_wall_ms: " << res.fx_stats->evaluation_wall_ms << "\n"
        << "fx_derivation_wall_ms: " << res.fx_stats->derivation_wall_ms << "\n"
        << "fx_delta_wall_ms: " << res.fx_stats->delta_wall_ms << "\n"
        << "fx_delta_complete_wall_ms: "
        << res.fx_stats->delta_complete_wall_ms << "\n"
        << "fx_partial_complete_wall_ms: "
        << res.fx_stats->partial_complete_wall_ms << "\n"
        << "fx_partial_grow_wall_ms: " << res.fx_stats->partial_grow_wall_ms
        << "\n"
        << "fx_revival_wall_ms: " << res.fx_stats->revival_wall_ms << "\n";
    }
    if (jev != nullptr) {
      // §5.3: decision-model calls are control overhead — wall-clock, not
      // charged to the budget.
      f << "control_overhead:\n"
        << "  calls: " << jev->calls() << "\n"
        << "  wall_ms: " << jev->total_wall_ms() << "\n"
        << "  fallbacks: " << jev->fallbacks() << "\n";
    }
    if (args.verify_against_closure) {
      f << "soundness: " << (sound ? "pass" : "fail") << "\n";
    }
  }

  // ---- stdout summary ----
  std::cout << "===== gar_reason agent: goal " << args.goal_spec << " =====\n"
            << "goal_derived: " << (res.goal_derived ? "true" : "false")
            << "  steps: " << res.chain.size()
            << "  derived: " << res.derived.size()
            << "  total_cost: " << res.total_cost
            << "  termination: " << res.termination << "\n";
  for (std::size_t i = 0;
       i < res.chain.size() && i < static_cast<std::size_t>(io.print_n);
       ++i) {
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
