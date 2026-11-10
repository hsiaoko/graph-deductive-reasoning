// src-local: shared motif-application failure classification of the
// executor. Applying a motif uses the exact chase semantics
// (gar::core::graph::EvalAntecedent) and classifies failures as static (permanent) or
// dynamic (waits for missing facts).
#pragma once

#include <cstdint>

#include "gar/analysis/dependency_graph.h"
#include "gar/analysis/predicate_table.h"
#include "gar/engine/agent/executor.h"
#include "gar/graph/predicate_evaluator.h"

namespace gar::core::engine {
namespace agent {

struct FailureInfo {
  bool dynamic = false;
  // Index into rule.antecedent of the first failing predicate
  // (== antecedent.size() when none was found, which EvalAntecedent's
  // `false` return rules out).
  std::size_t fail_idx = 0;
};

// Re-evaluates the antecedent per predicate, in order (EvalAntecedent
// short-circuits the same way), to find the blocking precondition of a
// failed application: an edge predicate is DYNAMIC iff some rule derives
// its (base) relation (dependency-graph in-degree > 0); attribute/ML
// preconditions are STATIC (they read G only, and G never changes).
// Whether a rule predicate is DYNAMIC: an edge predicate whose base
// relation some rule derives (dependency-graph in-degree > 0). Attr/ML
// predicates read G only and are static. Shared by the executors' failure
// classification (ClassifyFailure) and the fixpoint executor's
// precondition cache — one definition, no logic fork.
inline bool IsDynamicPredicate(const gar::Predicate& pred,
                               const gar::GARule& rule,
                               const AgentContext& ctx) {
  if (pred.kind != gar::PredKind::kEdge) return false;
  const std::uint32_t id =
      ctx.table->Find(gar::core::analysis::PredicateIdOf(pred, rule));
  return id != gar::core::analysis::kInvalidPredicate && ctx.dg->InDegree(id) > 0;
}

inline FailureInfo ClassifyFailure(const gar::GARule& rule,
                                   const std::uint32_t* emb,
                                   const gar::core::graph::VertexAttrs* attrs,
                                   const gar::core::graph::TripleGraph& g,
                                   const AgentContext& ctx) {
  FailureInfo fi;
  fi.fail_idx = rule.antecedent.size();
  for (std::size_t j = 0; j < rule.antecedent.size(); ++j) {
    const gar::Predicate& pred = rule.antecedent[j];
    if (!gar::core::graph::EvalAntecedent({pred}, emb, attrs, g)) {
      fi.fail_idx = j;
      fi.dynamic = IsDynamicPredicate(pred, rule, ctx);
      break;
    }
  }
  return fi;
}

}  // namespace agent
}  // namespace gar::core::engine
