#include "gar/engine/agent/motif.h"

#include "gar/analysis/predicate_table.h"
#include "gar/hdc/hypervector.h"

namespace gar::core::engine {
namespace agent {

RuleHead ResolveRuleHead(const gar::GARule& rule) {
  RuleHead h;
  if (rule.consequent && rule.consequent->kind == gar::PredKind::kEdge &&
      rule.consequent->label != gar::kWildcardLabel) {
    h.valid = true;
    h.slot = rule.consequent->label;
    h.pu = rule.consequent->src;
    h.pv = rule.consequent->dst;
  }
  if (h.valid && rule.pivot.size() == 2) {
    h.pu = rule.pivot[0];
    h.pv = rule.pivot[1];
  }
  return h;
}

std::uint64_t MotifHash(std::uint32_t rule, const std::uint32_t* emb,
                        std::uint32_t n) {
  std::uint64_t h = 0x6D6F746966000000ull ^ rule;  // "motif\0" salt ^ rule
  for (std::uint32_t i = 0; i < n; ++i) {
    h ^= emb[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  std::uint64_t s = h;
  return gar::core::hdc::SplitMix64Next(s);
}

void GroundPremises(const gar::GARule& rule, const Motif& mu,
                    const gar::core::graph::TripleGraph& g,
                    const gar::core::analysis::PredicateTable& table,
                    std::vector<Fact>* out) {
  out->clear();
  const std::uint32_t num_rel = g.num_relations();
  const std::uint32_t* emb = mu.embedding.data();

  // Pattern edges; inverse slots are normalized to the base relation with
  // swapped endpoints (r^-1(u,v) is the data edge r(v,u)).
  for (std::size_t e = 0; e < rule.edge_src.size(); ++e) {
    const gar::LabelID l = static_cast<gar::LabelID>(rule.edge_labels[e]);
    const std::uint32_t s = static_cast<std::uint32_t>(rule.edge_src[e]);
    const std::uint32_t d = static_cast<std::uint32_t>(rule.edge_dst[e]);
    if (l >= num_rel) {
      out->push_back(MakeEdgeFact(l - num_rel, emb[d], emb[s]));
    } else {
      out->push_back(MakeEdgeFact(l, emb[s], emb[d]));
    }
  }

  // Antecedent predicates, grounded.
  for (const auto& pred : rule.antecedent) {
    switch (pred.kind) {
      case gar::PredKind::kConst:
      case gar::PredKind::kExists: {
        const std::uint32_t id = table.Find(pred.attr);
        if (id != gar::core::analysis::kInvalidPredicate) {
          out->push_back(MakeAttrFact(id, emb[pred.node]));
        }
        break;
      }
      case gar::PredKind::kVar: {
        const std::uint32_t id = table.Find(pred.attr);
        if (id != gar::core::analysis::kInvalidPredicate) {
          out->push_back(MakeAttrFact2(id, emb[pred.node],
                                       emb[pred.rhs_node]));
        }
        break;
      }
      case gar::PredKind::kEdge: {
        const std::uint32_t u = emb[pred.src], v = emb[pred.dst];
        if (pred.label >= num_rel) {
          out->push_back(MakeEdgeFact(pred.label - num_rel, v, u));
        } else {
          out->push_back(MakeEdgeFact(pred.label, u, v));
        }
        break;
      }
      case gar::PredKind::kMl: {
        const std::uint32_t id = table.Find(pred.model);
        if (id != gar::core::analysis::kInvalidPredicate && pred.ml_vars.size() >= 2) {
          out->push_back(MakeMlFact(id, emb[pred.ml_vars[0]],
                                    emb[pred.ml_vars[1]]));
        }
        break;
      }
    }
  }
}

}  // namespace agent
}  // namespace gar::core::engine
