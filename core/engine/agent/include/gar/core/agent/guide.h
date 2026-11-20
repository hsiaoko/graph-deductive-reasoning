#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "gar/core/agent/motif.h"
#include "gar/core/graph/graph.h"
#include "gar/core/rule.h"

namespace gar::core::engine::agent {

enum class GuideValueType {
  kNumber,
  kString,
  kBoolean,
};

using GuideValue = std::variant<std::monostate, double, std::string, bool>;

struct GuideContext {
  const ScoredMotif& action;
  const GARule& rule;
  const gar::core::graph::Graph& graph;
  std::uint32_t head_distance;
};

using GuideFeature = std::function<GuideValue(const GuideContext&)>;

struct GuideFeatureDescriptor {
  GuideValueType type;
  GuideFeature evaluate;
};

class GuideFeatureCatalog {
 public:
  GuideFeatureCatalog();

  void Register(std::string name, GuideValueType type, GuideFeature feature);
  [[nodiscard]] const GuideFeatureDescriptor* Find(const std::string& name) const;

 private:
  mutable std::unordered_map<std::string, GuideFeatureDescriptor> features_;
};

struct GuideOperand {
  bool is_feature = false;
  std::string feature;
  GuideValue constant;
};

struct GuideCondition {
  GuideOperand lhs;
  GuideOperand rhs;
  std::string op;
};

struct GuidePriorityRule {
  std::string rule;
  std::vector<GuideCondition> when;
  double weight = 0.0;
};

class CompiledGuide {
 public:
  // Returns the normalized priority in [-1,1]. Unknown values never fire.
  double Priority(const std::string& rule_id, const GuideContext& ctx) const;
  [[nodiscard]] bool empty() const { return rules_.empty(); }
  [[nodiscard]] std::size_t rejected() const { return rejected_; }

 private:
  std::vector<GuidePriorityRule> rules_;
  const GuideFeatureCatalog* catalog_ = nullptr;
  std::size_t rejected_ = 0;

  friend CompiledGuide CompileGuide(
      const std::string&, const GuideFeatureCatalog&,
      const std::unordered_set<std::string>&, std::size_t, std::size_t);
};

CompiledGuide CompileGuide(
    const std::string& json_text, const GuideFeatureCatalog& catalog,
    const std::unordered_set<std::string>& valid_rules,
    std::size_t max_rules = 64, std::size_t max_conditions = 8);

}  // namespace gar::core::engine::agent
