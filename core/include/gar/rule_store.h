#pragma once

#include <filesystem>
#include <vector>

#include "gar/rule.h"

// RuleStore: serializes rules to YAML.
//
// Two layouts:
//  * SaveDir — one directory per rule (the canonical layout):
//      <root>/gar_000001/rule.yaml + csr_offsets.bin + csr_indices.bin
//      (+ csr_edge_labels.bin when the pattern has edge labels)
//    Inverse of RuleLoader::Load(dir).
//  * Save — a single YAML file with a top-level `rules:` sequence
//    (inverse of RuleLoader::LoadRuleSet).
namespace gar {

class RuleStore {
 public:
  // Writes each rule as <root>/<rule_id>/ (rule.yaml + CSR binaries).
  // Rules without an id are named gar_%06d in order.
  static void SaveDir(const std::vector<GARule>& rules,
                      const std::filesystem::path& root_dir);

  static void Save(const std::vector<GARule>& rules,
                   const std::filesystem::path& path);
};

}  // namespace gar
