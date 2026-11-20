#include "gar/core/agent/guide.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gar::core::engine::agent {

namespace {

using Json = nlohmann::json;

GuideValueType TypeOf(const GuideValue& value) {
  if (std::holds_alternative<double>(value)) return GuideValueType::kNumber;
  if (std::holds_alternative<std::string>(value)) return GuideValueType::kString;
  if (std::holds_alternative<bool>(value)) return GuideValueType::kBoolean;
  throw std::invalid_argument("unknown guide value type");
}

bool Allowed(GuideValueType type, const std::string& op) {
  static const std::unordered_set<std::string> kOps{
      "=", "!=", "<", "<=", ">", ">="};
  if (!kOps.contains(op)) return false;
  return type == GuideValueType::kNumber || op == "=" || op == "!=";
}

GuideValue EvaluateOperand(const GuideOperand& operand,
                           const GuideFeatureCatalog& catalog,
                           const GuideContext& ctx) {
  if (!operand.is_feature) return operand.constant;
  const auto* feature = catalog.Find(operand.feature);
  return feature == nullptr ? GuideValue{} : feature->evaluate(ctx);
}

bool Compare(const GuideValue& lhs, const GuideValue& rhs,
             const std::string& op) {
  if (std::holds_alternative<std::monostate>(lhs) ||
      std::holds_alternative<std::monostate>(rhs) ||
      TypeOf(lhs) != TypeOf(rhs)) {
    return false;
  }
  if (const auto* a = std::get_if<double>(&lhs)) {
    const double b = std::get<double>(rhs);
    if (op == "=") return *a == b;
    if (op == "!=") return *a != b;
    if (op == "<") return *a < b;
    if (op == "<=") return *a <= b;
    if (op == ">") return *a > b;
    if (op == ">=") return *a >= b;
    return false;
  }
  if (const auto* a = std::get_if<std::string>(&lhs)) {
    const auto& b = std::get<std::string>(rhs);
    if (op == "=") return *a == b;
    if (op == "!=") return *a != b;
    return false;
  }
  const bool a = std::get<bool>(lhs);
  const bool b = std::get<bool>(rhs);
  if (op == "=") return a == b;
  if (op == "!=") return a != b;
  return false;
}

GuideValue Literal(const Json& value) {
  if (value.is_number()) return value.get<double>();
  if (value.is_string()) return value.get<std::string>();
  if (value.is_boolean()) return value.get<bool>();
  return {};
}

bool ParseBinding(const std::string& name, std::uint32_t* variable,
                  std::string* kind) {
  if (!name.starts_with("binding.")) return false;
  const auto dot = name.find('.', 8);
  if (dot == std::string::npos || dot == 8) return false;
  const std::string number = name.substr(8, dot - 8);
  if (number.empty() ||
      !std::all_of(number.begin(), number.end(),
                   [](unsigned char c) { return std::isdigit(c); })) {
    return false;
  }
  *variable = static_cast<std::uint32_t>(std::stoul(number));
  *kind = name.substr(dot + 1);
  return *kind == "id" || *kind == "label" || *kind == "kind";
}

bool ParseOperand(const Json& value, const GuideFeatureCatalog& catalog,
                  bool require_feature, GuideOperand* out) {
  if (value.is_object() && value.contains("feature")) {
    if (!value["feature"].is_string()) return false;
    out->is_feature = true;
    out->feature = value["feature"].get<std::string>();
    return catalog.Find(out->feature) != nullptr;
  }
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    if (catalog.Find(text) != nullptr) {
      out->is_feature = true;
      out->feature = text;
      return true;
    }
    if (require_feature) return false;
  }
  if (require_feature) return false;
  out->constant = Literal(value);
  return !std::holds_alternative<std::monostate>(out->constant);
}

}  // namespace

GuideFeatureCatalog::GuideFeatureCatalog() {
  auto number = [this](std::string name, GuideFeature feature) {
    Register(std::move(name), GuideValueType::kNumber, std::move(feature));
  };
  number("rule.confidence", [](const GuideContext& c) {
    return GuideValue{c.rule.quality.confidence};
  });
  number("rule.support", [](const GuideContext& c) {
    return GuideValue{static_cast<double>(c.rule.quality.support)};
  });
  number("rule.body_edges", [](const GuideContext& c) {
    return GuideValue{static_cast<double>(c.rule.num_edges())};
  });
  number("goal.distance", [](const GuideContext& c) {
    return GuideValue{static_cast<double>(c.head_distance)};
  });
  number("score.total", [](const GuideContext& c) {
    return GuideValue{c.action.total};
  });
  number("score.price", [](const GuideContext& c) {
    return GuideValue{c.action.price};
  });
  number("score.residual_cost", [](const GuideContext& c) {
    return GuideValue{c.action.dcost};
  });
  number("evidence.witnessed", [](const GuideContext& c) {
    return GuideValue{static_cast<double>(c.action.premises_witnessed)};
  });
  number("evidence.total", [](const GuideContext& c) {
    return GuideValue{static_cast<double>(c.action.premises_total)};
  });
  number("evidence.ratio", [](const GuideContext& c) {
    return GuideValue{c.action.premises_total == 0
                          ? 1.0
                          : static_cast<double>(c.action.premises_witnessed) /
                                c.action.premises_total};
  });
}

void GuideFeatureCatalog::Register(std::string name, GuideValueType type,
                                   GuideFeature feature) {
  if (name.empty() || !feature) {
    throw std::invalid_argument("invalid guide feature registration");
  }
  features_[std::move(name)] = GuideFeatureDescriptor{type, std::move(feature)};
}

const GuideFeatureDescriptor* GuideFeatureCatalog::Find(
    const std::string& name) const {
  const auto it = features_.find(name);
  if (it != features_.end()) return &it->second;
  std::uint32_t variable = 0;
  std::string kind;
  if (!ParseBinding(name, &variable, &kind)) return nullptr;
  GuideFeature feature;
  GuideValueType type = GuideValueType::kNumber;
  if (kind == "id") {
    feature = [variable](const GuideContext& c) -> GuideValue {
      if (variable >= c.action.motif.embedding.size()) return {};
      return static_cast<double>(c.action.motif.embedding[variable]);
    };
  } else if (kind == "label") {
    feature = [variable](const GuideContext& c) -> GuideValue {
      if (variable >= c.action.motif.embedding.size() ||
          !c.graph.has_vertex_labels()) {
        return {};
      }
      return static_cast<double>(c.graph.VertexLabel(
          c.action.motif.embedding[variable]));
    };
  } else {
    type = GuideValueType::kString;
    feature = [variable](const GuideContext& c) -> GuideValue {
      if (variable >= c.action.motif.embedding.size() ||
          !c.graph.has_vertex_labels()) {
        return {};
      }
      return c.graph.LabelName(c.graph.VertexLabel(
          c.action.motif.embedding[variable]));
    };
  }
  const auto [inserted, _] = features_.emplace(
      name, GuideFeatureDescriptor{type, std::move(feature)});
  return &inserted->second;
}

double CompiledGuide::Priority(const std::string& rule_id,
                               const GuideContext& ctx) const {
  if (catalog_ == nullptr) return 0.0;
  double sum = 0.0;
  double norm = 0.0;
  for (const auto& rule : rules_) {
    norm += std::abs(rule.weight);
    if (rule.rule != rule_id && rule.rule != ctx.rule.name) continue;
    const bool fires = std::all_of(
        rule.when.begin(), rule.when.end(), [&](const GuideCondition& condition) {
          return Compare(EvaluateOperand(condition.lhs, *catalog_, ctx),
                         EvaluateOperand(condition.rhs, *catalog_, ctx),
                         condition.op);
        });
    if (fires) sum += rule.weight;
  }
  return sum / std::max(1.0, norm);
}

CompiledGuide CompileGuide(
    const std::string& json_text, const GuideFeatureCatalog& catalog,
    const std::unordered_set<std::string>& valid_rules, std::size_t max_rules,
    std::size_t max_conditions) {
  CompiledGuide result;
  result.catalog_ = &catalog;
  Json root;
  try {
    root = Json::parse(json_text);
  } catch (const std::exception&) {
    result.rejected_ = 1;
    return result;
  }
  if (!root.is_object() || !root.contains("rules") ||
      !root["rules"].is_array()) {
    result.rejected_ = 1;
    return result;
  }
  for (const auto& entry : root["rules"]) {
    if (result.rules_.size() >= max_rules) {
      ++result.rejected_;
      continue;
    }
    try {
      if (!entry.is_object() || !entry.contains("rule") ||
          !entry["rule"].is_string() || !entry.contains("weight") ||
          !entry["weight"].is_number()) {
        ++result.rejected_;
        continue;
      }
      GuidePriorityRule rule;
      rule.rule = entry["rule"].get<std::string>();
      if (!valid_rules.contains(rule.rule)) {
        ++result.rejected_;
        continue;
      }
      rule.weight = entry["weight"].get<double>();
      if (!std::isfinite(rule.weight) || std::abs(rule.weight) > 1.0) {
        ++result.rejected_;
        continue;
      }
      const Json conditions = entry.value("when", Json::array());
      if (!conditions.is_array() || conditions.size() > max_conditions) {
        ++result.rejected_;
        continue;
      }
      bool valid = true;
      for (const auto& condition : conditions) {
        if (!condition.is_array() || condition.size() != 3 ||
            !condition[1].is_string()) {
          valid = false;
          break;
        }
        GuideCondition compiled;
        compiled.op = condition[1].get<std::string>();
        if (!ParseOperand(condition[0], catalog, true, &compiled.lhs) ||
            !ParseOperand(condition[2], catalog, false, &compiled.rhs)) {
          valid = false;
          break;
        }
        const auto* lhs = catalog.Find(compiled.lhs.feature);
        if (lhs == nullptr || !Allowed(lhs->type, compiled.op)) {
          valid = false;
          break;
        }
        if (compiled.rhs.is_feature) {
          const auto* rhs = catalog.Find(compiled.rhs.feature);
          if (rhs == nullptr || rhs->type != lhs->type) {
            valid = false;
            break;
          }
        } else if (TypeOf(compiled.rhs.constant) != lhs->type) {
          valid = false;
          break;
        }
        rule.when.push_back(std::move(compiled));
      }
      if (!valid) {
        ++result.rejected_;
        continue;
      }
      result.rules_.push_back(std::move(rule));
    } catch (const std::exception&) {
      ++result.rejected_;
    }
  }
  return result;
}

}  // namespace gar::core::engine::agent
