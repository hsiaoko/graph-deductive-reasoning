#include "gar/graph/predicate_evaluator.h"

#include <algorithm>

namespace gar {
namespace graph {

namespace {

bool EvalCmp(CmpOp op, double lhs, double rhs) {
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
  *ok = false;  // string constants can't compare against numeric columns
  return 0.0;
}

bool HasEdge(const TripleGraph& g, std::uint32_t s,
             std::uint32_t d, std::uint32_t slot) {
  const auto [b, e] = g.OutEdgeRange(s);
  const std::uint32_t* dsts = g.OutEdgeDsts();
  auto lo = std::lower_bound(dsts + b, dsts + e, d);
  for (auto it = lo; it != dsts + e && *it == d; ++it) {
    if (g.OutEdgeSlots()[it - dsts] == slot) return true;
  }
  return false;
}

}  // namespace

bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttrs* attrs,
                    const TripleGraph& g) {
  for (const auto& pred : antecedent) {
    switch (pred.kind) {
      case PredKind::kConst: {
        if (attrs == nullptr) return false;
        const AttrColumn* col = attrs->Find(pred.attr);
        if (col == nullptr) return false;
        const auto row = emb[pred.node];
        if (!col->Valid(row)) return false;
        bool ok = false;
        const double rhs = AttrValueAsDouble(pred.value, &ok);
        if (!(ok && EvalCmp(pred.op, col->Value(row), rhs))) return false;
        break;
      }
      case PredKind::kVar: {
        if (attrs == nullptr) return false;
        const AttrColumn* lhs = attrs->Find(pred.attr);
        const AttrColumn* rhs = attrs->Find(pred.rhs_attr);
        if (lhs == nullptr || rhs == nullptr) return false;
        const auto ri = emb[pred.node], rj = emb[pred.rhs_node];
        if (!lhs->Valid(ri) || !rhs->Valid(rj)) return false;
        if (!EvalCmp(pred.op, lhs->Value(ri), rhs->Value(rj))) return false;
        break;
      }
      case PredKind::kExists: {
        if (attrs == nullptr) return false;
        const AttrColumn* col = attrs->Find(pred.attr);
        if (col == nullptr || !col->Valid(emb[pred.node])) return false;
        break;
      }
      case PredKind::kEdge:
        if (!HasEdge(g, emb[pred.src], emb[pred.dst], pred.label)) {
          return false;
        }
        break;
      case PredKind::kMl:
        return false;  // unsupported on CPU
    }
  }
  return true;
}

}  // namespace graph
}  // namespace gar
