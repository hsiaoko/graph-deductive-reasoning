#include "gar/core/graph/predicate_evaluator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gar::core::graph {

namespace {

bool EvalCmp(CmpOp op, double lhs, double rhs) {
  if (std::isnan(lhs) || std::isnan(rhs)) return false;
  switch (op) {
    case CmpOp::kEq: return lhs == rhs;
    case CmpOp::kNe: return lhs != rhs;
    case CmpOp::kLt: return lhs < rhs;
    case CmpOp::kLe: return lhs <= rhs;
    case CmpOp::kGt: return lhs > rhs;
    case CmpOp::kGe: return lhs >= rhs;
  }
  return false;
}

double AttrValueAsDouble(const AttrValue& v, bool* ok) {
  *ok = true;
  if (const auto* i = std::get_if<std::int64_t>(&v)) {
    return static_cast<double>(*i);
  }
  if (const auto* d = std::get_if<double>(&v)) return *d;
  if (const auto* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
  *ok = false;
  return 0.0;
}

bool HasEdge(const Graph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  return g.RelationPairSet(slot).count(PackPair(s, d)) != 0;
}

struct PendingMl {
  const MlModel* model = nullptr;
  std::vector<MlQuery> queries;
  std::vector<std::size_t> antecedent_indices;
  std::vector<double> scores;
};

}  // namespace

void MlModelRegistry::Register(std::string name, MlModel model) {
  if (name.empty() || !model) {
    throw std::invalid_argument("ML model name and callback must be nonempty");
  }
  const auto [it, inserted] = models_.emplace(std::move(name), std::move(model));
  if (!inserted) throw std::invalid_argument("duplicate ML model: " + it->first);
}

const MlModel* MlModelRegistry::Find(const std::string& name) const {
  const auto it = models_.find(name);
  return it == models_.end() ? nullptr : &it->second;
}

bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttributes* attrs,
                    const Graph& g, const MlModelRegistry* models) {
  std::unordered_map<std::string, PendingMl> pending;
  for (std::size_t i = 0; i < antecedent.size(); ++i) {
    const auto& pred = antecedent[i];
    if (pred.kind != PredKind::kMl) continue;
    const MlModel* model = models == nullptr ? nullptr : models->Find(pred.model);
    if (model == nullptr) {
      throw std::invalid_argument("unregistered ML model: " + pred.model);
    }
    auto& group = pending[pred.model];
    group.model = model;
    MlQuery query;
    query.label = pred.label_name.empty() ? std::to_string(pred.label)
                                          : pred.label_name;
    for (const auto variable : pred.ml_vars) {
      query.entities.push_back(emb[variable]);
    }
    group.queries.push_back(std::move(query));
    group.antecedent_indices.push_back(i);
  }
  for (auto& [name, group] : pending) {
    (void)name;
    group.scores = (*group.model)(group.queries);
    if (group.scores.size() != group.queries.size()) {
      throw std::runtime_error("ML model response size mismatch");
    }
  }

  for (std::size_t i = 0; i < antecedent.size(); ++i) {
    const auto& pred = antecedent[i];
    switch (pred.kind) {
      case PredKind::kConst: {
        if (attrs == nullptr) return false;
        const Attribute* col = FindAttribute(*attrs, pred.attr);
        if (col == nullptr) return false;
        const auto row = emb[pred.node];
        if (!AttributeValid(*col, row)) return false;
        bool ok = false;
        const double rhs = AttrValueAsDouble(pred.value, &ok);
        if (!(ok && EvalCmp(pred.op, AttributeValue(*col, row), rhs))) {
          return false;
        }
        break;
      }
      case PredKind::kVar: {
        if (attrs == nullptr) return false;
        const Attribute* lhs = FindAttribute(*attrs, pred.attr);
        const Attribute* rhs = FindAttribute(*attrs, pred.rhs_attr);
        if (lhs == nullptr || rhs == nullptr) return false;
        const auto ri = emb[pred.node], rj = emb[pred.rhs_node];
        if (!AttributeValid(*lhs, ri) || !AttributeValid(*rhs, rj)) return false;
        if (!EvalCmp(pred.op, AttributeValue(*lhs, ri),
                     AttributeValue(*rhs, rj))) {
          return false;
        }
        break;
      }
      case PredKind::kExists: {
        if (attrs == nullptr) return false;
        const Attribute* col = FindAttribute(*attrs, pred.attr);
        if (col == nullptr || !AttributeValid(*col, emb[pred.node])) {
          return false;
        }
        break;
      }
      case PredKind::kEdge:
        if (!HasEdge(g, emb[pred.src], emb[pred.dst], pred.label)) {
          return false;
        }
        break;
      case PredKind::kMl: {
        const auto& group = pending[pred.model];
        const auto position = std::find(group.antecedent_indices.begin(),
                                        group.antecedent_indices.end(), i);
        const auto index = static_cast<std::size_t>(
            position - group.antecedent_indices.begin());
        const double score = group.scores[index];
        if (std::isnan(score) || score < pred.threshold) return false;
        break;
      }
    }
  }
  return true;
}

}  // namespace gar::core::graph
