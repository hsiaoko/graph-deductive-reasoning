#include "gar/miner/rule_generator.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace gar::tools::miner {

using gar::core::graph::Graph;

RuleGenerator::RuleGenerator(const Graph& graph, RuleGeneratorConfig config)
    : g_(graph), cfg_(std::move(config)) {
  metadata_.reserve(g_.num_slots());
  for (std::uint32_t slot = 0; slot < g_.num_slots(); ++slot) {
    metadata_.push_back(ExtractMetadata(slot));
  }
}

RelationMetadata RuleGenerator::ExtractMetadata(std::uint32_t slot) const {
  RelationMetadata metadata;
  metadata.slot = slot;
  const auto& pairs = g_.RelationPairs(slot);
  metadata.support = pairs.size();
  metadata.sources.reserve(pairs.size());
  metadata.targets.reserve(pairs.size());
  for (std::uint64_t pair : pairs) {
    metadata.sources.insert(static_cast<std::uint32_t>(pair >> 32));
    metadata.targets.insert(static_cast<std::uint32_t>(pair));
  }
  return metadata;
}

std::vector<std::uint64_t> RuleGenerator::CountHeadSupport(
    const RelationMetadata& p, const RelationMetadata& q,
    const std::vector<char>& eligible_heads) const {
  std::vector<std::uint64_t> support(g_.num_relations(), 0);
  const std::uint32_t* dsts = g_.OutEdgeDsts();
  const std::uint32_t* slots = g_.OutEdgeSlots();
  for (std::uint32_t src : p.sources) {
    const auto [begin, end] = g_.OutEdgeRange(src);
    for (std::size_t i = begin; i < end; ++i) {
      const std::uint32_t slot = slots[i];
      if (slot >= g_.num_relations() || !eligible_heads[slot] ||
          !q.targets.contains(dsts[i])) {
        continue;
      }
      ++support[slot];
    }
  }
  return support;
}

GARule RuleGenerator::BuildRule(const CandidateRule& candidate) const {
  GARule rule;
  rule.name = g_.SlotName(candidate.p) + "(x0,x1) ∧ " +
              g_.SlotName(candidate.q) + "(x2,x3) → " +
              g_.SlotName(candidate.r) + "(x0,x3)";
  rule.n_vertices = 4;
  rule.edge_src = {0, 2};
  rule.edge_dst = {1, 3};
  rule.edge_labels = {static_cast<std::int32_t>(candidate.p),
                      static_cast<std::int32_t>(candidate.q)};
  rule.node_labels.assign(4, kWildcardLabel);
  rule.node_label_names.assign(4, "");
  rule.pivot = {0, 3};
  rule.labels.emplace(g_.SlotName(candidate.p), candidate.p);
  rule.labels.emplace(g_.SlotName(candidate.q), candidate.q);
  rule.labels.emplace(g_.SlotName(candidate.r), candidate.r);

  Predicate head;
  head.kind = PredKind::kEdge;
  head.src = 0;
  head.dst = 3;
  head.label = candidate.r;
  head.label_name = g_.SlotName(candidate.r);
  rule.consequent = head;

  rule.quality.support = candidate.support;
  rule.quality.confidence = candidate.confidence;
  rule.quality.dataset = cfg_.dataset_root;
  rule.quality.miner = "rule-gen-v1";
  return rule;
}

std::vector<CandidateRule> RuleGenerator::Generate() {
  std::vector<CandidateRule> out;
  out.reserve(cfg_.count);
  stats_ = {};

  for (std::uint32_t p = 0; p < g_.num_slots(); ++p) {
    for (std::uint32_t q = 0; q < g_.num_slots(); ++q) {
      const auto& body_p = metadata_[p];
      const auto& body_q = metadata_[q];
      const std::uint64_t source_count = body_p.sources.size();
      const std::uint64_t target_count = body_q.targets.size();
      if (source_count == 0 || target_count == 0) continue;
      if (source_count >
          std::numeric_limits<std::uint64_t>::max() / target_count) {
        throw std::overflow_error("candidate pair count overflows uint64");
      }
      const std::uint64_t candidate_pair_count = source_count * target_count;
      if (candidate_pair_count < cfg_.min_support) continue;
      ++stats_.body_pairs_evaluated;

      std::vector<char> eligible_heads(g_.num_relations(), 0);
      bool any_eligible = false;
      for (std::uint32_t r = 0; r < g_.num_relations(); ++r) {
        if (metadata_[r].support >= cfg_.min_support &&
            candidate_pair_count > metadata_[r].support) {
          eligible_heads[r] = 1;
          any_eligible = true;
        }
      }
      if (!any_eligible) continue;

      const auto head_support =
          CountHeadSupport(body_p, body_q, eligible_heads);
      for (std::uint32_t r = 0; r < g_.num_relations(); ++r) {
        if (!eligible_heads[r]) continue;
        ++stats_.candidates_scored;
        const std::uint64_t support = head_support[r];
        if (support < cfg_.min_support) continue;
        const std::uint64_t novel_count = candidate_pair_count - support;
        if (novel_count == 0) continue;

        CandidateRule candidate;
        candidate.p = p;
        candidate.q = q;
        candidate.r = r;
        candidate.candidate_pair_count = candidate_pair_count;
        candidate.support = support;
        candidate.confidence =
            static_cast<double>(support) / candidate_pair_count;
        candidate.novel_count = novel_count;
        candidate.rule = BuildRule(candidate);
        out.push_back(std::move(candidate));
        if (out.size() == cfg_.count) {
          stats_.rules_emitted = out.size();
          return out;
        }
      }
    }
  }
  stats_.rules_emitted = out.size();
  return out;
}

}  // namespace gar::tools::miner
