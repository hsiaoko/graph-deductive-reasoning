// src-local: shared motif-application failure classification of the
// executors. GreedyExecutor and BranchBoundExecutor both apply motifs with
// the exact chase semantics (graph::EvalAntecedent) and must classify
// failures identically.
#pragma once

#include <cstdint>

#include "gar/analysis/dependency_graph.h"
#include "gar/analysis/predicate_table.h"
#include "gar/engine/agent/executor.h"
#include "gar/graph/predicate_evaluator.h"

namespace gar {
namespace engine {
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
inline FailureInfo ClassifyFailure(const gar::GARule& rule,
                                   const std::uint32_t* emb,
                                   const graph::VertexAttrs* attrs,
                                   const graph::TripleGraph& g,
                                   const AgentContext& ctx) {
  FailureInfo fi;
  fi.fail_idx = rule.antecedent.size();
  for (std::size_t j = 0; j < rule.antecedent.size(); ++j) {
    const gar::Predicate& pred = rule.antecedent[j];
    if (!graph::EvalAntecedent({pred}, emb, attrs, g)) {
      fi.fail_idx = j;
      if (pred.kind == gar::PredKind::kEdge) {
        const std::uint32_t id =
            ctx.table->Find(analysis::PredicateIdOf(pred, rule));
        fi.dynamic =
            id != analysis::kInvalidPredicate && ctx.dg->InDegree(id) > 0;
      }
      break;
    }
  }
  return fi;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
