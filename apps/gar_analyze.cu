// gar_analyze: the analysis toolchain — one binary, five subcommands.
// All logic lives in the libraries (analysis / engine::agent /
// engine::deduction / bridge); this app only composes and does IO.
//
//   gar_analyze rules-stats  --rules <root> [--dot <out.dot>] [--yaml <out.yaml>]
//   gar_analyze gen-goals    --csr <dir> --rules <root> --per-slot N --seed S
//                            --out goals.tsv [--threads N]
//   gar_analyze gen-workload --csr <dir> --rules <root> --num-tasks N --seed S
//                            --out tasks.tsv [--budgets 0.5,1,2,4]
//   gar_analyze chain-cmp    --agent-out <dir> --closure-out <dir>
//                            [--goal 'rel(src,dst)']
//   gar_analyze verify       --run --csr <dir> --rules <root> --goal 'rel(src,dst)'
//                            [--budget B] [--policy ..] [--scorer ..]
//                            [--executor greedy|bnb] [--top-k N] [--gpu]
//                            [--seed S] [--hdc-dim N] [--max-candidates-per-rule N]
//                            [--alpha..--nu2 F] [--tau-hdc F] [--no-confidence]
//                            [--report verify_report.yaml]
//   gar_analyze verify       --from-dirs <agent_dir> <cpu_dir> [<gpu_dir>]
//                            --goal 'rel(src,dst)' [--report F]
//   gar_analyze verify       --tasks tasks.tsv --csr <dir> --rules <root>
//                            [--policy ..] [--scorer ..] [--executor ..] [--gpu]
//                            [--report F]
//
// gen-goals follows the paper's workload construction (§7): hold out the
// sampled triples from the graph (Γ0 = the rest), run the unrestricted
// chase once, and keep only goals the closure re-derives.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent_mode.h"
#include "closure_output.h"
#include "full_deduce_cli.h"
#include "gar/analysis/dependency_graph.h"
#include "gar/analysis/goal_sampler.h"
#include "gar/analysis/rule_stats.h"
#include "gar/engine/deduction/cpu_chase_engine.h"
#include "gar/engine/deduction/gpu_deduce_engine.h"

namespace {

namespace agent = gar::engine::agent;
namespace analysis = gar::analysis;
namespace deduction = gar::engine::deduction;

[[noreturn]] void Usage() {
  std::cerr
      << "usage:\n"
         "  gar_analyze rules-stats --rules <root> [--dot F] [--yaml F]\n"
         "  gar_analyze gen-goals --csr <dir> --rules <root> --per-slot N "
         "--seed S --out goals.tsv [--threads N] [--max-rounds N] [--max-derivations N] [--max-matches N]\n"
         "  gar_analyze gen-workload --csr <dir> --rules <root> "
         "--num-tasks N --seed S --out tasks.tsv [--budgets 0.5,1,2,4]\n"
         "  gar_analyze chain-cmp --agent-out <dir> --closure-out <dir> "
         "[--goal 'rel(src,dst)']\n"
         "  gar_analyze verify --run --csr <dir> --rules <root> --goal "
         "'rel(src,dst)' [--budget B] [--policy ..] [--scorer ..] "
         "[--executor greedy|bnb] [--top-k N] [--gpu] [--report F]\n"
         "  gar_analyze verify --from-dirs <agent> <cpu> [<gpu>] --goal "
         "'rel(src,dst)' [--report F]\n"
         "  gar_analyze verify --tasks tasks.tsv --csr <dir> --rules <root> "
         "[--policy ..] [--scorer ..] [--executor ..] [--gpu] [--report F]\n";
  std::exit(2);
}

// ---------- small IO helpers (app layer) ----------

// closure_edges.tsv row: src, relation, dst (names; round/rule dropped).
struct EdgeRow {
  std::string src, rel, dst;
};

std::vector<EdgeRow> ReadClosureEdges(const std::string& dir) {
  std::ifstream f(dir + "/closure_edges.tsv");
  if (!f) {
    throw std::runtime_error("cannot open " + dir + "/closure_edges.tsv");
  }
  std::vector<EdgeRow> out;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto t1 = line.find('\t');
    const auto t2 = line.find('\t', t1 + 1);
    const auto t3 = line.find('\t', t2 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos ||
        t3 == std::string::npos) {
      continue;
    }
    out.push_back(
        {line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1),
         line.substr(t2 + 1, t3 - t2 - 1)});
  }
  return out;
}

std::optional<std::string> ReadYamlScalar(const std::string& path,
                                          const std::string& key) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::string line;
  const std::string prefix = key + ":";
  while (std::getline(f, line)) {
    if (line.rfind(prefix, 0) == 0) {
      std::string v = line.substr(prefix.size());
      const auto b = v.find_first_not_of(' ');
      const auto e = v.find_last_not_of(' ');
      if (b == std::string::npos) return std::nullopt;
      return v.substr(b, e - b + 1);
    }
  }
  return std::nullopt;
}

// Textual goal split (no graph needed): "rel(src,dst)" -> (rel, src, dst);
// '_' is an open argument (wildcard in matching).
struct TextGoal {
  std::string rel, src, dst;
};

TextGoal ParseTextGoal(const std::string& spec) {
  const auto lp = spec.find('(');
  const auto rp = spec.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
    throw std::invalid_argument("bad --goal (want rel(src,dst)): " + spec);
  }
  const std::string args = spec.substr(lp + 1, rp - lp - 1);
  const auto comma = args.find(',');
  if (comma == std::string::npos) {
    throw std::invalid_argument("bad --goal (want two arguments): " + spec);
  }
  return {spec.substr(0, lp), args.substr(0, comma),
          args.substr(comma + 1)};
}

bool TextGoalMatches(const TextGoal& g, const EdgeRow& e) {
  return e.rel == g.rel && (g.src == "_" || g.src == e.src) &&
         (g.dst == "_" || g.dst == e.dst);
}

bool TextGoalIn(const TextGoal& g, const std::vector<EdgeRow>& edges) {
  for (const auto& e : edges) {
    if (TextGoalMatches(g, e)) return true;
  }
  return false;
}

// ---------- gen-goals core (shared by gen-goals / gen-workload) ----------

struct SampledGoal {
  std::string rel, src_name, dst_name;
  std::uint32_t slot = 0, src = 0, dst = 0;
  std::uint32_t bstar_round = 0;  // chase first-derivation round (empirical)
  std::uint32_t bstar_dg = 0;     // predicate-level derivation depth (LB)
};

// Holds out the sampled edges, runs the unrestricted chase once, and keeps
// the candidates the closure re-derives (paper §7: "we keep only goals
// that the unrestricted chase derives").
std::vector<SampledGoal> GenReachableGoals(const gar_apps::FullDeduceArgs& io,
                                           std::uint32_t per_slot,
                                           std::uint64_t seed,
                                           deduction::ChaseConfig chase_cfg) {
  gar_apps::FullDeduceInputs in =
      gar_apps::LoadFullDeduceInputs(io, "gar_analyze");

  analysis::PredicateTable table;
  analysis::DependencyGraph dg;
  dg.Build(in.rules, &table);

  analysis::GoalSampler sampler(in.graph, dg);
  const auto slots = sampler.DerivableSlots();
  const auto cands = sampler.Sample(per_slot, seed);
  std::cerr << "[gar_analyze] derivable slots: " << slots.size()
            << ", sampled candidates: " << cands.size() << "\n";

  // Hold out every candidate (base + inverse mirror) at once.
  gar::graph::TripleGraph g_held = in.graph;  // clone (vector memcpy)
  std::unordered_map<std::uint32_t,
                     std::vector<std::pair<std::uint32_t, std::uint32_t>>>
      by_slot;
  for (const auto& c : cands) by_slot[c.slot].emplace_back(c.src, c.dst);
  const std::uint64_t removed = g_held.RemoveEdgesBatch(by_slot);
  std::cerr << "[gar_analyze] held out " << removed << " edges\n";

  chase_cfg.keep_matches = false;  // matches are never needed here
  deduction::CpuChaseEngine engine(
      g_held, in.attrs.columns.empty() ? nullptr : &in.attrs);
  const auto closure = engine.Run(in.rules, chase_cfg);
  if (!closure.fixpoint) {
    std::cerr << "[gar_analyze] NOTE: closure stopped by a cap "
                 "(not a fixpoint); candidates derived beyond the cap are "
                 "dropped as unreachable\n";
  }
  std::cerr << "[gar_analyze] closure derived " << closure.derived.size()
            << " facts in " << closure.rounds << " rounds\n";

  // (slot, src, dst) -> first derivation round.
  std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>,
           std::uint32_t>
      derived_round;
  for (const auto& e : closure.derived) {
    derived_round.emplace(std::make_tuple(e.slot, e.src, e.dst), e.round);
  }
  const auto anchors = analysis::BaseAnchoredPredicates(dg, g_held);
  const auto depths = dg.DerivationDepths(anchors);

  std::vector<SampledGoal> goals;
  for (const auto& c : cands) {
    const auto it =
        derived_round.find(std::make_tuple(c.slot, c.src, c.dst));
    if (it == derived_round.end()) continue;  // not re-derived: dropped
    SampledGoal g;
    g.slot = c.slot;
    g.src = c.src;
    g.dst = c.dst;
    g.rel = in.graph.RelationName(c.slot);
    g.src_name = in.graph.EntityName(c.src);
    g.dst_name = in.graph.EntityName(c.dst);
    g.bstar_round = it->second;
    const std::uint32_t p = table.Find(g.rel);
    g.bstar_dg = (p != analysis::kInvalidPredicate)
                     ? depths[p]
                     : analysis::kUnreachable;
    goals.push_back(std::move(g));
  }
  std::cerr << "[gar_analyze] reachable goals: " << goals.size() << " / "
            << cands.size() << "\n";
  return goals;
}

void WriteGoalsTsv(const std::string& path,
                   const std::vector<SampledGoal>& goals) {
  std::ofstream f(path);
  // B* dual reading: bstar_round is the chase's first-derivation round of
  // the goal (the round-based chase semantics; the paper's exact B*, "cost
  // at which the chase derives g", is approximated by the round under
  // round-based chase); bstar_dg is the dependency-graph derivation depth
  // of the goal's predicate — a lower bound on the shortest chain length
  // (analysis::DependencyGraph::DerivationDepths).
  f << "# rel_name\tsrc\tdst\tbstar_round\tbstar_dg\n";
  for (const auto& g : goals) {
    f << g.rel << '\t' << g.src_name << '\t' << g.dst_name << '\t'
      << g.bstar_round << '\t' << g.bstar_dg << "\n";
  }
}

// ---------- subcommands ----------

int CmdRulesStats(int argc, char** argv) {
  std::string rules_root, dot_path, yaml_path;
  for (int i = 0; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (Usage(), ""); };
    if (k == "--rules") rules_root = next();
    else if (k == "--dot") dot_path = next();
    else if (k == "--yaml") yaml_path = next();
    else Usage();
  }
  if (rules_root.empty()) Usage();

  std::vector<std::filesystem::path> dirs;
  for (const auto& e : std::filesystem::directory_iterator(rules_root)) {
    if (e.is_directory() &&
        std::filesystem::exists(e.path() / "rule.yaml")) {
      dirs.push_back(e.path());
    }
  }
  std::sort(dirs.begin(), dirs.end());
  std::vector<gar::GARule> rules;
  for (const auto& d : dirs) rules.push_back(gar::RuleLoader::Load(d));
  std::cerr << "[gar_analyze] " << rules.size() << " rules\n";

  analysis::PredicateTable table;
  analysis::DependencyGraph dg;
  dg.Build(rules, &table);
  const auto st = analysis::ComputeRuleSetStats(rules, dg);
  analysis::PrintRuleSetStats(std::cout, st, dg);
  if (!dot_path.empty()) {
    dg.WriteDot(dot_path);
    std::cerr << "[gar_analyze] dot written to " << dot_path << "\n";
  }
  if (!yaml_path.empty()) {
    dg.WriteYaml(yaml_path);
    std::cerr << "[gar_analyze] yaml written to " << yaml_path << "\n";
  }
  return 0;
}

// Additive CLI passthrough for the reachability chase inside gen-goals /
// gen-workload (no semantic change: when no flag is given the config is
// exactly the previous default ChaseConfig).
template <typename NextFn>
bool ParseChaseCapOption(const std::string& k, NextFn&& next,
                         deduction::ChaseConfig* cfg) {
  if (k == "--threads")
    cfg->threads = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--max-rounds")
    cfg->max_rounds = static_cast<std::uint32_t>(std::stoul(next()));
  else if (k == "--max-derivations") cfg->max_derivations = std::stoull(next());
  else if (k == "--max-matches")
    cfg->max_matches_per_rule = std::stoull(next());
  else return false;
  return true;
}

int CmdGenGoals(int argc, char** argv) {
  gar_apps::FullDeduceArgs io;
  std::uint32_t per_slot = 0;
  std::uint64_t seed = 42;
  std::string out;
  deduction::ChaseConfig chase_cfg;
  for (int i = 0; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (Usage(), ""); };
    if (k == "--csr") io.csr_dir = next();
    else if (k == "--rules") io.rules_root = next();
    else if (k == "--per-slot")
      per_slot = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--seed") seed = std::stoull(next());
    else if (k == "--out") out = next();
    else if (!ParseChaseCapOption(k, next, &chase_cfg)) Usage();
  }
  if (io.csr_dir.empty() || io.rules_root.empty() || per_slot == 0 ||
      out.empty()) {
    Usage();
  }
  const auto goals = GenReachableGoals(io, per_slot, seed, chase_cfg);
  WriteGoalsTsv(out, goals);
  std::cerr << "[gar_analyze] wrote " << goals.size() << " goals to " << out
            << "\n";
  return 0;
}

int CmdGenWorkload(int argc, char** argv) {
  gar_apps::FullDeduceArgs io;
  std::uint32_t num_tasks = 0;
  std::uint64_t seed = 42;
  std::string out;
  std::vector<double> multipliers = {0.5, 1, 2, 4};
  deduction::ChaseConfig chase_cfg;
  for (int i = 0; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (Usage(), ""); };
    if (k == "--csr") io.csr_dir = next();
    else if (k == "--rules") io.rules_root = next();
    else if (k == "--num-tasks")
      num_tasks = static_cast<std::uint32_t>(std::stoul(next()));
    else if (k == "--seed") seed = std::stoull(next());
    else if (k == "--out") out = next();
    else if (k == "--budgets") {
      multipliers.clear();
      const std::string v = next();
      std::size_t pos = 0;
      while (pos <= v.size()) {
        const auto comma = v.find(',', pos);
        multipliers.push_back(
            std::stod(v.substr(pos, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - pos)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (!ParseChaseCapOption(k, next, &chase_cfg)) {
      Usage();
    }
  }
  if (io.csr_dir.empty() || io.rules_root.empty() || num_tasks == 0 ||
      out.empty()) {
    Usage();
  }

  // Oversample to cover hold-out losses, then keep the first N reachable
  // goals (sampler order is deterministic given the seed).
  gar_apps::FullDeduceInputs in =
      gar_apps::LoadFullDeduceInputs(io, "gar_analyze");
  analysis::PredicateTable table;
  analysis::DependencyGraph dg;
  dg.Build(in.rules, &table);
  const auto n_slots =
      analysis::GoalSampler(in.graph, dg).DerivableSlots().size();
  const std::uint32_t per_slot = static_cast<std::uint32_t>(std::max<std::uint64_t>(
      1, (4ull * num_tasks + (n_slots ? n_slots : 1) - 1) /
             (n_slots ? n_slots : 1)));
  // NOTE: LoadFullDeduceInputs was already run above for the slot count;
  // GenReachableGoals reloads (loaders are cheap relative to the chase).
  auto goals = GenReachableGoals(io, per_slot, seed, chase_cfg);
  if (goals.size() > num_tasks) goals.resize(num_tasks);

  std::ofstream f(out);
  // Budget ladder anchored at the DG lower bound: B in {m * max(bstar_dg,
  // 1)}, rounded up. This is deliberately a TIGHT budget anchor — the 5b
  // scale finding is that exact branch-and-bound is infeasible on large
  // dependency-graph neighborhoods with loose budgets, while tight budgets
  // prune via c + price <= B by construction.
  f << "# goal_rel\tsrc\tdst\tbudget\tbstar_dg\tbstar_round\n";
  std::size_t n_rows = 0;
  for (const auto& g : goals) {
    const std::uint64_t anchor =
        std::max<std::uint32_t>(g.bstar_dg, 1);
    for (const double m : multipliers) {
      const auto b = static_cast<std::uint64_t>(
          std::ceil(m * static_cast<double>(anchor)));
      f << g.rel << '\t' << g.src_name << '\t' << g.dst_name << '\t' << b
        << '\t' << g.bstar_dg << '\t' << g.bstar_round << "\n";
      ++n_rows;
    }
  }
  std::cerr << "[gar_analyze] wrote " << n_rows << " tasks (" << goals.size()
            << " goals x " << multipliers.size() << " budgets) to " << out
            << "\n";
  return 0;
}

int CmdChainCmp(int argc, char** argv) {
  std::string agent_dir, closure_dir, goal_spec;
  for (int i = 0; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (Usage(), ""); };
    if (k == "--agent-out") agent_dir = next();
    else if (k == "--closure-out") closure_dir = next();
    else if (k == "--goal") goal_spec = next();
    else Usage();
  }
  if (agent_dir.empty() || closure_dir.empty()) Usage();

  const auto agent_edges = ReadClosureEdges(agent_dir);
  const auto closure_edges = ReadClosureEdges(closure_dir);
  std::vector<EdgeRow> missing;
  for (const auto& e : agent_edges) {
    bool found = false;
    for (const auto& c : closure_edges) {
      if (e.src == c.src && e.rel == c.rel && e.dst == c.dst) {
        found = true;
        break;
      }
    }
    if (!found) missing.push_back(e);
  }
  const auto agent_cost =
      ReadYamlScalar(agent_dir + "/summary.yaml", "total_cost");
  const auto closure_total =
      ReadYamlScalar(closure_dir + "/summary.yaml", "derived_total");

  std::cout << "[gar_analyze] chain-cmp: " << agent_dir << " vs "
            << closure_dir << "\n"
            << "  agent derived:   " << agent_edges.size() << "\n"
            << "  closure derived: " << closure_edges.size() << "\n"
            << "  subset (soundness): "
            << (missing.empty() ? "PASS" : "FAIL") << "\n";
  if (agent_cost && closure_total) {
    std::cout << "  cost: agent total_cost=" << *agent_cost
              << " vs closure derived_total=" << *closure_total << "\n";
  }
  if (!missing.empty()) {
    std::cout << "  facts outside the closure:\n";
    for (const auto& e : missing) {
      std::cout << "    " << e.rel << "(" << e.src << "," << e.dst << ")\n";
    }
  }
  if (!goal_spec.empty()) {
    const TextGoal g = ParseTextGoal(goal_spec);
    std::cout << "  goal " << goal_spec << ": agent="
              << (TextGoalIn(g, agent_edges) ? "yes" : "no")
              << " closure="
              << (TextGoalIn(g, closure_edges) ? "yes" : "no") << "\n";
  }
  return missing.empty() ? 0 : 1;
}

// ---------- verify ----------

// Verdict rules (the contract of the verify subcommand):
//   CONSISTENT      goal in the cpu closure (and the gpu closure agrees
//                   when given), the agent derived the goal, and the
//                   agent's derived set is a subset of the cpu closure.
//   BUDGET-LIMITED  goal in the closure but the agent did not derive it,
//                   AND (budget < bstar_dg or the agent exhausted its
//                   budget) — the expected under-budget behavior. Exit 0.
//   DIVERGENT       the agent derived a fact outside the closure / the
//                   goal is derivable, the budget was sufficient
//                   (>= bstar_dg) and unexhausted, yet the agent missed it
//                   (fair-policy violation, Thm 3) / cpu and gpu closures
//                   disagree on the goal. Exit 1.
//   INFEASIBLE      (extra, documented) the goal is not in the closure at
//                   all and the agent did not derive it — nothing to
//                   cross-check. Exit 0.
struct VerdictInput {
  bool goal_in_cpu = false;
  bool has_gpu = false;
  bool goal_in_gpu = false;
  bool agent_goal = false;
  bool subset_ok = true;
  bool budget_exhausted = false;
  std::uint64_t budget = 0;
  std::uint32_t bstar_dg = analysis::kUnreachable;
  std::vector<std::string> divergent_facts;
};

std::string VerdictOf(const VerdictInput& v) {
  if (!v.subset_ok) return "DIVERGENT";
  if (v.has_gpu && v.goal_in_cpu != v.goal_in_gpu) return "DIVERGENT";
  if (v.goal_in_cpu) {
    if (v.agent_goal) return "CONSISTENT";
    const bool under_budgeted =
        (v.bstar_dg != analysis::kUnreachable && v.budget < v.bstar_dg);
    return (under_budgeted || v.budget_exhausted) ? "BUDGET-LIMITED"
                                                  : "DIVERGENT";
  }
  return v.agent_goal ? "DIVERGENT" : "INFEASIBLE";
}

struct VerifyArgs {
  bool run = false;
  bool gpu = false;
  std::vector<std::string> from_dirs;
  std::string tasks_path;
  gar_apps::FullDeduceArgs io;
  gar_apps::AgentModeArgs agent;
  std::string report = "verify_report.yaml";
  deduction::ChaseConfig chase_cfg;  // caps for the closure cross-check
  // --shared-holdout: paper's exact workload semantics (§7) — hold out ALL
  // task goals at once (Γ0 = G \ X) and share one closure across tasks,
  // instead of the default per-task hold-out (Γ0 = G \ {g}).
  bool shared_holdout = false;
};

VerifyArgs ParseVerifyArgs(int argc, char** argv) {
  VerifyArgs a;
  for (int i = 0; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--run") a.run = true;
    else if (k == "--gpu") a.gpu = true;
    else if (k == "--from-dirs") {
      if (i + 2 >= argc) Usage();
      a.from_dirs.push_back(argv[++i]);
      a.from_dirs.push_back(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        a.from_dirs.push_back(argv[++i]);
      }
    } else if (k == "--tasks") a.tasks_path = next();
    else if (k == "--shared-holdout") a.shared_holdout = true;
    else if (k == "--report") a.report = next();
    else if (k == "--csr") a.io.csr_dir = next();
    else if (k == "--rules") a.io.rules_root = next();
    else if (ParseChaseCapOption(k, next, &a.chase_cfg)) {
    } else if (!gar_apps::ParseAgentModeOption(k, next, &a.agent)) {
      std::cerr << "unknown option: " << k << "\n";
      Usage();
    }
  }
  const int modes = (a.run ? 1 : 0) + (!a.from_dirs.empty() ? 1 : 0) +
                    (!a.tasks_path.empty() ? 1 : 0);
  if (modes != 1) Usage();
  if ((a.run || !a.tasks_path.empty()) &&
      (a.io.csr_dir.empty() || a.io.rules_root.empty())) {
    Usage();
  }
  if ((a.run || !a.from_dirs.empty()) && a.agent.goal_spec.empty()) Usage();
  return a;
}

void WriteVerifyReport(const std::string& path, const TextGoal& goal,
                       const VerdictInput& v, const std::string& verdict,
                       double agent_cost, std::uint64_t agent_steps) {
  std::ofstream f(path);
  f << "goal: \"" << goal.rel << "(" << goal.src << "," << goal.dst
    << ")\"\n"
    << "goal_in_cpu_closure: " << (v.goal_in_cpu ? "true" : "false") << "\n";
  if (v.has_gpu) {
    f << "goal_in_gpu_closure: " << (v.goal_in_gpu ? "true" : "false")
      << "\n";
  }
  f << "agent_goal_derived: " << (v.agent_goal ? "true" : "false") << "\n"
    << "agent_total_cost: " << agent_cost << "\n"
    << "agent_steps: " << agent_steps << "\n"
    << "budget: " << v.budget << "\n"
    << "bstar_dg: "
    << (v.bstar_dg == analysis::kUnreachable
            ? "unreachable"
            : std::to_string(v.bstar_dg))
    << "\n"
    << "subset_ok: " << (v.subset_ok ? "true" : "false") << "\n"
    << "verdict: " << verdict << "\n";
  if (!v.divergent_facts.empty()) {
    f << "divergent_facts:\n";
    for (const auto& s : v.divergent_facts) f << "  - \"" << s << "\"\n";
  }
}

// One in-process verify run: agent + cpu chase (+ optional gpu), verdict.
// `g_base` stays pristine; both engines work on clones of the HELD-OUT
// graph: following the paper's task definition (§7: a task takes a held-out
// triple as g and the rest of the graph as Γ0), a fully ground goal edge is
// removed (base + inverse mirror) before either engine runs. Open-argument
// goals skip the hold-out (no specific edge to remove).
VerdictInput VerifyOne(const gar_apps::FullDeduceArgs& io,
                       const gar_apps::AgentModeArgs& agent_args,
                       const agent::Goal& goal,
                       gar::graph::TripleGraph* g_base,
                       const std::vector<gar::GARule>& rules,
                       const std::vector<std::string>& rule_ids,
                       const gar::graph::VertexAttrs* attrs,
                       const analysis::DependencyGraph& dg,
                       const std::vector<std::uint32_t>& depths,
                       bool with_gpu, agent::AgentResult* agent_out,
                       const deduction::ChaseConfig& chase_cfg) {
  VerdictInput v;
  v.budget = agent_args.budget;

  // Hold out the goal edge: Γ0 = G \ {g}.
  gar::graph::TripleGraph g0 = *g_base;
  if (goal.args[0] && goal.args[1]) {
    g0.RemoveEdgesBatch(
        {{goal.slot, {{*goal.args[0], *goal.args[1]}}}});
  }

  // agent on a clone (greedy mutates; bnb clones internally — either way
  // the base stays untouched)
  {
    gar::graph::TripleGraph g_agent = g0;
    agent::ReasoningAgent rag = gar_apps::MakeReasoningAgent(
        agent_args, &dg, &goal, &g0, &rules, &rule_ids);
    agent::AgentState state(g_agent, attrs, rules);
    agent::ExecConfig ec;
    ec.budget = agent_args.budget;
    ec.match_cap_per_rule = agent_args.max_candidates_per_rule;
    ec.top_k = agent_args.top_k;
    ec.utility_gamma = agent_args.score.gamma;
    ec.open_collect_cap = agent_args.open_collect_cap;
    ec.backward_order = agent_args.backward_order;
    ec.backward_full_enum = agent_args.backward_full_enum;
    ec.seed = agent_args.seed;
    *agent_out = rag.Run(&state, goal, ec);
  }
  v.agent_goal = agent_out->goal_derived;
  v.budget_exhausted = agent_out->budget_exhausted;

  // cpu closure on a clone
  deduction::ChaseConfig cfg = chase_cfg;
  cfg.keep_matches = false;
  gar::graph::TripleGraph g_cpu = g0;
  deduction::CpuChaseEngine cpu_engine(g_cpu, attrs);
  const auto cpu_res = cpu_engine.Run(rules, cfg);
  auto contains_goal = [](const gar::graph::TripleGraph& g,
                          const auto& derived, const agent::Goal& gl) {
    if (gl.args[0] && gl.args[1] &&
        g.RelationPairSet(gl.slot)
            .count(gar::graph::PackPair(*gl.args[0], *gl.args[1]))) {
      return true;  // base edge or derived-and-merged
    }
    for (const auto& e : derived) {
      if (agent::GoalSatisfied(gl, e)) return true;
    }
    return false;
  };
  v.goal_in_cpu = contains_goal(g_cpu, cpu_res.derived, goal);
  // subset: every agent-derived fact sits in the cpu closure graph
  for (const auto& e : agent_out->derived) {
    if (!g_cpu.RelationPairSet(e.slot)
             .count(gar::graph::PackPair(e.src, e.dst))) {
      v.subset_ok = false;
      v.divergent_facts.push_back(
          g_base->SlotName(e.slot) + "(" + g_base->EntityName(e.src) + "," +
          g_base->EntityName(e.dst) + ")");
    }
  }

  if (with_gpu) {
    v.has_gpu = true;
    gar::graph::TripleGraph g_gpu = g0;
    deduction::GpuDeduceEngine gpu_engine(g_gpu, attrs);
    const auto gpu_res = gpu_engine.Run(rules, cfg);
    v.goal_in_gpu = contains_goal(g_gpu, gpu_res.derived, goal);
  }

  const std::uint32_t p =
      dg.table().Find(g_base->RelationName(goal.slot));
  v.bstar_dg = p == analysis::kInvalidPredicate ? analysis::kUnreachable
                                                : depths[p];
  return v;
}

int CmdVerifyRun(const VerifyArgs& a) {
  gar_apps::FullDeduceInputs in =
      gar_apps::LoadFullDeduceInputs(a.io, "gar_analyze");
  analysis::PredicateTable table;
  analysis::DependencyGraph dg;
  dg.Build(in.rules, &table);
  const auto anchors = analysis::BaseAnchoredPredicates(dg, in.graph);
  const auto depths = dg.DerivationDepths(anchors);

  const agent::Goal goal =
      gar_apps::ParseGoalSpec(a.agent.goal_spec, in.graph);
  agent::AgentResult agent_res;
  const gar::graph::VertexAttrs* attrs =
      in.attrs.columns.empty() ? nullptr : &in.attrs;
  const VerdictInput v =
      VerifyOne(a.io, a.agent, goal, &in.graph, in.rules, in.rule_ids, attrs, dg, depths,
                a.gpu, &agent_res, a.chase_cfg);
  const std::string verdict = VerdictOf(v);
  WriteVerifyReport(a.report, ParseTextGoal(a.agent.goal_spec), v, verdict,
                    agent_res.total_cost, agent_res.chain.size());
  std::cout << "[gar_analyze] verify: goal " << a.agent.goal_spec << " -> "
            << verdict << " (agent steps=" << agent_res.chain.size()
            << " cost=" << agent_res.total_cost
            << ", cpu closure goal=" << (v.goal_in_cpu ? "yes" : "no")
            << ")\n";
  std::cerr << "[gar_analyze] report written to " << a.report << "\n";
  return verdict == "DIVERGENT" ? 1 : 0;
}

int CmdVerifyFromDirs(const VerifyArgs& a) {
  const TextGoal goal = ParseTextGoal(a.agent.goal_spec);
  const auto agent_edges = ReadClosureEdges(a.from_dirs[0]);
  const auto cpu_edges = ReadClosureEdges(a.from_dirs[1]);
  VerdictInput v;
  v.goal_in_cpu = TextGoalIn(goal, cpu_edges);
  v.agent_goal = TextGoalIn(goal, agent_edges);
  if (a.from_dirs.size() == 3) {
    v.has_gpu = true;
    v.goal_in_gpu = TextGoalIn(goal, ReadClosureEdges(a.from_dirs[2]));
  }
  // From-dirs has no dependency graph: bstar is unknown, so the
  // BUDGET-LIMITED reading degrades to the agent's own budget_exhausted
  // flag (recorded in its summary.yaml).
  const auto exhausted = ReadYamlScalar(a.from_dirs[0] + "/summary.yaml",
                                        "budget_exhausted");
  v.budget_exhausted = exhausted && *exhausted == "true";
  const auto cost =
      ReadYamlScalar(a.from_dirs[0] + "/summary.yaml", "total_cost");
  for (const auto& e : agent_edges) {
    if (!TextGoalIn({e.rel, e.src, e.dst}, cpu_edges)) {
      v.subset_ok = false;
      v.divergent_facts.push_back(e.rel + "(" + e.src + "," + e.dst + ")");
    }
  }
  const std::string verdict = VerdictOf(v);
  WriteVerifyReport(a.report, goal, v, verdict,
                    cost ? std::stod(*cost) : 0.0, agent_edges.size());
  std::cout << "[gar_analyze] verify --from-dirs: goal "
            << a.agent.goal_spec << " -> " << verdict << "\n";
  std::cerr << "[gar_analyze] report written to " << a.report << "\n";
  return verdict == "DIVERGENT" ? 1 : 0;
}

int CmdVerifyTasks(const VerifyArgs& a) {
  gar_apps::FullDeduceInputs in =
      gar_apps::LoadFullDeduceInputs(a.io, "gar_analyze");
  analysis::PredicateTable table;
  analysis::DependencyGraph dg;
  dg.Build(in.rules, &table);
  const auto anchors = analysis::BaseAnchoredPredicates(dg, in.graph);
  const auto depths = dg.DerivationDepths(anchors);
  const gar::graph::VertexAttrs* attrs =
      in.attrs.columns.empty() ? nullptr : &in.attrs;

  std::ifstream tf(a.tasks_path);
  if (!tf) throw std::runtime_error("cannot open " + a.tasks_path);
  std::string line;
  std::size_t n_consistent = 0, n_budget = 0, n_divergent = 0,
              n_infeasible = 0, n_derived = 0, n_feasible = 0;
  std::ofstream rep(a.report);
  rep << "# workload verify: " << a.tasks_path << "\n"
      << "# goal\tbudget\tverdict\tagent_steps\tagent_cost\tagent_wall_ms\n";
  // Shared-closure state for the no-op hold-out fast path and for
  // --shared-holdout (see loop).
  bool shared_closure_ready = false;
  std::optional<gar::graph::TripleGraph> shared_g_cpu, shared_g_gpu;
  std::optional<deduction::ChaseResult> shared_cpu_res, shared_gpu_res;
  // --shared-holdout: Γ0 = G minus ALL task goal edges (base ones), once.
  gar::graph::TripleGraph g0_shared = in.graph;
  if (a.shared_holdout) {
    std::unordered_map<std::uint32_t,
                       std::vector<std::pair<std::uint32_t, std::uint32_t>>>
        by_slot;
    std::ifstream tf0(a.tasks_path);
    std::string l0;
    while (std::getline(tf0, l0)) {
      if (l0.empty() || l0[0] == '#') continue;
      std::vector<std::string> cols;
      std::size_t pos = 0;
      while (pos <= l0.size()) {
        const auto tab = l0.find('\t', pos);
        cols.push_back(l0.substr(
            pos, tab == std::string::npos ? std::string::npos : tab - pos));
        if (tab == std::string::npos) break;
        pos = tab + 1;
      }
      if (cols.size() < 4) continue;
      const agent::Goal gl = gar_apps::ParseGoalSpec(
          cols[0] + "(" + cols[1] + "," + cols[2] + ")", in.graph);
      if (gl.args[0] && gl.args[1]) {
        by_slot[gl.slot].emplace_back(*gl.args[0], *gl.args[1]);
      }
    }
    const std::uint64_t removed = g0_shared.RemoveEdgesBatch(by_slot);
    std::cerr << "[gar_analyze] shared hold-out: removed " << removed
              << " goal edges from Γ0\n";
  }
  // Per task the goal edge is held out and both engines re-run on the
  // held-out graph (paper task semantics: Γ0 = G minus the held-out goal).
  // NOTE: this makes --tasks cost one closure per task — fine on small
  // graphs; on fb15k-scale graphs prefer generating tasks with the shared
  // hold-out set in mind (gen-workload) and verifying a few spot tasks.
  while (std::getline(tf, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> cols;
    std::size_t pos = 0;
    while (pos <= line.size()) {
      const auto tab = line.find('\t', pos);
      cols.push_back(line.substr(
          pos, tab == std::string::npos ? std::string::npos : tab - pos));
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
    if (cols.size() < 4) continue;
    const std::string spec = cols[0] + "(" + cols[1] + "," + cols[2] + ")";
    gar_apps::AgentModeArgs task_args = a.agent;
    task_args.goal_spec = spec;
    task_args.budget = std::stoull(cols[3]);

    const agent::Goal goal = gar_apps::ParseGoalSpec(spec, in.graph);
    agent::AgentResult agent_res;
    // Hold-out regimes. Default (paper §7 per-task approximation): each
    // base-edge goal is held out alone, chased per task (VerifyOne); a
    // non-base goal makes the hold-out a no-op and shares one closure.
    // --shared-holdout (paper's exact wording: "we held out X triples ...
    // the rest of the graph as Γ0"): all task goals are held out together
    // and one closure is shared by the whole workload.
    bool goal_is_base =
        goal.args[0] && goal.args[1] &&
        in.graph.RelationPairSet(goal.slot)
            .count(gar::graph::PackPair(*goal.args[0], *goal.args[1])) != 0;
    if (a.shared_holdout) goal_is_base = false;  // handled by g0 below
    double agent_wall_ms = 0.0;
    VerdictInput v;
    if (goal_is_base && !a.shared_holdout) {
      v = VerifyOne(a.io, task_args, goal, &in.graph, in.rules, in.rule_ids,
                    attrs, dg, depths, a.gpu, &agent_res, a.chase_cfg);
    } else {
      if (!shared_closure_ready) {
        // Fast path: hold-out is a no-op → Γ0 = G. Shared-holdout path:
        // Γ0 = G minus all task goals (prepared above).
        shared_g_cpu = a.shared_holdout ? g0_shared : in.graph;
        deduction::ChaseConfig cfg = a.chase_cfg;
        cfg.keep_matches = false;
        deduction::CpuChaseEngine eng(*shared_g_cpu, attrs);
        shared_cpu_res = eng.Run(in.rules, cfg);
        if (a.gpu) {
          shared_g_gpu = a.shared_holdout ? g0_shared : in.graph;
          deduction::GpuDeduceEngine geng(*shared_g_gpu, attrs);
          shared_gpu_res = geng.Run(in.rules, cfg);
        }
        shared_closure_ready = true;
      }
      const auto a_t0 = std::chrono::steady_clock::now();
      {
        gar::graph::TripleGraph g_agent =
            a.shared_holdout ? g0_shared : in.graph;
        agent::ReasoningAgent rag =
            gar_apps::MakeReasoningAgent(task_args, &dg, &goal,
                                         &g_agent, &in.rules, &in.rule_ids);
        agent::AgentState state(g_agent, attrs, in.rules);
        agent::ExecConfig ec;
        ec.budget = task_args.budget;
        ec.match_cap_per_rule = task_args.max_candidates_per_rule;
        ec.top_k = task_args.top_k;
        ec.utility_gamma = task_args.score.gamma;
        ec.open_collect_cap = task_args.open_collect_cap;
        ec.backward_order = task_args.backward_order;
        ec.backward_full_enum = task_args.backward_full_enum;
        ec.seed = task_args.seed;
        agent_res = rag.Run(&state, goal, ec);
      }
      agent_wall_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - a_t0)
                          .count();
      v.budget = task_args.budget;
      v.agent_goal = agent_res.goal_derived;
      v.budget_exhausted = agent_res.budget_exhausted;
      // Fast path implies the goal is not a base edge, so "in the cpu
      // closure" reduces to "derived by the shared chase".
      v.goal_in_cpu = false;
      for (const auto& e : shared_cpu_res->derived) {
        if (agent::GoalSatisfied(goal, e)) {
          v.goal_in_cpu = true;
          break;
        }
      }
      if (a.gpu) {
        v.has_gpu = true;
        v.goal_in_gpu = false;
        for (const auto& e : shared_gpu_res->derived) {
          if (agent::GoalSatisfied(goal, e)) {
            v.goal_in_gpu = true;
            break;
          }
        }
      }
      for (const auto& e : agent_res.derived) {
        if (!shared_g_cpu->RelationPairSet(e.slot)
                 .count(gar::graph::PackPair(e.src, e.dst))) {
          v.subset_ok = false;
        }
      }
      const std::uint32_t p =
          dg.table().Find(in.graph.RelationName(goal.slot));
      v.bstar_dg = p == analysis::kInvalidPredicate
                       ? analysis::kUnreachable
                       : depths[p];
    }
    const std::string verdict = VerdictOf(v);
    if (verdict == "CONSISTENT") ++n_consistent;
    else if (verdict == "BUDGET-LIMITED") ++n_budget;
    else if (verdict == "DIVERGENT") ++n_divergent;
    else ++n_infeasible;
    if (v.goal_in_cpu) {
      ++n_feasible;
      if (v.agent_goal) ++n_derived;
    }
    rep << spec << '\t' << task_args.budget << '\t' << verdict << '\t'
        << agent_res.chain.size() << '\t' << agent_res.total_cost << '\t'
        << agent_wall_ms << "\n";
  }
  std::cout << "[gar_analyze] verify --tasks: " << a.tasks_path << "\n"
            << "  CONSISTENT=" << n_consistent
            << " BUDGET-LIMITED=" << n_budget
            << " DIVERGENT=" << n_divergent
            << " INFEASIBLE=" << n_infeasible << "\n"
            << "  success rate (derived/feasible): " << n_derived << "/"
            << n_feasible << "\n";
  std::cerr << "[gar_analyze] report written to " << a.report << "\n";
  return n_divergent == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) Usage();
  const std::string sub = argv[1];
  try {
    if (sub == "rules-stats") return CmdRulesStats(argc - 2, argv + 2);
    if (sub == "gen-goals") return CmdGenGoals(argc - 2, argv + 2);
    if (sub == "gen-workload") return CmdGenWorkload(argc - 2, argv + 2);
    if (sub == "chain-cmp") return CmdChainCmp(argc - 2, argv + 2);
    if (sub == "verify") {
      const VerifyArgs a = ParseVerifyArgs(argc - 2, argv + 2);
      if (a.run) return CmdVerifyRun(a);
      if (!a.from_dirs.empty()) return CmdVerifyFromDirs(a);
      return CmdVerifyTasks(a);
    }
    Usage();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
