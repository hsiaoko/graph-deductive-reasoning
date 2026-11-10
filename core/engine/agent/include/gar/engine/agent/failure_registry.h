#pragma once

// FailureRegistry: recorded motif failures (paper §3.1). A precondition is
// STATIC when no rule derives its predicate (attribute/ML test, edge of a
// non-derived relation): it reads G only, G never changes, so the motif is
// retired for good. It is DYNAMIC when some rule derives its predicate: the
// failure is recorded against the current fact set and the motif is
// re-admitted once Gamma grows.
//
// Simplification: the paper re-admits once the failed PREDICATE gains a
// fact; we re-admit on ANY growth of Gamma (version bump). That is the
// conservative direction — completeness is unaffected (every paper
// re-admission also happens here), at the price of possibly retrying a
// motif whose blocking predicate has not actually gained a fact yet.
// Trying a motif at most once per fact set (Theorem 7) still holds.

#include <cstdint>
#include <unordered_map>

namespace gar {
namespace engine {
namespace agent {

class FailureRegistry {
 public:
  void RecordFailure(std::uint64_t motif_key, bool blocking_pred_is_dynamic,
                     std::uint64_t version) {
    entries_[motif_key] = Entry{blocking_pred_is_dynamic, version};
  }

  // Static failures are retired forever; dynamic ones while the fact set
  // is unchanged since the failure.
  [[nodiscard]] bool IsRetired(std::uint64_t motif_key,
                               std::uint64_t current_version) const {
    const auto it = entries_.find(motif_key);
    if (it == entries_.end()) return false;
    if (!it->second.is_dynamic) return true;
    return current_version <= it->second.version;
  }

 private:
  struct Entry {
    bool is_dynamic;
    std::uint64_t version;
  };
  std::unordered_map<std::uint64_t, Entry> entries_;
};

}  // namespace agent
}  // namespace engine
}  // namespace gar
