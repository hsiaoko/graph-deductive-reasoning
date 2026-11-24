#include "gar/core/agent/fact_encoder.h"

#include <string>
#include <utility>

#include "gar/core/analysis/predicate_table.h"

namespace gar::core::engine::agent {
namespace {

std::uint64_t Fnv1a64(const std::string& value) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const unsigned char c : value) {
    hash = (hash ^ c) * 0x100000001B3ull;
  }
  return hash;
}

std::uint64_t NameSymbol(const std::string& name) {
  std::uint64_t state = Fnv1a64(name);
  return gar::core::hdc::SplitMix64Next(state) | (1ull << 63);
}

}  // namespace

std::uint64_t FactPredicateSymbol(
    const Fact& fact, const gar::core::graph::Graph& graph,
    const gar::core::analysis::PredicateTable& predicates) {
  const std::string& name = fact.kind == FactKind::kEdge
                                ? graph.RelationName(fact.pred)
                                : predicates.Name(fact.pred);
  return NameSymbol(name);
}

std::uint64_t GraphFingerprint(const gar::core::graph::Graph& graph) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  auto add = [&](const std::string& value) {
    for (const unsigned char c : value) hash = (hash ^ c) * 0x100000001B3ull;
    hash = (hash ^ 0xFF) * 0x100000001B3ull;
  };
  add(std::to_string(graph.num_entities()));
  add(std::to_string(graph.num_relations()));
  add(std::to_string(graph.num_edges()));
  for (std::uint32_t i = 0; i < graph.num_entities(); ++i)
    add(graph.EntityName(i));
  for (std::uint32_t i = 0; i < graph.num_relations(); ++i)
    add(graph.RelationName(i));
  return hash;
}

const gar::core::hdc::DenseVector& FactEncoder::PredicateDense(
    std::uint64_t symbol) const {
  const auto it = pred_dense_cache_.find(symbol);
  if (it != pred_dense_cache_.end()) return it->second;
  auto vector =
      gar::core::hdc::DenseVector::FromHypervector(encoder_.SymbolVector(symbol));
  return pred_dense_cache_.emplace(symbol, std::move(vector)).first->second;
}

const gar::core::hdc::DenseVector& FactEncoder::ArgumentDense(
    std::uint32_t position, std::uint64_t entity) const {
  const std::uint64_t key = (static_cast<std::uint64_t>(position) << 32) | entity;
  const auto it = arg_dense_cache_.find(key);
  if (it != arg_dense_cache_.end()) return it->second;
  auto vector = gar::core::hdc::DenseVector::FromHypervector(
      gar::core::hdc::Hypervector::Permute(encoder_.SymbolVector(entity), position));
  return arg_dense_cache_.emplace(key, std::move(vector)).first->second;
}

gar::core::hdc::DenseVector FactEncoder::Encode(const Fact& fact) const {
  std::uint64_t pred_symbol;
  if (fact.kind == FactKind::kEdge) {
    auto [it, inserted] = pred_symbol_cache_.try_emplace(fact.pred);
    if (inserted) it->second = NameSymbol(graph_->RelationName(fact.pred));
    pred_symbol = it->second;
  } else {
    pred_symbol = FactPredicateSymbol(fact, *graph_, *predicates_);
  }
  // v_f = normalize(v_p + sum_i rho^i(v_{a_i})); each term is a cached table
  // read, so encoding a fact is two vector adds plus a normalize instead of
  // regenerating every symbol's hypervector from its hash on each call.
  gar::core::hdc::DenseVector sum = PredicateDense(pred_symbol);
  for (std::uint32_t i = 0; i < fact.arity; ++i) {
    sum.Add(ArgumentDense(i, fact.args[i]));
  }
  return sum.Normalized();
}

gar::core::hdc::DenseVector FactEncoder::EncodeGoal(const Goal& goal) const {
  const Fact goal_fact = MakeEdgeFact(goal.slot, goal.args[0].value_or(0),
                                      goal.args[1].value_or(0));
  const std::uint64_t args[2] = {
      goal.args[0] ? static_cast<std::uint64_t>(*goal.args[0])
                   : NameSymbol("gar.goal.wildcard.0"),
      goal.args[1] ? static_cast<std::uint64_t>(*goal.args[1])
                   : NameSymbol("gar.goal.wildcard.1")};
  return encoder_.EncodeFactDense(
      FactPredicateSymbol(goal_fact, *graph_, *predicates_), args, 2);
}

}  // namespace gar::core::engine::agent
