#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gar/miner/pattern_matcher.h"
#include "gar/miner/triple_graph.h"
#include "gar/rule.h"

// ChaseEngine: CPU deduction (logical closure / chase) of a GAR rule set over
// a knowledge graph.
//
//   G_{i+1} = G_i ∪ { p₀(emb) | rule Q[x̄](X → p₀), emb ∈ Q(G_i), X(emb) }
//
// iterated to a fixpoint. Matching reuses miner::PatternMatcher (homomorphism
// semantics, edge-label constraints, vertex-label post-filter — the same
// counting convention as the miners). X predicates are evaluated per
// embedding: const (x.A⊕c), var (x.A⊕y.B), exists (x.A) against the vertex
// attribute table, edge (l(x,y)) against the current (augmented) graph; ml
// predicates are unsupported and cause the rule to be skipped. Derived facts
// are merged into the TripleGraph (base + inverse slot) after each round, so
// later rounds match on the augmented graph.
//
// This is the naive chase: every round re-enumerates every rule. Rules within
// a round are evaluated in parallel (oneTBB when available); per-rule
// derived-fact candidates are merged in rule order, keeping dedup credit
// deterministic. Termination is guaranteed (heads introduce no fresh vertices,
// so at most |V|²·|R| edges exist); caps bound the practical cost and flag a
// non-fixpoint stop.
namespace gar {
namespace reason {

// CPU-side vertex attribute column. The app converts AttributesIO columns
// (Int / Float64, with valid mask) into this; the engine never sees
// MatrixGraph types.
struct AttrColumn {
  std::string name;
  bool is_int = true;
  std::vector<std::uint8_t> valid;  // empty = dense (every row valid)
  std::vector<std::int64_t> ints;
  std::vector<double> doubles;

  [[nodiscard]] bool Valid(std::uint32_t row) const {
    return valid.empty() || (row < valid.size() && valid[row]);
  }
  [[nodiscard]] double Value(std::uint32_t row) const {
    return is_int ? static_cast<double>(ints[row]) : doubles[row];
  }
};

struct VertexAttrs {
  std::vector<AttrColumn> columns;
  [[nodiscard]] const AttrColumn* Find(const std::string& name) const {
    for (const auto& c : columns) {
      if (c.name == name) return &c;
    }
    return nullptr;
  }
};

struct ChaseConfig {
  std::uint64_t max_matches_per_rule = 1'000'000;  // embedding cap per rule
  std::uint64_t max_derivations = 10'000'000;      // total new-facts cap
  std::uint32_t max_rounds = 100;
  bool keep_matches = true;  // retain final-round embeddings for output
};

struct DerivedEdge {
  std::uint32_t src, dst, slot;  // slot as named by the rule's head label
  std::uint32_t rule;            // index into the input rule vector
  std::uint32_t round;           // 1-based chase round
};

struct RuleChaseStats {
  std::uint64_t embeddings = 0;   // topology matches, final round
  std::uint64_t x_satisfied = 0;  // matches passing X, final round
  std::uint64_t derived = 0;      // new facts first derived by this rule
  bool match_capped = false;      // embedding cap hit (counts are lower bounds)
  bool skipped = false;           // unsupported rule shape (see note)
  std::string note;
};

// Final-round matches of one rule (kept when ChaseConfig::keep_matches).
struct RuleMatches {
  std::uint32_t n = 0;                // pattern vertices per embedding
  std::vector<std::uint32_t> embs;    // flat, n per row
  std::vector<std::uint8_t> x_ok;     // X satisfied
  std::vector<std::uint8_t> head_hit; // head edge present in the final graph
  bool capped = false;
};

struct ChaseResult {
  std::vector<RuleChaseStats> rules;
  std::vector<RuleMatches> matches;
  std::vector<DerivedEdge> derived;  // all new facts, in derivation order
  std::uint32_t rounds = 0;
  bool fixpoint = false;             // false when stopped by a cap
};

class ChaseEngine {
 public:
  // `g` is mutated in place: derived edges are merged into its indexes.
  ChaseEngine(gar::miner::TripleGraph& g, const VertexAttrs* attrs)
      : g_(g), attrs_(attrs) {}

  ChaseResult Run(const std::vector<GARule>& rules, const ChaseConfig& cfg);

 private:
  gar::miner::TripleGraph& g_;
  const VertexAttrs* attrs_;  // may be nullptr (only edge/exists predicates)
};

struct RoundMerge {
  std::uint64_t new_facts = 0;
  bool deriv_capped = false;  // max_derivations hit mid-merge
};

// Ordered merge of one chase round's per-rule candidate facts: dedupes
// (slot, src, dst) across rules in rule order (the lowest-index rule gets
// the credit), appends to result->derived / rules[i].derived, and merges
// the batch into the graph (base + inverse mirror). Shared by the CPU chase
// and the GPU deduce app so credit assignment is identical across engines.
RoundMerge MergeRoundFacts(
    const std::vector<std::vector<DerivedEdge>>& contribs,
    std::uint64_t max_derivations, gar::miner::TripleGraph& g,
    ChaseResult* result);

// keep[t] = edge (src[t], dst[t], slot) NOT present in g — the head
// pre-filter of the chase, factored out for the GPU deduce app (whose
// .cu translation unit cannot include TBB headers). Parallel over t when
// oneTBB is available.
void MarkAbsentEdges(const gar::miner::TripleGraph& g,
                     const std::uint32_t* src, const std::uint32_t* dst,
                     std::uint32_t slot, std::size_t n, char* keep);

// Evaluates a rule's antecedent conjunction against one embedding:
// const (x.A⊕c) / var (x.A⊕y.B) / exists (x.A) against the vertex
// attribute table (nullptr attrs makes every attribute predicate false),
// edge (l(x,y)) against the graph; ml predicates return false. Shared by
// the chase and the validate_rules app.
bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttrs* attrs,
                    const gar::miner::TripleGraph& g);

}  // namespace reason
}  // namespace gar
