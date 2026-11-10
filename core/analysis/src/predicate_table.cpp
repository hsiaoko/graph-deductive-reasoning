#include "gar/analysis/predicate_table.h"

#include <algorithm>

namespace gar {
namespace analysis {

std::uint32_t PredicateTable::Intern(const std::string& name) {
  const auto it = ids_.find(name);
  if (it != ids_.end()) return it->second;
  const auto id = static_cast<std::uint32_t>(names_.size());
  ids_.emplace(name, id);
  names_.push_back(name);
  return id;
}

std::uint32_t PredicateTable::Find(const std::string& name) const {
  const auto it = ids_.find(name);
  return it == ids_.end() ? kInvalidPredicate : it->second;
}

const std::string& PredicateTable::Name(std::uint32_t id) const {
  return names_[id];
}

namespace {

// Strips the "^-1" inverse-slot suffix (TripleGraph::SlotName's spelling).
std::string BaseRelationName(std::string name) {
  constexpr char kInv[] = "^-1";
  if (name.size() > 3 && name.compare(name.size() - 3, 3, kInv) == 0) {
    name.resize(name.size() - 3);
  }
  return name;
}

}  // namespace

std::string LabelNameOf(gar::LabelID label, const gar::GARule& rule) {
  if (label == gar::kWildcardLabel) return "<wildcard>";
  // Reverse lookup; the labels dictionary mixes node-label and edge-label
  // id spaces, so several names may map to one id — take the
  // lexicographically smallest for determinism.
  std::string best;
  for (const auto& [name, id] : rule.labels) {
    if (id == label && (best.empty() || name < best)) best = name;
  }
  if (best.empty()) return "label:" + std::to_string(label);
  return BaseRelationName(best);
}

std::string PredicateIdOf(const gar::Predicate& pred,
                          const gar::GARule& rule) {
  switch (pred.kind) {
    case gar::PredKind::kConst:
    case gar::PredKind::kVar:
    case gar::PredKind::kExists:
      return pred.attr;
    case gar::PredKind::kEdge:
      if (!pred.label_name.empty()) return BaseRelationName(pred.label_name);
      return LabelNameOf(pred.label, rule);
    case gar::PredKind::kMl:
      return pred.model;
  }
  return "<unknown>";
}

}  // namespace analysis
}  // namespace gar
