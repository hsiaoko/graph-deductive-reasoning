#pragma once

// RuleSetStats: descriptive statistics of a rule set and of its dependency
// shape — the human-readable "what does this rule set look like" report
// (gar_analyze rules-stats prints it verbatim).

#include <cstdint>
#include <map>
#include <ostream>
#include <vector>

#include "gar/analysis/dependency_graph.h"
#include "gar/rule.h"

namespace gar::core::analysis {

struct RuleSetStats {
  std::uint64_t num_rules = 0;
  std::uint64_t num_queries = 0;  // rules without a consequent

  // Pattern size histograms: size -> number of rules.
  std::map<std::uint32_t, std::uint64_t> pattern_vertices_hist;
  std::map<std::uint32_t, std::uint64_t> pattern_edges_hist;

  std::uint64_t support_min = 0, support_median = 0, support_max = 0;
  double confidence_min = 0.0, confidence_median = 0.0, confidence_max = 0.0;

  // Antecedent predicate kinds over all rules.
  std::uint64_t pred_const = 0, pred_var = 0, pred_exists = 0;
  std::uint64_t pred_edge = 0, pred_ml = 0;

  // Dependency shape.
  std::uint64_t num_predicates = 0;
  std::uint64_t num_arcs = 0;
  std::uint64_t derivable_preds = 0;   // in-degree > 0 (some rule's head)
  std::uint64_t source_preds = 0;      // out > 0, in == 0 (precondition only)
  std::uint64_t sink_preds = 0;        // in > 0, out == 0 (consequent only)
  std::uint64_t num_sccs = 0;
  std::uint64_t num_recursive_sccs = 0;
  std::uint64_t max_recursive_scc_size = 0;
  // Longest shortest backward distance: max over sink predicates of the
  // maximal finite d(.) towards that sink (kUnreachable excluded).
  std::uint32_t max_distance = 0;
};

RuleSetStats ComputeRuleSetStats(const std::vector<gar::GARule>& rules,
                                 const DependencyGraph& dg);

void PrintRuleSetStats(std::ostream& os, const RuleSetStats& stats,
                       const DependencyGraph& dg);

}  // namespace gar::core::analysis
