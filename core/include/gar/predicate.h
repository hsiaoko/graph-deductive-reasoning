#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

#include "gar/types.h"

// Predicates over pattern nodes, covering the five forms of GAR predicates
// (Fan et al., "Capturing Associations in Graphs", PVLDB 2020):
//
//   kConst   x.A <op> c        attribute vs. constant
//   kVar     x.A <op> y.B      attribute vs. attribute
//   kEdge    l(x, y)           edge with label l exists between x and y
//   kExists  x.A               attribute A exists at x
//   kMl      M(x, y, l)        ML classifier predicts an l-labeled link
namespace gar {

enum class PredKind { kConst, kVar, kEdge, kExists, kMl };

enum class CmpOp { kEq, kNe, kLt, kLe, kGt, kGe };

const char* ToString(PredKind kind);
const char* ToString(CmpOp op);
CmpOp ParseCmpOp(const std::string& s);    // throws std::invalid_argument
PredKind ParsePredKind(const std::string& s);

using AttrValue = std::variant<std::int64_t, double, bool, std::string>;

struct Predicate {
  PredKind kind = PredKind::kConst;

  // kConst / kVar / kExists: subject is `node.attr`.
  VertexID node = 0;
  std::string attr;
  CmpOp op = CmpOp::kEq;

  // kConst: constant right-hand side.
  AttrValue value = std::int64_t{0};

  // kVar: attribute right-hand side (rhs_node.rhs_attr).
  VertexID rhs_node = 0;
  std::string rhs_attr;

  // kEdge: edge existence between src and dst. `label_name` is the symbolic
  // form; `label` is its dictionary id (kWildcardLabel if unresolved).
  VertexID src = 0;
  VertexID dst = 0;
  LabelID label = kWildcardLabel;
  std::string label_name;

  // kMl: model applied to (ml_vars[0], ml_vars[1]) predicting `label`.
  std::vector<VertexID> ml_vars;
  std::string model;
  double threshold = 0.0;

  // Structural equality over the logical content (used for dedup and the
  // support/confidence bookkeeping).
  bool operator==(const Predicate& o) const;
};

std::ostream& operator<<(std::ostream& os, const AttrValue& v);
std::ostream& operator<<(std::ostream& os, const Predicate& p);

}  // namespace gar
