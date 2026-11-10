#include "precondition_cache.h"

#include <cstring>

namespace gar {
namespace engine {
namespace agent {
namespace deduce {

namespace {

std::uint64_t Mix64(std::uint64_t h, std::uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}

std::uint64_t HashStr(const std::string& s) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : s) h = (h ^ c) * 0x100000001B3ull;
  return h;
}

}  // namespace

std::size_t GroundPreconditionKeyHash::operator()(
    const GroundPreconditionKey& k) const {
  const gar::Predicate& p = k.pred;
  std::uint64_t h = Mix64(0x517cc1b727220a95ull,
                          static_cast<std::uint64_t>(p.kind));
  h = Mix64(h, k.binding[0]);
  h = Mix64(h, k.binding[1]);
  h = Mix64(h, HashStr(p.attr));
  h = Mix64(h, HashStr(p.rhs_attr));
  h = Mix64(h, static_cast<std::uint64_t>(p.op));
  h = Mix64(h, p.label);
  h = Mix64(h, HashStr(p.label_name));
  h = Mix64(h, HashStr(p.model));
  h = Mix64(h, p.ml_vars.size());
  std::uint64_t vb = 0;
  std::visit(
      [&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          vb = HashStr(v);
        } else if constexpr (std::is_same_v<T, bool>) {
          vb = v ? 1 : 2;
        } else {
          std::uint64_t bits = 0;
          if constexpr (sizeof(v) <= 8) std::memcpy(&bits, &v, sizeof(v));
          vb = bits;
        }
      },
      p.value);
  h = Mix64(h, vb);
  std::uint64_t tb = 0;
  std::memcpy(&tb, &p.threshold, sizeof(double));
  h = Mix64(h, tb);
  return static_cast<std::size_t>(h ^ (h >> 31));
}

GroundPreconditionKey MakePreconditionKey(const gar::Predicate& pred,
                                          const std::uint32_t* emb) {
  GroundPreconditionKey k;
  k.pred = pred;  // full logical content (label already resolved)
  switch (pred.kind) {
    case gar::PredKind::kConst:
    case gar::PredKind::kExists:
      k.binding = {emb[pred.node], 0};
      break;
    case gar::PredKind::kVar:
      k.binding = {emb[pred.node], emb[pred.rhs_node]};
      break;
    case gar::PredKind::kEdge:
      k.binding = {emb[pred.src], emb[pred.dst]};
      break;
    case gar::PredKind::kMl:
      k.binding = {emb[pred.ml_vars[0]], emb[pred.ml_vars[1]]};
      break;
  }
  return k;
}

bool GroundEdgeFact(const gar::Predicate& pred, const std::uint32_t* emb,
                    const graph::TripleGraph& g, Fact* out) {
  if (pred.kind != gar::PredKind::kEdge) return false;
  const std::uint32_t u = emb[pred.src], v = emb[pred.dst];
  const std::uint32_t num_rel = g.num_relations();
  if (pred.label >= num_rel) {
    *out = MakeEdgeFact(pred.label - num_rel, v, u);
  } else {
    *out = MakeEdgeFact(pred.label, u, v);
  }
  return true;
}

PrecondState ClassifyPredicate(const gar::Predicate& pred,
                               const gar::GARule& rule,
                               const std::uint32_t* emb,
                               const graph::VertexAttrs* attrs,
                               const graph::TripleGraph& g,
                               const AgentContext& ctx) {
  const bool ok = graph::EvalAntecedent({pred}, emb, attrs, g);
  const bool dyn = IsDynamicPredicate(pred, rule, ctx);
  if (dyn) {
    return ok ? PrecondState::kDynamicPresent : PrecondState::kDynamicMissing;
  }
  return ok ? PrecondState::kStaticTrue : PrecondState::kStaticFalse;
}

std::optional<bool> PreconditionCache::Lookup(
    const GroundPreconditionKey& k, bool is_static,
    std::uint64_t version) const {
  const auto it = m_.find(k);
  if (it == m_.end()) return std::nullopt;
  const Entry& e = it->second;
  if (e.is_static) return e.value;                 // permanent
  if (e.value) return true;                        // dynamic positive: Γ grows
  return e.version == version ? std::optional<bool>(false) : std::nullopt;
}

void PreconditionCache::Record(const GroundPreconditionKey& k, bool value,
                               bool is_static, std::uint64_t version) {
  const auto it = m_.find(k);
  if (it != m_.end()) {
    // Upgrade only in the safe direction: a negative may be replaced by a
    // positive (Γ grew); static entries never change.
    if (!it->second.is_static && !it->second.value && value) {
      it->second = Entry{true, false, version};
    }
    return;
  }
  m_.emplace(k, Entry{value, is_static, version});
}

}  // namespace deduce
}  // namespace agent
}  // namespace engine
}  // namespace gar
