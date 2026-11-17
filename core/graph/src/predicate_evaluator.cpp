#include "gar/core/graph/predicate_evaluator.h"

#include <algorithm>
#include <cmath>

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

}  // namespace

bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttributes* attrs,
                    const Graph& g) {
  for (const auto& pred : antecedent) {
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
        if (!AttributeValid(*lhs, ri) || !AttributeValid(*rhs, rj)) {
          return false;
        }
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
      case PredKind::kMl:
        return false;
    }
  }
  return true;
}

}  // namespace gar::core::graph
