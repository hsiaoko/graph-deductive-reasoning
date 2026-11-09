#include "fixpoint_working_set.h"

#include <algorithm>

namespace gar {
namespace engine {
namespace agent {
namespace fixpoint {

ActionRecord& WorkingSet::AddReady(Motif mu, RuleHead head) {
  ready_.push_back(
      ActionRecord{next_ready_id_++, std::move(mu), head, next_seq_++});
  ready_positions_[ready_.back().id] = ready_.size() - 1;
  return ready_.back();
}

void WorkingSet::RemoveReady(std::uint64_t id) {
  const auto it = ready_positions_.find(id);
  if (it == ready_positions_.end()) return;
  const std::size_t pos = it->second;
  std::uint64_t moved_id = 0;
  if (pos + 1 < ready_.size()) {
    ready_[pos] = std::move(ready_.back());
    moved_id = ready_[pos].id;
    ready_positions_[moved_id] = pos;
  }
  ready_.pop_back();
  ready_positions_.erase(it);
  if (moved_id != 0 && ready_priority_enabled_) TouchReady(moved_id);
}

ActionRecord* WorkingSet::FindReady(std::uint64_t id) {
  const auto it = ready_positions_.find(id);
  return it == ready_positions_.end() ? nullptr : &ready_[it->second];
}

void WorkingSet::RebuildReadyHeap() {
  ready_heap_.clear();
  ready_heap_.reserve(ready_.size());
  for (std::size_t pos = 0; pos < ready_.size(); ++pos) {
    const auto& rec = ready_[pos];
    ready_heap_.push_back(ReadyPriority{rec.selection_priority, pos, rec.id,
                                        rec.score.admissible});
  }
  std::make_heap(ready_heap_.begin(), ready_heap_.end(),
                 [](const ReadyPriority& a, const ReadyPriority& b) {
                   if (a.priority != b.priority) return a.priority < b.priority;
                   return a.position > b.position;
                 });
}

void WorkingSet::TouchReady(std::uint64_t id) {
  const auto it = ready_positions_.find(id);
  if (it == ready_positions_.end()) return;
  ready_priority_enabled_ = true;
  const std::size_t pos = it->second;
  const auto& rec = ready_[pos];
  ready_heap_.push_back(ReadyPriority{rec.selection_priority, pos, id,
                                      rec.score.admissible});
  std::push_heap(ready_heap_.begin(), ready_heap_.end(),
                 [](const ReadyPriority& a, const ReadyPriority& b) {
                   if (a.priority != b.priority) return a.priority < b.priority;
                   return a.position > b.position;
                 });
  if (ready_heap_.size() > 4 * ready_.size() + 1024) RebuildReadyHeap();
}

ActionRecord* WorkingSet::HighestReady() {
  const auto less = [](const ReadyPriority& a, const ReadyPriority& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.position > b.position;
  };
  while (!ready_heap_.empty()) {
    const ReadyPriority& top = ready_heap_.front();
    const auto it = ready_positions_.find(top.id);
    if (it != ready_positions_.end()) {
      const std::size_t pos = it->second;
      auto& rec = ready_[pos];
      if (top.position == pos && top.priority == rec.selection_priority &&
          top.admissible == rec.score.admissible) {
        return &rec;
      }
    }
    std::pop_heap(ready_heap_.begin(), ready_heap_.end(), less);
    ready_heap_.pop_back();
  }
  return nullptr;
}

void WorkingSet::DiscardHighestReady() {
  if (HighestReady() == nullptr) return;
  const auto less = [](const ReadyPriority& a, const ReadyPriority& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.position > b.position;
  };
  std::pop_heap(ready_heap_.begin(), ready_heap_.end(), less);
  ready_heap_.pop_back();
}

SuspendedAction& WorkingSet::AddSuspended(SuspendedAction sw) {
  sw.id = next_wid_++;
  suspended_.push_back(std::move(sw));
  auto& stored = suspended_.back();
  suspended_positions_[stored.id] = suspended_.size() - 1;
  if (stored.is_partial) {
    if (stored.wait.open_side) {
      open_waiters_[{stored.wait.slot, stored.wait.bound_value,
                     stored.wait.bound_is_src}]
          .push_back(stored.id);
    } else {
      ground_waiters_[stored.wait.ground].push_back(stored.id);
    }
  }
  return stored;
}

void WorkingSet::RemoveSuspended(std::uint64_t id) {
  const auto it = suspended_positions_.find(id);
  if (it == suspended_positions_.end()) return;
  const std::size_t pos = it->second;
  const auto& wait = suspended_[pos].wait;
  if (suspended_[pos].is_partial) {
    if (wait.open_side) {
      const OpenWaitKey key{wait.slot, wait.bound_value, wait.bound_is_src};
      const auto bucket = open_waiters_.find(key);
      if (bucket != open_waiters_.end()) {
        auto& ids = bucket->second;
        const auto id_it = std::find(ids.begin(), ids.end(), id);
        if (id_it != ids.end()) ids.erase(id_it);
        if (ids.empty()) open_waiters_.erase(bucket);
      }
    } else {
      const auto bucket = ground_waiters_.find(wait.ground);
      if (bucket != ground_waiters_.end()) {
        auto& ids = bucket->second;
        const auto id_it = std::find(ids.begin(), ids.end(), id);
        if (id_it != ids.end()) ids.erase(id_it);
        if (ids.empty()) ground_waiters_.erase(bucket);
      }
    }
  }
  if (pos + 1 < suspended_.size()) {
    suspended_[pos] = std::move(suspended_.back());
    suspended_positions_[suspended_[pos].id] = pos;
  }
  suspended_.pop_back();
  suspended_positions_.erase(it);
}

SuspendedAction* WorkingSet::FindSuspended(std::uint64_t id) {
  const auto it = suspended_positions_.find(id);
  return it == suspended_positions_.end() ? nullptr : &suspended_[it->second];
}

std::vector<std::uint64_t> WorkingSet::MatchingPartials(
    const graph::DerivedEdge& d, std::uint32_t mirror) const {
  std::vector<std::uint64_t> matches;
  auto collect = [&](std::uint32_t slot, std::uint32_t src,
                     std::uint32_t dst) {
    const auto ground =
        ground_waiters_.find(KeyOf(MakeEdgeFact(slot, src, dst)));
    if (ground != ground_waiters_.end()) {
      matches.insert(matches.end(), ground->second.begin(),
                     ground->second.end());
    }
    for (const OpenWaitKey key : {OpenWaitKey{slot, src, true},
                                  OpenWaitKey{slot, dst, false}}) {
      const auto open = open_waiters_.find(key);
      if (open != open_waiters_.end()) {
        matches.insert(matches.end(), open->second.begin(), open->second.end());
      }
    }
  };
  collect(d.slot, d.src, d.dst);
  collect(mirror, d.dst, d.src);
  std::sort(matches.begin(), matches.end(), [&](std::uint64_t a,
                                                std::uint64_t b) {
    return suspended_positions_.at(a) < suspended_positions_.at(b);
  });
  matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
  return matches;
}

}  // namespace fixpoint
}  // namespace agent
}  // namespace engine
}  // namespace gar
