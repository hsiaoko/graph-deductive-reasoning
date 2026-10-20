#include "gar/predicate.h"

#include <stdexcept>

namespace gar {

const char* ToString(PredKind kind) {
  switch (kind) {
    case PredKind::kConst: return "const";
    case PredKind::kVar: return "var";
    case PredKind::kEdge: return "edge";
    case PredKind::kExists: return "exists";
    case PredKind::kMl: return "ml";
  }
  return "?";
}

PredKind ParsePredKind(const std::string& s) {
  if (s == "const") return PredKind::kConst;
  if (s == "var") return PredKind::kVar;
  if (s == "edge") return PredKind::kEdge;
  if (s == "exists") return PredKind::kExists;
  if (s == "ml") return PredKind::kMl;
  throw std::invalid_argument("unknown predicate kind: " + s);
}

const char* ToString(CmpOp op) {
  switch (op) {
    case CmpOp::kEq: return "==";
    case CmpOp::kNe: return "!=";
    case CmpOp::kLt: return "<";
    case CmpOp::kLe: return "<=";
    case CmpOp::kGt: return ">";
    case CmpOp::kGe: return ">=";
  }
  return "?";
}

CmpOp ParseCmpOp(const std::string& s) {
  if (s == "==" || s == "=" || s == "eq") return CmpOp::kEq;
  if (s == "!=" || s == "ne") return CmpOp::kNe;
  if (s == "<"  || s == "lt") return CmpOp::kLt;
  if (s == "<=" || s == "le") return CmpOp::kLe;
  if (s == ">"  || s == "gt") return CmpOp::kGt;
  if (s == ">=" || s == "ge") return CmpOp::kGe;
  throw std::invalid_argument("unknown comparison operator: " + s);
}

bool Predicate::operator==(const Predicate& o) const {
  if (kind != o.kind) return false;
  switch (kind) {
    case PredKind::kConst:
      return node == o.node && attr == o.attr && op == o.op && value == o.value;
    case PredKind::kVar:
      return node == o.node && attr == o.attr && op == o.op &&
             rhs_node == o.rhs_node && rhs_attr == o.rhs_attr;
    case PredKind::kEdge:
      return src == o.src && dst == o.dst && label == o.label &&
             label_name == o.label_name;
    case PredKind::kExists:
      return node == o.node && attr == o.attr;
    case PredKind::kMl:
      return model == o.model && ml_vars == o.ml_vars && label == o.label &&
             threshold == o.threshold;
  }
  return false;
}

std::ostream& operator<<(std::ostream& os, const AttrValue& v) {
  std::visit(
      [&os](const auto& x) {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, bool>) {
          os << (x ? "true" : "false");
        } else {
          os << x;
        }
      },
      v);
  return os;
}

std::ostream& operator<<(std::ostream& os, const Predicate& p) {
  switch (p.kind) {
    case PredKind::kConst:
      os << "x" << p.node << "." << p.attr << " " << ToString(p.op) << " "
         << p.value;
      break;
    case PredKind::kVar:
      os << "x" << p.node << "." << p.attr << " " << ToString(p.op) << " x"
         << p.rhs_node << "." << p.rhs_attr;
      break;
    case PredKind::kEdge:
      os << p.label_name << "(x" << p.src << ",x" << p.dst << ")";
      break;
    case PredKind::kExists:
      os << "exists(x" << p.node << "." << p.attr << ")";
      break;
    case PredKind::kMl:
      os << p.model << "(";
      for (std::size_t i = 0; i < p.ml_vars.size(); ++i) {
        if (i) os << ", ";
        os << "x" << p.ml_vars[i];
      }
      os << ", " << p.label_name << ", >=" << p.threshold << ")";
      break;
  }
  return os;
}

}  // namespace gar
