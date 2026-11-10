#include "gar/analysis/rule_stats.h"

#include <algorithm>
#include <vector>

namespace gar {
namespace analysis {

namespace {

template <typename T>
T Median(std::vector<T> v) {
  if (v.empty()) return T{};
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

RuleSetStats ComputeRuleSetStats(const std::vector<gar::GARule>& rules,
                                 const DependencyGraph& dg) {
  RuleSetStats st;
  st.num_rules = rules.size();
  std::vector<std::uint64_t> supports;
  std::vector<double> confidences;
  for (const auto& r : rules) {
    if (r.IsQuery()) ++st.num_queries;
    ++st.pattern_vertices_hist[r.num_vertices()];
    ++st.pattern_edges_hist[r.num_edges()];
    supports.push_back(r.quality.support);
    confidences.push_back(r.quality.confidence);
    for (const auto& p : r.antecedent) {
      switch (p.kind) {
        case gar::PredKind::kConst: ++st.pred_const; break;
        case gar::PredKind::kVar: ++st.pred_var; break;
        case gar::PredKind::kExists: ++st.pred_exists; break;
        case gar::PredKind::kEdge: ++st.pred_edge; break;
        case gar::PredKind::kMl: ++st.pred_ml; break;
      }
    }
  }
  if (!supports.empty()) {
    st.support_min = *std::min_element(supports.begin(), supports.end());
    st.support_max = *std::max_element(supports.begin(), supports.end());
    st.support_median = Median(supports);
    st.confidence_min =
        *std::min_element(confidences.begin(), confidences.end());
    st.confidence_max =
        *std::max_element(confidences.begin(), confidences.end());
    st.confidence_median = Median(confidences);
  }

  st.num_predicates = dg.NumPredicates();
  st.num_arcs = dg.NumArcs();
  for (std::uint32_t p = 0; p < dg.NumPredicates(); ++p) {
    const bool has_in = dg.InDegree(p) > 0;
    const bool has_out = dg.OutDegree(p) > 0;
    if (has_in) ++st.derivable_preds;
    if (has_out && !has_in) ++st.source_preds;
    if (has_in && !has_out) ++st.sink_preds;
  }
  const auto sccs = dg.StronglyConnectedComponents();
  st.num_sccs = sccs.size();
  for (const auto& s : sccs) {
    if (s.recursive) {
      ++st.num_recursive_sccs;
      st.max_recursive_scc_size =
          std::max<std::uint64_t>(st.max_recursive_scc_size, s.members.size());
    }
  }
  // Longest shortest distance: backward BFS from every sink, keep the
  // maximal finite d.
  for (std::uint32_t p = 0; p < dg.NumPredicates(); ++p) {
    if (dg.InDegree(p) == 0 || dg.OutDegree(p) != 0) continue;  // sinks only
    for (const std::uint32_t d : dg.DistancesTo(p)) {
      if (d != kUnreachable) st.max_distance = std::max(st.max_distance, d);
    }
  }
  return st;
}

void PrintRuleSetStats(std::ostream& os, const RuleSetStats& st,
                       const DependencyGraph& dg) {
  os << "rule set: " << st.num_rules << " rules (" << st.num_queries
     << " queries)\n";
  auto print_hist = [&](const char* label,
                        const std::map<std::uint32_t, std::uint64_t>& h) {
    os << label;
    bool first = true;
    for (const auto& [size, count] : h) {
      if (!first) os << ", ";
      os << size << ":" << count;
      first = false;
    }
    os << "\n";
  };
  print_hist("pattern vertices: ", st.pattern_vertices_hist);
  print_hist("pattern edges:    ", st.pattern_edges_hist);
  os << "antecedent predicates: const=" << st.pred_const
     << " var=" << st.pred_var << " exists=" << st.pred_exists
     << " edge=" << st.pred_edge << " ml=" << st.pred_ml << "\n";
  os << "support:    min=" << st.support_min << " median="
     << st.support_median << " max=" << st.support_max << "\n";
  os << "confidence: min=" << st.confidence_min
     << " median=" << st.confidence_median << " max=" << st.confidence_max
     << "\n";
  os << "dependency graph: " << st.num_predicates << " predicates, "
     << st.num_arcs << " arcs\n";
  os << "  derivable predicates: " << st.derivable_preds
     << " (appear as a consequent)\n";
  os << "  source predicates:    " << st.source_preds
     << " (precondition only)\n";
  os << "  sink predicates:      " << st.sink_preds << " (consequent only)\n";
  os << "  SCCs: " << st.num_sccs << " (" << st.num_recursive_sccs
     << " recursive, largest family " << st.max_recursive_scc_size
     << " predicates)\n";
  os << "  longest shortest distance to a sink: " << st.max_distance << "\n";
  (void)dg;
}

}  // namespace analysis
}  // namespace gar
