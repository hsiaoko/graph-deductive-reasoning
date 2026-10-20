#pragma once

#include <filesystem>
#include <vector>

#include "gar/rule.h"

// RuleLoader: loads GAR rules from YAML. A rule is either a directory:
//
//   <dir>/rule.yaml            metadata, nodes, predicates, quality
//   <dir>/csr_offsets.bin      CSR row pointers (uint32)
//   <dir>/csr_indices.bin      CSR column indices (uint32)
//   <dir>/csr_edge_labels.bin  optional per-edge labels (uint32)
//
// or a self-contained YAML file with the pattern edges inline (no CSR
// binaries). A rule-set file holds many rules under a top-level `rules:`.
//
// YAML schema (see data/rules/example/rule.yaml):
//
//   rule:
//     id: "gar_0001"
//     name: "..."
//     labels:               # optional label name -> LabelID dictionary
//       Person: 1
//     pattern:
//       nodes:
//         - {id: 0, var: "x", label: "Person"}
//       edges:              # inline topology (alternative to CSR bins)
//         - {src: 0, dst: 1, label: "owns"}
//       pivot: [0]          # support-counting pivot variables
//     predicates:           # X; kind inferred from keys when omitted
//       - {var: 0, attr: "age", op: ">=", value: 18}                  # const
//       - {lhs: {var: 0, attr: "city"}, op: "=", rhs: {var: 2, attr: "name"}}  # var
//       - {src: 0, dst: 1, label: "owns"}                             # edge
//       - {var: 0, attr: "phone"}                                     # exists
//       - {model: "M_anom", vars: [0, 1], label: "anom", threshold: 0.8}  # ml
//     consequent:           # p0; single predicate; omit for a query ψ
//       {src: 0, dst: 2, label: "HighRisk"}
//     quality:
//       {support: 1234, confidence: 0.87, dataset: "...", miner: "..."}
namespace gar {

class RuleLoader {
 public:
  // Loads a rule directory (rule.yaml + CSR binaries referenced by it).
  static GARule Load(const std::filesystem::path& rule_dir);

  // Loads a single self-contained rule YAML file.
  static GARule LoadYaml(const std::filesystem::path& yaml_path);

  // Loads every rule in a rule-set file (top-level `rules:` sequence).
  // Also accepts a single-rule file (top-level `rule:`).
  static std::vector<GARule> LoadRuleSet(const std::filesystem::path& path);
};

}  // namespace gar
