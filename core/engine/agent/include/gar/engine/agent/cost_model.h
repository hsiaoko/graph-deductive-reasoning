#pragma once

// CostModel: application cost estimates (paper §3.2: every application
// pays for evaluating its preconditions; cost(mu,S) >= w0(phi) >= 1).
//
//   Estimate(pred)      T(p): cost of testing one precondition
//   RuleLowerBound(phi) w0: certified lower bound of one application
//
// UnitCostModel: everything 1 — "under unit cost, each application costs
// exactly 1". Anchor for later: a LogCostModel would estimate T/w0 from
// logged averages of past chains (§5.1's logged-average cost source);
// deliberately not implemented this phase.

#include "gar/predicate.h"
#include "gar/rule.h"

namespace gar::core::engine {
namespace agent {

class CostModel {
 public:
  virtual ~CostModel() = default;
  virtual double Estimate(const gar::Predicate& pred) const = 0;
  // w0(phi); the unit model charges exactly 1 per application.
  virtual double RuleLowerBound(const gar::GARule& rule) const = 0;
  // Pattern-edge premise tests are edge lookups; unit cost 1. (Anchor for
  // per-kind logged costs.)
  virtual double EdgeTestCost() const { return 1.0; }
};

class UnitCostModel final : public CostModel {
 public:
  double Estimate(const gar::Predicate& pred) const override {
    (void)pred;
    return 1.0;
  }
  double RuleLowerBound(const gar::GARule& rule) const override {
    (void)rule;
    return 1.0;
  }
};

}  // namespace agent
}  // namespace gar::core::engine
