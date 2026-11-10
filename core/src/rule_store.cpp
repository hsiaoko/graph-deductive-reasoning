#include "gar/rule_store.h"

#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "gar/csr.h"

namespace gar {

namespace {

// Rebuilds the CSR topology from the flat edge arrays (for the legacy
// csr_*.bin serialization).
CSRTopology ToCsr(const GARule& rule) {
  std::vector<EdgeIndex> offsets(rule.num_vertices() + 1, 0);
  for (auto s : rule.edge_src) ++offsets[static_cast<std::size_t>(s) + 1];
  for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
  std::vector<VertexID> indices(offsets.back());
  std::vector<LabelID> labels(offsets.back(), kWildcardLabel);
  std::vector<EdgeIndex> cursor(offsets.begin(), offsets.end());
  for (std::size_t e = 0; e < rule.edge_src.size(); ++e) {
    const auto pos = cursor[static_cast<std::size_t>(rule.edge_src[e])]++;
    indices[pos] = static_cast<VertexID>(rule.edge_dst[e]);
    labels[pos] = static_cast<LabelID>(rule.edge_labels[e]);
  }
  return CSRTopology(std::move(offsets), std::move(indices),
                     std::move(labels));
}

using RevLabels = std::unordered_map<LabelID, std::string>;

RevLabels ReverseLabels(const GARule& rule) {
  RevLabels rev;
  for (const auto& [name, id] : rule.labels) rev.emplace(id, name);
  return rev;
}

YAML::Node EmitPredicate(const Predicate& p, const RevLabels& rev) {
  YAML::Node n;
  n["kind"] = ToString(p.kind);
  switch (p.kind) {
    case PredKind::kConst:
      n["var"] = p.node;
      n["attr"] = p.attr;
      n["op"] = ToString(p.op);
      std::visit([&](const auto& x) { n["value"] = x; }, p.value);
      break;
    case PredKind::kVar:
      n["lhs"] = YAML::Node(YAML::NodeType::Map);
      n["lhs"]["var"] = p.node;
      n["lhs"]["attr"] = p.attr;
      n["op"] = ToString(p.op);
      n["rhs"] = YAML::Node(YAML::NodeType::Map);
      n["rhs"]["var"] = p.rhs_node;
      n["rhs"]["attr"] = p.rhs_attr;
      break;
    case PredKind::kEdge:
      n["src"] = p.src;
      n["dst"] = p.dst;
      if (!p.label_name.empty()) {
        n["label"] = p.label_name;
      } else if (p.label != kWildcardLabel) {
        const auto it = rev.find(p.label);
        n["label"] = it != rev.end() ? YAML::Node(it->second)
                                     : YAML::Node(p.label);
      }
      break;
    case PredKind::kExists:
      n["var"] = p.node;
      n["attr"] = p.attr;
      break;
    case PredKind::kMl:
      n["model"] = p.model;
      for (VertexID v : p.ml_vars) n["vars"].push_back(v);
      if (!p.label_name.empty()) n["label"] = p.label_name;
      n["threshold"] = p.threshold;
      break;
  }
  return n;
}

YAML::Node EmitRule(const GARule& rule, bool inline_edges) {
  const RevLabels rev = ReverseLabels(rule);

  YAML::Node r;
  if (!rule.id.empty()) r["id"] = rule.id;
  if (!rule.name.empty()) r["name"] = rule.name;
  if (!rule.description.empty()) r["description"] = rule.description;

  if (!rule.labels.empty()) {
    YAML::Node labels;
    for (const auto& [name, id] : rule.labels) labels[name] = id;
    r["labels"] = labels;
  }

  YAML::Node pat;
  for (std::size_t v = 0; v < rule.num_vertices(); ++v) {
    YAML::Node n;
    n["id"] = static_cast<VertexID>(v);
    if (v < rule.node_label_names.size() && !rule.node_label_names[v].empty()) {
      n["label"] = rule.node_label_names[v];
    } else if (v < rule.node_labels.size() &&
               rule.node_labels[v] != kWildcardLabel) {
      n["label"] = rule.node_labels[v];
    }
    pat["nodes"].push_back(n);
  }
  if (inline_edges) {
    for (std::size_t e = 0; e < rule.edge_src.size(); ++e) {
      YAML::Node edge;
      edge["src"] = static_cast<VertexID>(rule.edge_src[e]);
      edge["dst"] = static_cast<VertexID>(rule.edge_dst[e]);
      if (rule.edge_labels[e] != static_cast<std::int32_t>(kWildcardLabel)) {
        const auto lid = static_cast<LabelID>(rule.edge_labels[e]);
        const auto it = rev.find(lid);
        edge["label"] = it != rev.end() ? YAML::Node(it->second)
                                        : YAML::Node(lid);
      }
      pat["edges"].push_back(edge);
    }
  }
  for (VertexID v : rule.pivot) pat["pivot"].push_back(v);
  r["pattern"] = pat;

  for (const auto& p : rule.antecedent) {
    r["predicates"].push_back(EmitPredicate(p, rev));
  }
  if (rule.consequent) {
    r["consequent"] = EmitPredicate(*rule.consequent, rev);
  }

  if (rule.quality.support > 0 || rule.quality.confidence > 0) {
    YAML::Node q;
    q["support"] = rule.quality.support;
    q["confidence"] = rule.quality.confidence;
    if (!rule.quality.dataset.empty()) q["dataset"] = rule.quality.dataset;
    if (!rule.quality.miner.empty()) q["miner"] = rule.quality.miner;
    r["quality"] = q;
  }
  return r;
}

}  // namespace

void RuleStore::SaveDir(const std::vector<GARule>& rules,
                        const std::filesystem::path& root_dir) {
  std::filesystem::create_directories(root_dir);
  std::size_t idx = 0;
  for (auto rule : rules) {
    if (rule.id.empty()) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "gar_%06zu", idx);
      rule.id = buf;
    }
    const auto dir = root_dir / rule.id;
    std::filesystem::create_directories(dir);

    // Topology goes to the CSR binaries; the YAML carries no inline edges.
    ToCsr(rule).SaveBin(dir);

    YAML::Node root;
    root["rule"] = EmitRule(rule, /*inline_edges=*/false);
    std::ofstream out(dir / "rule.yaml");
    if (!out) {
      throw std::runtime_error("RuleStore: cannot write " + dir.string() +
                               "/rule.yaml");
    }
    out << root << "\n";
    ++idx;
  }
}

void RuleStore::Save(const std::vector<GARule>& rules,
                     const std::filesystem::path& path) {
  YAML::Node root;
  for (const auto& rule : rules) {
    root["rules"].push_back(EmitRule(rule, /*inline_edges=*/true));
  }
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("RuleStore: cannot write " + path.string());
  }
  out << root << "\n";
}

}  // namespace gar
