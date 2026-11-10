#include "gar/rule_loader.h"

#include <stdexcept>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "gar/csr.h"

namespace gar {

namespace {

[[noreturn]] void Fail(const std::string& where, const std::string& what) {
  throw std::runtime_error("rule yaml [" + where + "]: " + what);
}

using LabelDict = std::unordered_map<std::string, LabelID>;

LabelID LabelIdOf(const LabelDict& dict, const std::string& name,
                  const char* where) {
  auto it = dict.find(name);
  if (it == dict.end()) {
    throw std::runtime_error(std::string("rule yaml [") + where +
                             "]: unknown label '" + name + "'");
  }
  return it->second;
}

// Reads a vertex reference: either a bare id (0) or {var: 0}.
VertexID ReadNodeRef(const YAML::Node& node, const char* where) {
  if (node.IsScalar()) return node.as<VertexID>();
  if (node["var"]) return node["var"].as<VertexID>();
  if (node["id"]) return node["id"].as<VertexID>();
  Fail(where, "node reference must be an id or {var: id}");
}

AttrValue ParseValue(const YAML::Node& node) {
  if (node.IsScalar()) {
    bool b;
    if (YAML::convert<bool>::decode(node, b)) return AttrValue{b};
    try {
      return AttrValue{node.as<std::int64_t>()};
    } catch (const YAML::Exception&) {
    }
    try {
      return AttrValue{node.as<double>()};
    } catch (const YAML::Exception&) {
    }
    return AttrValue{node.as<std::string>()};
  }
  Fail("value", "attribute value must be a scalar");
}

// Tolerant predicate parser: an explicit `kind` wins; otherwise the kind is
// inferred from the keys present (GraphRAG-style duck typing):
//   model            -> ml
//   src+dst (+label) -> edge
//   lhs+rhs          -> var
//   value            -> const
//   var+attr only    -> exists
Predicate ParsePredicate(const YAML::Node& node, const LabelDict& dict,
                         const char* where) {
  Predicate p;

  PredKind kind;
  if (node["kind"]) {
    kind = ParsePredKind(node["kind"].as<std::string>());
  } else if (node["model"]) {
    kind = PredKind::kMl;
  } else if (node["src"] && node["dst"]) {
    kind = PredKind::kEdge;
  } else if (node["lhs"] && node["rhs"]) {
    kind = PredKind::kVar;
  } else if (node["value"]) {
    kind = PredKind::kConst;
  } else if (node["var"] && node["attr"]) {
    kind = PredKind::kExists;
  } else {
    Fail(where, "cannot infer predicate kind");
  }
  p.kind = kind;

  switch (kind) {
    case PredKind::kConst:
    case PredKind::kExists: {
      if (!node["var"] || !node["attr"]) {
        Fail(where, "const/exists predicate requires 'var' and 'attr'");
      }
      p.node = ReadNodeRef(node["var"], where);
      p.attr = node["attr"].as<std::string>();
      if (kind == PredKind::kConst) {
        p.value = ParseValue(node["value"]);
        p.op = ParseCmpOp(node["op"] ? node["op"].as<std::string>() : "==");
      }
      break;
    }
    case PredKind::kVar: {
      p.node = ReadNodeRef(node["lhs"], where);
      p.attr = node["lhs"]["attr"].as<std::string>();
      p.rhs_node = ReadNodeRef(node["rhs"], where);
      p.rhs_attr = node["rhs"]["attr"].as<std::string>();
      p.op = ParseCmpOp(node["op"] ? node["op"].as<std::string>() : "==");
      break;
    }
    case PredKind::kEdge: {
      p.src = ReadNodeRef(node["src"], where);
      p.dst = ReadNodeRef(node["dst"], where);
      if (node["label"]) {
        if (node["label"].IsScalar()) {
          try {
            p.label = node["label"].as<LabelID>();
          } catch (const YAML::Exception&) {
            p.label_name = node["label"].as<std::string>();
            if (!dict.empty()) p.label = LabelIdOf(dict, p.label_name, where);
          }
        }
      }
      if (p.label_name.empty() && p.label != kWildcardLabel && !dict.empty()) {
        // keep symbolic name when it was given as a string
      }
      break;
    }
    case PredKind::kMl: {
      p.model = node["model"].as<std::string>();
      if (!node["vars"] || node["vars"].size() != 2) {
        Fail(where, "ml predicate requires exactly 2 'vars'");
      }
      for (const auto& v : node["vars"]) {
        p.ml_vars.push_back(ReadNodeRef(v, where));
      }
      if (node["label"]) {
        p.label_name = node["label"].as<std::string>();
        if (!dict.empty()) p.label = LabelIdOf(dict, p.label_name, where);
      }
      p.threshold = node["threshold"] ? node["threshold"].as<double>() : 0.0;
      break;
    }
  }
  return p;
}

// Builds the flat edge arrays from inline pattern edges (in the YAML edge
// order, which is stable for a given rule.yaml).
void BuildFlatEdges(
    const std::vector<std::tuple<VertexID, VertexID, LabelID, bool>>& edges,
    std::size_t num_vertices, GARule& rule) {
  rule.n_vertices = static_cast<std::uint32_t>(num_vertices);
  rule.edge_src.clear();
  rule.edge_dst.clear();
  rule.edge_labels.clear();
  rule.edge_src.reserve(edges.size());
  rule.edge_dst.reserve(edges.size());
  rule.edge_labels.reserve(edges.size());
  for (const auto& [src, dst, l, has_l] : edges) {
    if (src >= num_vertices || dst >= num_vertices) {
      throw std::runtime_error("rule yaml [pattern.edges]: endpoint out of "
                               "range");
    }
    rule.edge_src.push_back(static_cast<std::int32_t>(src));
    rule.edge_dst.push_back(static_cast<std::int32_t>(dst));
    rule.edge_labels.push_back(has_l
                                   ? static_cast<std::int32_t>(l)
                                   : static_cast<std::int32_t>(kWildcardLabel));
  }
}

GARule ParseRuleNode(const YAML::Node& r) {
  GARule rule;

  LabelDict dict;
  if (r["labels"]) {
    for (const auto& kv : r["labels"]) {
      dict.emplace(kv.first.as<std::string>(), kv.second.as<LabelID>());
    }
  }
  rule.labels = dict;

  if (r["id"]) rule.id = r["id"].as<std::string>();
  if (r["name"]) rule.name = r["name"].as<std::string>();
  if (r["description"]) rule.description = r["description"].as<std::string>();

  // ---- pattern ----
  std::vector<std::tuple<VertexID, VertexID, LabelID, bool>> edges;
  std::size_t num_vertices = 0;
  const YAML::Node pat = r["pattern"];
  if (pat) {
    if (pat["nodes"]) {
      for (const auto& n : pat["nodes"]) {
        num_vertices =
            std::max(num_vertices,
                     static_cast<std::size_t>(n["id"].as<VertexID>()) + 1);
      }
      rule.node_labels.assign(num_vertices, kWildcardLabel);
      rule.node_label_names.assign(num_vertices, "");
      for (const auto& n : pat["nodes"]) {
        const auto id = n["id"].as<VertexID>();
        if (n["label"]) {
          rule.node_label_names[id] = n["label"].as<std::string>();
          rule.node_labels[id] =
              dict.empty() ? kWildcardLabel
                           : LabelIdOf(dict, rule.node_label_names[id],
                                       "pattern.nodes");
        }
      }
    }
    if (pat["edges"]) {
      for (const auto& e : pat["edges"]) {
        const VertexID src = ReadNodeRef(e["src"], "pattern.edges");
        const VertexID dst = ReadNodeRef(e["dst"], "pattern.edges");
        num_vertices = std::max(num_vertices,
                                static_cast<std::size_t>(std::max(src, dst)) +
                                    1);
        LabelID l = kWildcardLabel;
        bool has_l = false;
        if (e["label"]) {
          has_l = true;
          try {
            l = e["label"].as<LabelID>();
          } catch (const YAML::Exception&) {
            l = dict.empty()
                    ? kWildcardLabel
                    : LabelIdOf(dict, e["label"].as<std::string>(),
                                "pattern.edges");
          }
        }
        edges.emplace_back(src, dst, l, has_l);
      }
    }
    if (pat["pivot"]) {
      for (const auto& v : pat["pivot"]) {
        rule.pivot.push_back(v.as<VertexID>());
      }
    }
    if (pat["num_vertices"]) {
      num_vertices = std::max(num_vertices,
                              pat["num_vertices"].as<std::size_t>());
    }
  }
  if (!edges.empty() || num_vertices > 0) {
    BuildFlatEdges(edges, num_vertices, rule);
  }
  if (rule.node_labels.empty() && rule.n_vertices > 0) {
    rule.node_labels.assign(rule.n_vertices, kWildcardLabel);
    rule.node_label_names.assign(rule.n_vertices, "");
  }

  // ---- predicates / consequent ----
  if (r["predicates"]) {
    for (const auto& item : r["predicates"]) {
      rule.antecedent.push_back(ParsePredicate(item, dict, "predicates"));
    }
  }
  if (r["consequent"]) {
    rule.consequent = ParsePredicate(r["consequent"], dict, "consequent");
  }

  // ---- quality ----
  if (r["quality"]) {
    const auto& q = r["quality"];
    if (q["support"]) rule.quality.support = q["support"].as<std::uint64_t>();
    if (q["confidence"]) rule.quality.confidence = q["confidence"].as<double>();
    if (q["dataset"]) rule.quality.dataset = q["dataset"].as<std::string>();
    if (q["miner"]) rule.quality.miner = q["miner"].as<std::string>();
  }

  // Default pivot: variables of the consequent.
  if (rule.pivot.empty() && rule.consequent) {
    const auto& p0 = *rule.consequent;
    switch (p0.kind) {
      case PredKind::kConst:
      case PredKind::kExists:
        rule.pivot = {p0.node};
        break;
      case PredKind::kVar:
        rule.pivot = {p0.node, p0.rhs_node};
        break;
      case PredKind::kEdge:
        rule.pivot = {p0.src, p0.dst};
        break;
      case PredKind::kMl:
        rule.pivot = p0.ml_vars;
        break;
    }
  }
  return rule;
}

}  // namespace

GARule RuleLoader::LoadYaml(const std::filesystem::path& yaml_path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(yaml_path.string());
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("cannot parse " + yaml_path.string() + ": " +
                             e.what());
  }
  GARule rule = ParseRuleNode(root["rule"] ? root["rule"] : root);
  if (rule.n_vertices > 0) rule.Validate();
  return rule;
}

GARule RuleLoader::Load(const std::filesystem::path& rule_dir) {
  GARule rule = LoadYaml(rule_dir / "rule.yaml");

  // CSR binaries override inline edges when present; convert to flat.
  if (std::filesystem::exists(rule_dir / "csr_offsets.bin")) {
    const CSRTopology topo = CSRTopology::LoadBin(rule_dir);
    rule.n_vertices = static_cast<std::uint32_t>(topo.num_vertices());
    rule.edge_src.clear();
    rule.edge_dst.clear();
    rule.edge_labels.clear();
    rule.edge_src.reserve(topo.num_edges());
    rule.edge_dst.reserve(topo.num_edges());
    rule.edge_labels.reserve(topo.num_edges());
    for (std::size_t s = 0; s < topo.num_vertices(); ++s) {
      for (EdgeIndex e = topo.offsets[s]; e < topo.offsets[s + 1]; ++e) {
        rule.edge_src.push_back(static_cast<std::int32_t>(s));
        rule.edge_dst.push_back(static_cast<std::int32_t>(topo.indices[e]));
        rule.edge_labels.push_back(
            topo.has_edge_labels()
                ? static_cast<std::int32_t>(topo.edge_labels[e])
                : static_cast<std::int32_t>(kWildcardLabel));
      }
    }
    if (rule.node_labels.empty()) {
      rule.node_labels.assign(rule.n_vertices, kWildcardLabel);
      rule.node_label_names.assign(rule.n_vertices, "");
    }
  }
  rule.Validate();
  return rule;
}

std::vector<GARule> RuleLoader::LoadRuleSet(
    const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("cannot parse " + path.string() + ": " +
                             e.what());
  }
  std::vector<GARule> rules;
  if (root["rules"]) {
    for (const auto& r : root["rules"]) {
      GARule rule = ParseRuleNode(r);
      rule.Validate();
      rules.push_back(std::move(rule));
    }
  } else {
    GARule rule = ParseRuleNode(root["rule"] ? root["rule"] : root);
    rule.Validate();
    rules.push_back(std::move(rule));
  }
  return rules;
}

}  // namespace gar
