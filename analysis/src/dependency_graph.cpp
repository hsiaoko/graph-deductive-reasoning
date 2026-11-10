#include "gar/analysis/dependency_graph.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gar {
namespace analysis {

namespace {

// Quotes a string for YAML/Graphviz double-quoted contexts.
std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + '"';
}

}  // namespace

std::vector<std::uint32_t> DependencyGraph::DerivationDepths(
    const std::vector<char>& base_anchored) const {
  const std::size_t n = table_->size();

  // Group arcs into rules: one consequent per rule, so a rule's arcs share
  // the head (dst); premises = distinct src set.
  struct RuleArcs {
    std::uint32_t head = 0;
    std::vector<std::uint32_t> premises;
  };
  std::unordered_map<std::uint32_t, RuleArcs> by_rule;
  for (const auto& a : arcs_) {
    auto& ra = by_rule[a.rule];
    ra.head = a.dst;
    if (std::find(ra.premises.begin(), ra.premises.end(), a.src) ==
        ra.premises.end()) {
      ra.premises.push_back(a.src);
    }
  }

  // Dijkstra over the rule hypergraph, tracking two depth notions:
  //   cost[p]   premise-side price of p: 0 when base-anchored (readable
  //             from data), else the settled derivation depth;
  //   target[p] derive-side price of p: min over rules deriving p of
  //             1 + max premise cost — the fewest applications that can
  //             derive a NEW fact of p (this is the bstar_dg read; an
  //             anchored but derivable p still gets a finite target).
  // A rule is ready when all premises have finite cost; its candidate is
  // 1 + max premise cost. Candidates are popped in increasing order, so
  // the first candidate settling a head is its minimum (least fixpoint).
  struct RuleState {
    std::uint32_t head;
    std::uint32_t pending;      // premises without a finite cost yet
    std::uint32_t max_settled;  // max cost over ready premises
  };
  std::vector<RuleState> rstates;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      used_by;  // predicate -> rule indices referencing it as premise
  rstates.reserve(by_rule.size());
  std::vector<std::uint32_t> cost(n, kUnreachable);
  std::vector<std::uint32_t> target(n, kUnreachable);
  for (std::uint32_t p = 0; p < n; ++p) {
    if (p < base_anchored.size() && base_anchored[p]) cost[p] = 0;
  }
  for (const auto& [rule_idx, ra] : by_rule) {
    (void)rule_idx;
    RuleState rs;
    rs.head = ra.head;
    rs.pending = 0;
    rs.max_settled = 0;
    for (const std::uint32_t q : ra.premises) {
      if (cost[q] == kUnreachable) {
        ++rs.pending;
      } else {
        rs.max_settled = std::max(rs.max_settled, cost[q]);
      }
      used_by[q].push_back(static_cast<std::uint32_t>(rstates.size()));
    }
    rstates.push_back(rs);
  }
  using Entry = std::pair<std::uint32_t, std::uint32_t>;  // (cand, rule)
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pq;
  for (std::uint32_t i = 0; i < rstates.size(); ++i) {
    if (rstates[i].pending == 0) {
      pq.emplace(rstates[i].max_settled + 1, i);
    }
  }
  std::vector<char> settled(n, 0);
  while (!pq.empty()) {
    const auto [d, ri] = pq.top();
    pq.pop();
    const RuleState& rs = rstates[ri];
    if (settled[rs.head]) continue;  // an earlier (smaller) candidate won
    settled[rs.head] = 1;
    target[rs.head] = d;
    if (cost[rs.head] == kUnreachable) {
      // Non-anchored head becomes premise-usable at its derivation depth.
      cost[rs.head] = d;
      for (const std::uint32_t rj : used_by[rs.head]) {
        RuleState& s = rstates[rj];
        if (s.pending == 0) continue;
        s.max_settled = std::max(s.max_settled, d);
        if (--s.pending == 0) pq.emplace(s.max_settled + 1, rj);
      }
    }
  }
  return target;
}

std::vector<char> BaseAnchoredPredicates(const DependencyGraph& dg,
                                         const graph::TripleGraph& g) {
  // Name -> base relation id map, built once (the graph has no reverse
  // dictionary).
  std::unordered_map<std::string, std::uint32_t> rel_by_name;
  for (std::uint32_t r = 0; r < g.num_relations(); ++r) {
    rel_by_name.emplace(g.RelationName(r), r);
  }
  std::vector<char> anchored(dg.NumPredicates(), 0);
  for (std::uint32_t p = 0; p < dg.NumPredicates(); ++p) {
    const std::string& name = dg.table().Name(p);
    const auto it = rel_by_name.find(name);
    if (it == rel_by_name.end()) {
      anchored[p] = 1;  // attribute/ML precondition: reads data directly
    } else {
      anchored[p] = g.RelationPairs(it->second).empty() ? 0 : 1;
    }
  }
  return anchored;
}

void DependencyGraph::Build(const std::vector<gar::GARule>& rules,
                            PredicateTable* table) {
  table_ = table;
  arcs_.clear();

  std::unordered_set<std::uint64_t> seen;  // (q, p, rule) dedup
  auto add_arc = [&](std::uint32_t q, std::uint32_t p, std::uint32_t i) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(q) << 40) |
        (static_cast<std::uint64_t>(p) << 20) | i;
    if (seen.insert(key).second) arcs_.push_back({q, p, i});
  };

  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    const gar::GARule& r = rules[i];
    if (!r.consequent) continue;  // query rule: derives nothing
    const std::uint32_t p = table->Intern(PredicateIdOf(*r.consequent, r));
    // Pattern edges: each distinct edge label is an edge predicate.
    for (std::uint32_t e = 0; e < r.pattern.num_edges(); ++e) {
      add_arc(table->Intern(LabelNameOf(r.pattern.edge_labels[e], r)), p, i);
    }
    // Antecedent predicates (const/var/exists/edge/ml).
    for (const auto& pred : r.antecedent) {
      add_arc(table->Intern(PredicateIdOf(pred, r)), p, i);
    }
  }

  // ---- CSR-ify, forward (by src) and reverse (by dst) ----
  const std::size_t n = table->size();
  std::vector<std::uint32_t> order(arcs_.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;

  out_off_.assign(n + 1, 0);
  for (const auto& a : arcs_) ++out_off_[a.src + 1];
  for (std::size_t i = 0; i < n; ++i) out_off_[i + 1] += out_off_[i];
  out_arcs_.resize(arcs_.size());
  {
    std::vector<std::uint32_t> cur(out_off_.begin(), out_off_.end());
    std::sort(order.begin(), order.end(), [&](std::uint32_t x,
                                              std::uint32_t y) {
      return std::tie(arcs_[x].src, arcs_[x].dst) <
             std::tie(arcs_[y].src, arcs_[y].dst);
    });
    for (const std::uint32_t id : order) out_arcs_[cur[arcs_[id].src]++] = id;
  }

  in_off_.assign(n + 1, 0);
  for (const auto& a : arcs_) ++in_off_[a.dst + 1];
  for (std::size_t i = 0; i < n; ++i) in_off_[i + 1] += in_off_[i];
  in_arcs_.resize(arcs_.size());
  {
    std::vector<std::uint32_t> cur(in_off_.begin(), in_off_.end());
    std::sort(order.begin(), order.end(), [&](std::uint32_t x,
                                              std::uint32_t y) {
      return std::tie(arcs_[x].dst, arcs_[x].src) <
             std::tie(arcs_[y].dst, arcs_[y].src);
    });
    for (const std::uint32_t id : order) in_arcs_[cur[arcs_[id].dst]++] = id;
  }
}

std::vector<std::uint32_t> DependencyGraph::DistancesTo(
    std::uint32_t goal_pred, const std::vector<double>* w0) const {
  const std::size_t n = table_->size();
  if (goal_pred >= n) {
    throw std::invalid_argument("DistancesTo: unknown goal predicate");
  }
  if (w0 == nullptr) {
    // Unit-cost backward BFS from the goal over reverse arcs.
    std::vector<std::uint32_t> d(n, kUnreachable);
    std::vector<std::uint32_t> queue{goal_pred};
    d[goal_pred] = 0;
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const std::uint32_t p = queue[head];
      for (std::uint32_t e = in_off_[p]; e < in_off_[p + 1]; ++e) {
        const std::uint32_t q = arcs_[in_arcs_[e]].src;
        if (d[q] == kUnreachable) {
          d[q] = d[p] + 1;
          queue.push_back(q);
        }
      }
    }
    return d;
  }
  // Weighted: Dijkstra with arc weight w0(rule of the arc); parallel arcs
  // (same q -> p via different rules) relax independently, so the cheapest
  // rule wins. Sums are rounded to the nearest uint32 on output.
  std::vector<double> dist(n, HUGE_VAL);
  using Entry = std::pair<double, std::uint32_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pq;
  dist[goal_pred] = 0.0;
  pq.emplace(0.0, goal_pred);
  while (!pq.empty()) {
    const auto [du, p] = pq.top();
    pq.pop();
    if (du > dist[p]) continue;
    for (std::uint32_t e = in_off_[p]; e < in_off_[p + 1]; ++e) {
      const Arc& a = arcs_[in_arcs_[e]];
      const double w = a.rule < w0->size() ? (*w0)[a.rule] : 1.0;
      if (dist[a.src] > du + w) {
        dist[a.src] = du + w;
        pq.emplace(dist[a.src], a.src);
      }
    }
  }
  std::vector<std::uint32_t> d(n, kUnreachable);
  for (std::size_t i = 0; i < n; ++i) {
    if (dist[i] != HUGE_VAL) {
      d[i] = static_cast<std::uint32_t>(std::lround(dist[i]));
    }
  }
  return d;
}

std::uint32_t DependencyGraph::Distance(std::uint32_t pred,
                                        std::uint32_t goal) const {
  return DistancesTo(goal)[pred];
}

void DependencyGraph::EnsureSccCache() const {
  if (scc_cache_) return;
  // Tarjan, recursive (predicate counts are small: hundreds, not millions).
  const std::size_t n = table_->size();
  std::vector<std::int32_t> index(n, -1), low(n, 0);
  std::vector<char> on_stack(n, 0);
  std::vector<std::uint32_t> stack;
  std::vector<Scc> out;
  std::int32_t next_index = 0;

  const std::function<void(std::uint32_t)> visit = [&](std::uint32_t v) {
    index[v] = low[v] = next_index++;
    stack.push_back(v);
    on_stack[v] = 1;
    for (std::uint32_t e = out_off_[v]; e < out_off_[v + 1]; ++e) {
      const std::uint32_t w = arcs_[out_arcs_[e]].dst;
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (on_stack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] == index[v]) {
      Scc scc;
      for (;;) {
        const std::uint32_t w = stack.back();
        stack.pop_back();
        on_stack[w] = 0;
        scc.members.push_back(w);
        if (w == v) break;
      }
      std::sort(scc.members.begin(), scc.members.end());
      // size > 1, or a self-loop (a rule deriving p from p).
      scc.recursive = scc.members.size() > 1;
      if (!scc.recursive) {
        const std::uint32_t v0 = scc.members[0];
        for (std::uint32_t e = out_off_[v0]; e < out_off_[v0 + 1]; ++e) {
          if (arcs_[out_arcs_[e]].dst == v0) {
            scc.recursive = true;
            break;
          }
        }
      }
      out.push_back(std::move(scc));
    }
  };
  for (std::uint32_t v = 0; v < n; ++v) {
    if (index[v] < 0) visit(v);
  }
  recursive_family_of_.assign(n, -1);
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!out[i].recursive) continue;
    for (const std::uint32_t m : out[i].members) {
      recursive_family_of_[m] = static_cast<int>(i);
    }
  }
  scc_cache_ = std::move(out);
}

std::vector<DependencyGraph::Scc>
DependencyGraph::StronglyConnectedComponents() const {
  EnsureSccCache();
  return *scc_cache_;
}

int DependencyGraph::RecursiveFamilyOf(std::uint32_t pred) const {
  EnsureSccCache();
  if (pred >= recursive_family_of_.size()) return -1;
  return recursive_family_of_[pred];
}

std::vector<DependencyGraph::PathParent> DependencyGraph::ShortestPathTo(
    std::uint32_t goal_pred) const {
  const std::size_t n = table_->size();
  if (goal_pred >= n) {
    throw std::invalid_argument("ShortestPathTo: unknown goal predicate");
  }
  // Same unit-cost BFS as DistancesTo, tracking the tree parent (first
  // discovery = shortest; multiple arcs (q,p) collapse to the first rule).
  std::vector<PathParent> parent(n);
  std::vector<char> seen(n, 0);
  std::vector<std::uint32_t> queue{goal_pred};
  seen[goal_pred] = 1;
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::uint32_t p = queue[head];
    for (std::uint32_t e = in_off_[p]; e < in_off_[p + 1]; ++e) {
      const Arc& a = arcs_[in_arcs_[e]];
      if (seen[a.src]) continue;
      seen[a.src] = 1;
      parent[a.src] = PathParent{p, a.rule};
      queue.push_back(a.src);
    }
  }
  return parent;
}

std::string DependencyGraph::RenderPath(
    std::uint32_t from_pred, std::uint32_t goal_pred,
    const std::vector<std::string>* rule_ids) const {
  const std::size_t n = table_->size();
  if (from_pred >= n || goal_pred >= n) return "";
  if (from_pred == goal_pred) return table_->Name(from_pred);
  const auto parent = ShortestPathTo(goal_pred);
  std::string out;
  std::vector<char> visited(n, 0);  // defensive; a BFS tree cannot cycle
  std::uint32_t cur = from_pred;
  while (true) {
    if (visited[cur]) return "";
    visited[cur] = 1;
    out += table_->Name(cur);
    if (cur == goal_pred) return out;
    const PathParent& pp = parent[cur];
    if (pp.parent == kInvalidPredicate) return "";
    out += " --[";
    out += (rule_ids != nullptr && pp.via_rule < rule_ids->size())
               ? (*rule_ids)[pp.via_rule]
               : "r" + std::to_string(pp.via_rule);
    out += "]--> ";
    cur = pp.parent;
  }
}

std::uint32_t DependencyGraph::NumPredicates() const {
  return static_cast<std::uint32_t>(table_->size());
}

void DependencyGraph::WriteDot(
    const std::filesystem::path& out,
    const std::vector<std::uint32_t>* distances) const {
  std::ofstream f(out);
  if (!f) throw std::runtime_error("WriteDot: cannot open " + out.string());
  f << "digraph DependencyGraph {\n  rankdir=LR;\n";
  const std::size_t n = table_->size();
  for (std::size_t i = 0; i < n; ++i) {
    f << "  n" << i << " [label=" << Quote(table_->Name(i));
    if (distances != nullptr) {
      const std::uint32_t d = (*distances)[i];
      f << (d == kUnreachable ? " (d=inf)"
                              : " (d=" + std::to_string(d) + ")");
    }
    f << "];\n";
  }
  const auto sccs = StronglyConnectedComponents();
  std::uint32_t cluster = 0;
  for (const auto& scc : sccs) {
    if (!scc.recursive) continue;
    f << "  subgraph cluster_" << cluster++ << " {\n    label="
      << Quote("SCC (recursive)") << ";\n";
    for (const std::uint32_t m : scc.members) f << "    n" << m << ";\n";
    f << "  }\n";
  }
  for (const auto& a : arcs_) {
    f << "  n" << a.src << " -> n" << a.dst << " [label="
      << Quote("r" + std::to_string(a.rule)) << "];\n";
  }
  f << "}\n";
}

void DependencyGraph::WriteYaml(const std::filesystem::path& out) const {
  std::ofstream f(out);
  if (!f) throw std::runtime_error("WriteYaml: cannot open " + out.string());
  const std::size_t n = table_->size();
  f << "num_predicates: " << n << "\n";
  f << "predicates:\n";
  for (std::size_t i = 0; i < n; ++i) {
    f << "  - {id: " << i << ", name: " << Quote(table_->Name(i)) << "}\n";
  }
  f << "num_edges: " << arcs_.size() << "\n";
  f << "edges:\n";
  for (const auto& a : arcs_) {
    f << "  - {src: " << a.src << ", dst: " << a.dst << ", rule: " << a.rule
      << "}\n";
  }
  const auto sccs = StronglyConnectedComponents();
  f << "sccs:\n";
  for (std::size_t i = 0; i < sccs.size(); ++i) {
    f << "  - {id: " << i << ", recursive: "
      << (sccs[i].recursive ? "true" : "false") << ", members: [";
    for (std::size_t j = 0; j < sccs[i].members.size(); ++j) {
      if (j) f << ", ";
      f << sccs[i].members[j];
    }
    f << "]}\n";
  }
}

std::vector<std::size_t> GoalConeRuleIndices(
    const std::vector<gar::GARule>& rules, const std::string& goal_predicate) {
  PredicateTable table;
  DependencyGraph dg;
  dg.Build(rules, &table);
  const std::uint32_t goal = table.Find(goal_predicate);
  if (goal == kInvalidPredicate) return {};  // no rule derives this predicate
  const std::vector<std::uint32_t> distances = dg.DistancesTo(goal);
  std::vector<std::size_t> indices;
  indices.reserve(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    if (!rules[i].consequent) continue;  // pattern queries derive nothing
    const std::uint32_t p =
        table.Find(PredicateIdOf(*rules[i].consequent, rules[i]));
    if (p != kInvalidPredicate && distances[p] != kUnreachable) {
      indices.push_back(i);
    }
  }
  return indices;
}

}  // namespace analysis
}  // namespace gar
