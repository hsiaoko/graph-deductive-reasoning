#include "gpu_match_backend.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/data_structures/device_buffer.cuh"
#include "core/data_structures/host_buffer.cuh"
#include "gar/core/graph/graph.h"

namespace gar::core::engine::agent {
namespace {

using sics::matrixgraph::core::data_structures::Buffer;
using sics::matrixgraph::core::data_structures::DeviceOwnedBuffer;

constexpr int kMaxPatternVertices = 16;
constexpr std::uint64_t kMaxChunkOutputValues = 16ULL * 1024ULL * 1024ULL;

void CheckCuda(cudaError_t status, const char* operation) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string("GPU AdaDeduce: ") + operation +
                             ": " + cudaGetErrorString(status));
  }
}

struct SlotView {
  const std::uint64_t* base_pairs = nullptr;
  const std::uint64_t* delta_pairs = nullptr;
  std::uint32_t base_count = 0;
  std::uint32_t delta_count = 0;
};

struct DeviceMatchPlan {
  const std::int32_t* edge_src = nullptr;
  const std::int32_t* edge_dst = nullptr;
  const std::int32_t* edge_slot = nullptr;
  const std::int32_t* order = nullptr;
  const std::int32_t* drivers = nullptr;
  const std::int32_t* back_offsets = nullptr;
  const std::int32_t* back_edges = nullptr;
  int n_vertices = 0;
  int n_edges = 0;
};

struct DeviceMatchOutput {
  std::uint32_t* embeddings = nullptr;
  std::uint64_t* produced = nullptr;
  std::uint64_t capacity = 0;
};

struct DeviceMatchJob {
  DeviceMatchPlan plan;
  DeviceMatchOutput output;
  SlotView neighbor;
  std::int32_t pin_vertices[kMaxPatternVertices] = {};
  std::uint32_t pin_values[kMaxPatternVertices] = {};
  std::uint64_t base_begin = 0;
  std::uint64_t delta_begin = 0;
  std::uint64_t capacity = 0;
  std::uint64_t work_offset = 0;
  std::uint64_t work_count = 0;
  std::uint32_t base_count = 0;
  std::uint32_t delta_count = 0;
  std::uint32_t seed_src = 0;
  std::uint32_t seed_dst = 0;
  std::uint32_t seed_edge = 0;
  int n_pins = 0;
  bool has_seed = false;
  bool chain2_delta = false;
};

struct DeltaAppend {
  std::uint32_t slot = 0;
  std::uint32_t src = 0;
  std::uint32_t dst = 0;
};

__host__ __device__ std::uint64_t PackPair(std::uint32_t src,
                                            std::uint32_t dst) {
  return (static_cast<std::uint64_t>(src) << 32) | dst;
}

__device__ std::uint32_t FlipSlot(std::uint32_t slot,
                                  std::uint32_t num_relations) {
  return slot < num_relations ? slot + num_relations : slot - num_relations;
}

__device__ std::uint32_t LowerBound(const std::uint64_t* values,
                                    std::uint32_t count,
                                    std::uint64_t key) {
  std::uint32_t lo = 0;
  std::uint32_t hi = count;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (values[mid] < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

__device__ bool HasEdge(const SlotView& slot, std::uint32_t src,
                        std::uint32_t dst) {
  const std::uint64_t key = PackPair(src, dst);
  const std::uint32_t pos = LowerBound(slot.base_pairs, slot.base_count, key);
  if (pos < slot.base_count && slot.base_pairs[pos] == key) return true;
  const std::uint32_t delta_pos =
      LowerBound(slot.delta_pairs, slot.delta_count, key);
  return delta_pos < slot.delta_count && slot.delta_pairs[delta_pos] == key;
}

__device__ int PinValue(const DeviceMatchJob& job, int vertex,
                        std::uint32_t* value) {
  for (int i = 0; i < job.n_pins; ++i) {
    if (job.pin_vertices[i] == vertex) {
      *value = job.pin_values[i];
      return 1;
    }
  }
  return 0;
}

__device__ void EmitEmbedding(const DeviceMatchJob& job,
                              const std::int32_t* embedding,
                              const DeviceMatchOutput& output) {
  const std::uint64_t row = atomicAdd(
      reinterpret_cast<unsigned long long*>(output.produced), 1ULL);
  if (row >= output.capacity) return;
  std::uint32_t* out = output.embeddings +
                       row * static_cast<std::uint64_t>(job.plan.n_vertices);
  for (int v = 0; v < job.plan.n_vertices; ++v) {
    out[v] = static_cast<std::uint32_t>(embedding[v]);
  }
}

struct CandidateCursor {
  std::uint32_t base_next = 0;
  std::uint32_t base_count = 0;
  std::uint32_t delta_next = 0;
  std::uint32_t anchor = 0;
  std::uint32_t scan_slot = 0;
  std::uint32_t pin = 0;
  bool has_pin = false;
  bool pin_done = false;
};

__device__ void InitCursor(const DeviceMatchJob& job, const SlotView* slots,
                           std::uint32_t num_relations, int pos,
                           const std::int32_t* embedding,
                           CandidateCursor* cursor) {
  const DeviceMatchPlan& plan = job.plan;
  const int vertex = plan.order[pos];
  const int driver = plan.drivers[pos];
  const bool is_out = plan.edge_src[driver] == vertex;
  const int other = is_out ? plan.edge_dst[driver] : plan.edge_src[driver];
  cursor->anchor = static_cast<std::uint32_t>(embedding[other]);
  cursor->scan_slot = static_cast<std::uint32_t>(plan.edge_slot[driver]);
  if (is_out) cursor->scan_slot = FlipSlot(cursor->scan_slot, num_relations);
  cursor->has_pin = PinValue(job, vertex, &cursor->pin) != 0;
  cursor->pin_done = false;
  if (cursor->has_pin) return;

  const SlotView& slot = slots[cursor->scan_slot];
  cursor->base_count = slot.base_count;
  cursor->base_next =
      LowerBound(slot.base_pairs, slot.base_count, PackPair(cursor->anchor, 0));
  cursor->delta_next = LowerBound(slot.delta_pairs, slot.delta_count,
                                  PackPair(cursor->anchor, 0));
}

__device__ bool NextCandidate(const DeviceMatchJob& job,
                              const SlotView* slots, int pos,
                              CandidateCursor* cursor,
                              std::uint32_t* candidate) {
  const DeviceMatchPlan& plan = job.plan;
  const int vertex = plan.order[pos];
  const int driver = plan.drivers[pos];
  if (cursor->has_pin) {
    if (cursor->pin_done) return false;
    cursor->pin_done = true;
    const bool is_out = plan.edge_src[driver] == vertex;
    const std::uint32_t src = is_out ? cursor->pin : cursor->anchor;
    const std::uint32_t dst = is_out ? cursor->anchor : cursor->pin;
    if (!HasEdge(slots[plan.edge_slot[driver]], src, dst)) return false;
    *candidate = cursor->pin;
    return true;
  }

  const SlotView& slot = slots[cursor->scan_slot];
  while (cursor->base_next < cursor->base_count) {
    const std::uint64_t pair = slot.base_pairs[cursor->base_next++];
    if (static_cast<std::uint32_t>(pair >> 32) != cursor->anchor) break;
    *candidate = static_cast<std::uint32_t>(pair);
    return true;
  }
  while (cursor->delta_next < slot.delta_count) {
    const std::uint64_t pair = slot.delta_pairs[cursor->delta_next++];
    if (static_cast<std::uint32_t>(pair >> 32) != cursor->anchor) break;
    *candidate = static_cast<std::uint32_t>(pair);
    return true;
  }
  return false;
}

__device__ bool BackEdgesHold(const DeviceMatchJob& job,
                              const SlotView* slots, int pos,
                              const std::int32_t* embedding) {
  const DeviceMatchPlan& plan = job.plan;
  const int driver = plan.drivers[pos];
  for (int i = plan.back_offsets[pos]; i < plan.back_offsets[pos + 1]; ++i) {
    const int edge = plan.back_edges[i];
    if (edge == driver) continue;
    const std::uint32_t src =
        static_cast<std::uint32_t>(embedding[plan.edge_src[edge]]);
    const std::uint32_t dst =
        static_cast<std::uint32_t>(embedding[plan.edge_dst[edge]]);
    if (!HasEdge(slots[plan.edge_slot[edge]], src, dst)) return false;
  }
  return true;
}

__device__ void MatchChain2Delta(const DeviceMatchJob& job,
                                 std::uint64_t item) {
  const SlotView& neighbor = job.neighbor;
  std::uint64_t pair = 0;
  if (item < job.base_count) {
    pair = neighbor.base_pairs[job.base_begin + item];
  } else {
    pair = neighbor.delta_pairs[job.delta_begin + item - job.base_count];
  }
  const std::uint32_t candidate = static_cast<std::uint32_t>(pair);
  const int candidate_vertex = job.seed_edge == 0 ? 2 : 0;
  std::uint32_t pin = 0;
  if (PinValue(job, candidate_vertex, &pin) != 0 && candidate != pin) return;
  std::int32_t embedding[3];
  if (job.seed_edge == 0) {
    embedding[0] = static_cast<std::int32_t>(job.seed_src);
    embedding[1] = static_cast<std::int32_t>(job.seed_dst);
    embedding[2] = static_cast<std::int32_t>(candidate);
  } else {
    embedding[0] = static_cast<std::int32_t>(candidate);
    embedding[1] = static_cast<std::int32_t>(job.seed_src);
    embedding[2] = static_cast<std::int32_t>(job.seed_dst);
  }
  EmitEmbedding(job, embedding, job.output);
}

__device__ void MatchSeed(const DeviceMatchJob& job, const SlotView* slots,
                          std::uint32_t num_relations, std::uint64_t item,
                          const DeviceMatchOutput& output) {
  if (job.chain2_delta) {
    MatchChain2Delta(job, item);
    return;
  }
  const DeviceMatchPlan& plan = job.plan;
  const int seed_edge = job.has_seed ? static_cast<int>(job.seed_edge)
                                     : plan.drivers[1];
  const int second = plan.order[1];
  const bool is_out = plan.edge_src[seed_edge] == second;
  std::uint32_t scan_slot =
      static_cast<std::uint32_t>(plan.edge_slot[seed_edge]);
  if (is_out) scan_slot = FlipSlot(scan_slot, num_relations);

  std::uint32_t pin0 = 0;
  std::uint32_t pin1 = 0;
  const bool has_pin0 = PinValue(job, plan.order[0], &pin0) != 0;
  const bool has_pin1 = PinValue(job, second, &pin1) != 0;
  const bool swapped = !has_pin0 && has_pin1;
  if (swapped) scan_slot = FlipSlot(scan_slot, num_relations);

  const SlotView scan = slots[scan_slot];
  std::uint32_t seed_src = job.seed_src;
  std::uint32_t seed_dst = job.seed_dst;
  if (!job.has_seed) {
    std::uint64_t pair = 0;
    if (item < job.base_count) {
      pair = scan.base_pairs[job.base_begin + item];
    } else {
      pair = scan.delta_pairs[job.delta_begin + item - job.base_count];
    }
    const std::uint32_t hi = static_cast<std::uint32_t>(pair >> 32);
    const std::uint32_t lo = static_cast<std::uint32_t>(pair);
    seed_src = swapped ? lo : hi;
    seed_dst = swapped ? hi : lo;
    if ((has_pin0 && seed_src != pin0) || (has_pin1 && seed_dst != pin1)) {
      return;
    }
  }

  std::int32_t embedding[kMaxPatternVertices];
  for (int v = 0; v < plan.n_vertices; ++v) embedding[v] = -1;
  if (job.has_seed) {
    embedding[plan.edge_src[seed_edge]] = static_cast<std::int32_t>(seed_src);
    embedding[plan.edge_dst[seed_edge]] = static_cast<std::int32_t>(seed_dst);
  } else {
    embedding[plan.order[0]] = static_cast<std::int32_t>(seed_src);
    embedding[second] = static_cast<std::int32_t>(seed_dst);
  }

  for (int edge = 0; edge < plan.n_edges; ++edge) {
    if (edge == seed_edge) continue;
    const int src = plan.edge_src[edge];
    const int dst = plan.edge_dst[edge];
    if (embedding[src] < 0 || embedding[dst] < 0) continue;
    if (!HasEdge(slots[plan.edge_slot[edge]],
                 static_cast<std::uint32_t>(embedding[src]),
                 static_cast<std::uint32_t>(embedding[dst]))) {
      return;
    }
  }
  if (plan.n_vertices == 2) {
    EmitEmbedding(job, embedding, output);
    return;
  }

  CandidateCursor cursors[kMaxPatternVertices];
  int pos = 2;
  InitCursor(job, slots, num_relations, pos, embedding, &cursors[pos]);
  while (pos >= 2) {
    if (pos == plan.n_vertices) {
      EmitEmbedding(job, embedding, output);
      --pos;
      continue;
    }
    std::uint32_t candidate = 0;
    bool advanced = false;
    while (NextCandidate(job, slots, pos, &cursors[pos], &candidate)) {
      embedding[plan.order[pos]] = static_cast<std::int32_t>(candidate);
      if (BackEdgesHold(job, slots, pos, embedding)) {
        advanced = true;
        break;
      }
    }
    if (advanced) {
      ++pos;
      if (pos < plan.n_vertices) {
        InitCursor(job, slots, num_relations, pos, embedding, &cursors[pos]);
      }
    } else {
      --pos;
    }
  }
}

__global__ void MatchBatchKernel(const DeviceMatchJob* jobs, int n_jobs,
                                 std::uint64_t total_work,
                                 const SlotView* slots,
                                 std::uint32_t num_relations) {
  const std::uint64_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const std::uint64_t step =
      static_cast<std::uint64_t>(blockDim.x) * gridDim.x;
  for (std::uint64_t work = tid; work < total_work; work += step) {
    int lo = 0;
    int hi = n_jobs;
    while (lo + 1 < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (jobs[mid].work_offset <= work) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    const DeviceMatchJob& job = jobs[lo];
    if (work >= job.work_offset + job.work_count) continue;
    MatchSeed(job, slots, num_relations, work - job.work_offset, job.output);
  }
}

struct HostMatchPlan {
  std::vector<std::int32_t> edge_src;
  std::vector<std::int32_t> edge_dst;
  std::vector<std::int32_t> edge_slot;
  std::vector<std::int32_t> order;
  std::vector<std::int32_t> drivers;
  std::vector<std::int32_t> back_offsets;
  std::vector<std::int32_t> back_edges;
};

HostMatchPlan BuildHostPlan(const gar::core::graph::PatternSpec& spec,
                            const std::optional<std::uint32_t> seed_edge =
                                std::nullopt) {
  const int n = static_cast<int>(spec.num_vertices);
  const int m = static_cast<int>(spec.edges.size());
  HostMatchPlan plan;
  plan.drivers.assign(n, -1);
  plan.back_offsets.assign(n + 1, 0);
  for (const auto& edge : spec.edges) {
    plan.edge_src.push_back(static_cast<std::int32_t>(edge[0]));
    plan.edge_dst.push_back(static_cast<std::int32_t>(edge[1]));
    plan.edge_slot.push_back(static_cast<std::int32_t>(edge[2]));
  }
  if (n < 2 || m == 0) return plan;

  struct AdjacentEdge {
    int edge = 0;
    int other = 0;
  };
  std::vector<std::vector<AdjacentEdge>> adjacency(n);
  for (int e = 0; e < m; ++e) {
    adjacency[plan.edge_src[e]].push_back({e, plan.edge_dst[e]});
    adjacency[plan.edge_dst[e]].push_back({e, plan.edge_src[e]});
  }
  std::vector<char> bound(n, 0);
  if (seed_edge.has_value()) {
    if (*seed_edge >= static_cast<std::uint32_t>(m)) {
      throw std::invalid_argument("GPU AdaDeduce: seed edge out of range");
    }
    const int src = plan.edge_src[*seed_edge];
    const int dst = plan.edge_dst[*seed_edge];
    plan.order.push_back(src);
    plan.order.push_back(dst);
    bound[src] = 1;
    bound[dst] = 1;
  } else {
    bound[0] = 1;
    plan.order.push_back(0);
  }
  while (plan.order.size() < static_cast<std::size_t>(n)) {
    int best = n;
    int best_back = 0;
    for (int v = 0; v < n; ++v) {
      if (bound[v]) continue;
      int back = 0;
      for (const auto& edge : adjacency[v]) back += bound[edge.other];
      if (back == 0) continue;
      if (back > best_back || (back == best_back && v < best)) {
        best = v;
        best_back = back;
      }
    }
    if (best == n) {
      for (int v = 0; v < n; ++v) {
        if (!bound[v]) {
          best = v;
          break;
        }
      }
    }
    bound[best] = 1;
    plan.order.push_back(best);
  }

  std::vector<char> earlier(n, 0);
  earlier[plan.order[0]] = 1;
  for (int pos = 1; pos < n; ++pos) {
    const int vertex = plan.order[pos];
    for (const auto& edge : adjacency[vertex]) {
      if (!earlier[edge.other]) continue;
      if (plan.drivers[pos] < 0) plan.drivers[pos] = edge.edge;
      plan.back_edges.push_back(edge.edge);
    }
    earlier[vertex] = 1;
    plan.back_offsets[pos + 1] =
        static_cast<std::int32_t>(plan.back_edges.size());
  }
  return plan;
}

template <typename T>
void Upload(DeviceOwnedBuffer<T>* device, const std::vector<T>& host,
            cudaStream_t stream) {
  if (host.empty()) return;
  device->Init(Buffer<T>{const_cast<T*>(host.data()),
                         sizeof(T) * host.size()},
               stream);
}

template <typename T>
void EnsureCapacity(DeviceOwnedBuffer<T>* buffer, std::size_t bytes,
                    cudaStream_t stream) {
  if (buffer->GetSize() < bytes) buffer->Init(bytes, stream);
}

struct PreparedPattern {
  gar::core::graph::PatternSpec spec;
  std::optional<std::uint32_t> seed_edge;
  HostMatchPlan host;
  DeviceOwnedBuffer<std::int32_t> edge_src;
  DeviceOwnedBuffer<std::int32_t> edge_dst;
  DeviceOwnedBuffer<std::int32_t> edge_slot;
  DeviceOwnedBuffer<std::int32_t> order;
  DeviceOwnedBuffer<std::int32_t> drivers;
  DeviceOwnedBuffer<std::int32_t> back_offsets;
  DeviceOwnedBuffer<std::int32_t> back_edges;
  DeviceMatchPlan device;
};

class GpuAgentGraph {
 public:
  explicit GpuAgentGraph(const gar::core::graph::Graph& graph)
      : graph_(graph), storage_(graph.num_slots()), views_(graph.num_slots()) {
    CheckCuda(cudaStreamCreate(&stream_), "create stream");
    for (std::uint32_t slot = 0; slot < graph.num_slots(); ++slot) {
      const auto& pairs = graph.RelationPairs(slot);
      storage_[slot].host_base = pairs;
      if (!pairs.empty()) {
        storage_[slot].base_pairs.Init(
            Buffer<std::uint64_t>{const_cast<std::uint64_t*>(pairs.data()),
                                  sizeof(std::uint64_t) * pairs.size()},
            stream_);
      }
      views_[slot].base_pairs = storage_[slot].base_pairs.GetPtr();
      views_[slot].base_count = static_cast<std::uint32_t>(pairs.size());
    }
    CheckCuda(cudaMalloc(&device_views_, sizeof(SlotView) * views_.size()),
              "allocate slot views");
    CheckCuda(cudaMemcpyAsync(device_views_, views_.data(),
                              sizeof(SlotView) * views_.size(),
                              cudaMemcpyHostToDevice, stream_),
              "upload slot views");
    CheckCuda(cudaStreamSynchronize(stream_), "initialize graph");
  }

  GpuAgentGraph(const GpuAgentGraph&) = delete;
  GpuAgentGraph& operator=(const GpuAgentGraph&) = delete;

  ~GpuAgentGraph() {
    if (stream_ != nullptr) cudaStreamSynchronize(stream_);
    if (device_views_ != nullptr) cudaFree(device_views_);
    if (stream_ != nullptr) cudaStreamDestroy(stream_);
  }

  const SlotView* views() const { return device_views_; }
  cudaStream_t stream() const { return stream_; }
  std::uint32_t num_relations() const { return graph_.num_relations(); }
  const gar::core::graph::Graph& host_graph() const { return graph_; }
  SlotView HostView(std::uint32_t slot) const { return views_[slot]; }

  std::pair<std::uint32_t, std::uint32_t> BaseRange(
      std::uint32_t slot, std::optional<std::uint32_t> anchor) const {
    return Range(storage_[slot].host_base, anchor);
  }

  std::pair<std::uint32_t, std::uint32_t> DeltaRange(
      std::uint32_t slot, std::optional<std::uint32_t> anchor) const {
    return Range(storage_[slot].host_delta, anchor);
  }

  void CommitDerived(const gar::core::graph::DerivedEdge& derived) {
    pending_.push_back(DeltaAppend{derived.slot, derived.src, derived.dst});
    const std::uint32_t mirror = graph_.IsInverse(derived.slot)
                                     ? graph_.BaseRelation(derived.slot)
                                     : derived.slot + graph_.num_relations();
    pending_.push_back(DeltaAppend{mirror, derived.dst, derived.src});
    dirty_slots_.insert(derived.slot);
    dirty_slots_.insert(mirror);
  }

  void FlushPending() {
    if (pending_.empty()) return;
    for (const DeltaAppend& append : pending_) {
      storage_[append.slot].host_delta.push_back(PackPair(append.src,
                                                          append.dst));
    }
    for (const std::uint32_t slot : dirty_slots_) {
      SlotStorage& storage = storage_[slot];
      auto& pairs = storage.host_delta;
      std::sort(pairs.begin(), pairs.end());
      pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
      EnsureCapacity(&storage.delta_pairs,
                     sizeof(std::uint64_t) * pairs.size(), stream_);
      CheckCuda(cudaMemcpyAsync(storage.delta_pairs.GetPtr(), pairs.data(),
                                sizeof(std::uint64_t) * pairs.size(),
                                cudaMemcpyHostToDevice, stream_),
                "upload sorted delta");
      views_[slot].delta_pairs = storage.delta_pairs.GetPtr();
      views_[slot].delta_count = static_cast<std::uint32_t>(pairs.size());
      CheckCuda(cudaMemcpyAsync(device_views_ + slot, &views_[slot],
                                sizeof(SlotView), cudaMemcpyHostToDevice,
                                stream_),
                "update slot view");
    }
    pending_.clear();
    dirty_slots_.clear();
  }

 private:
  static std::pair<std::uint32_t, std::uint32_t> Range(
      const std::vector<std::uint64_t>& pairs,
      const std::optional<std::uint32_t> anchor) {
    if (!anchor.has_value()) {
      return {0, static_cast<std::uint32_t>(pairs.size())};
    }
    const auto begin =
        std::lower_bound(pairs.begin(), pairs.end(), PackPair(*anchor, 0));
    const auto end = std::lower_bound(
        pairs.begin(), pairs.end(),
        *anchor == std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint64_t>::max()
            : PackPair(*anchor + 1, 0));
    return {static_cast<std::uint32_t>(begin - pairs.begin()),
            static_cast<std::uint32_t>(end - begin)};
  }

  struct SlotStorage {
    DeviceOwnedBuffer<std::uint64_t> base_pairs;
    DeviceOwnedBuffer<std::uint64_t> delta_pairs;
    std::vector<std::uint64_t> host_base;
    std::vector<std::uint64_t> host_delta;
  };

  const gar::core::graph::Graph& graph_;
  std::vector<SlotStorage> storage_;
  std::vector<SlotView> views_;
  SlotView* device_views_ = nullptr;
  cudaStream_t stream_ = nullptr;
  std::vector<DeltaAppend> pending_;
  std::unordered_set<std::uint32_t> dirty_slots_;
};

struct JobWorkspace {
  DeviceOwnedBuffer<std::uint32_t> embeddings;
};

class GpuMatchBackend final : public MatchBackend {
 public:
  explicit GpuMatchBackend(const gar::core::graph::Graph& graph)
      : graph_(graph) {}

  ~GpuMatchBackend() override {
    if (host_results_ != nullptr) cudaFreeHost(host_results_);
  }

  std::vector<MatchResult> RunBatch(
      const std::vector<MatchRequest>& requests) override {
    graph_.FlushPending();
    std::vector<MatchResult> results(requests.size());
    std::size_t begin = 0;
    std::uint64_t chunk_values = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const std::uint64_t values =
          requests[i].cap * requests[i].spec.num_vertices;
      if (i > begin && chunk_values + values > kMaxChunkOutputValues) {
        RunChunk(requests, begin, i, &results);
        begin = i;
        chunk_values = 0;
      }
      chunk_values += values;
    }
    if (begin < requests.size()) {
      RunChunk(requests, begin, requests.size(), &results);
    }
    return results;
  }

  void CommitDerived(
      const gar::core::graph::DerivedEdge& derived) override {
    graph_.CommitDerived(derived);
  }

 private:
  const PreparedPattern& PreparePattern(
      const gar::core::graph::PatternSpec& spec,
      const std::optional<std::uint32_t> seed_edge = std::nullopt) {
    for (const auto& pattern : patterns_) {
      if (pattern->spec.num_vertices == spec.num_vertices &&
          pattern->spec.edges == spec.edges &&
          pattern->seed_edge == seed_edge) {
        return *pattern;
      }
    }
    if (spec.num_vertices > kMaxPatternVertices) {
      throw std::invalid_argument("GPU AdaDeduce supports at most 16 pattern "
                                  "vertices");
    }
    patterns_.push_back(std::make_unique<PreparedPattern>());
    PreparedPattern& pattern = *patterns_.back();
    pattern.spec = spec;
    pattern.seed_edge = seed_edge;
    pattern.host = BuildHostPlan(spec, seed_edge);
    Upload(&pattern.edge_src, pattern.host.edge_src, graph_.stream());
    Upload(&pattern.edge_dst, pattern.host.edge_dst, graph_.stream());
    Upload(&pattern.edge_slot, pattern.host.edge_slot, graph_.stream());
    Upload(&pattern.order, pattern.host.order, graph_.stream());
    Upload(&pattern.drivers, pattern.host.drivers, graph_.stream());
    Upload(&pattern.back_offsets, pattern.host.back_offsets, graph_.stream());
    Upload(&pattern.back_edges, pattern.host.back_edges, graph_.stream());
    pattern.device.edge_src = pattern.edge_src.GetPtr();
    pattern.device.edge_dst = pattern.edge_dst.GetPtr();
    pattern.device.edge_slot = pattern.edge_slot.GetPtr();
    pattern.device.order = pattern.order.GetPtr();
    pattern.device.drivers = pattern.drivers.GetPtr();
    pattern.device.back_offsets = pattern.back_offsets.GetPtr();
    pattern.device.back_edges = pattern.back_edges.GetPtr();
    pattern.device.n_vertices = static_cast<int>(spec.num_vertices);
    pattern.device.n_edges = static_cast<int>(spec.edges.size());
    return pattern;
  }

  void LaunchMatchJobs(std::size_t n_jobs, std::uint64_t total_work) {
    if (total_work == 0) return;
    const std::uint64_t blocks =
        std::min<std::uint64_t>(256, (total_work + 127) / 128);
    MatchBatchKernel<<<static_cast<int>(blocks), 128, 0, graph_.stream()>>>(
        jobs_buffer_.GetPtr(), static_cast<int>(n_jobs), total_work,
        graph_.views(), graph_.num_relations());
    CheckCuda(cudaGetLastError(), "launch match batch");
  }

  void RunChunk(const std::vector<MatchRequest>& requests, std::size_t begin,
                std::size_t end, std::vector<MatchResult>* results) {
    const std::size_t n_jobs = end - begin;
    EnsureCapacity(&produced_buffer_, sizeof(std::uint64_t) * n_jobs,
                   graph_.stream());
    CheckCuda(cudaMemsetAsync(produced_buffer_.GetPtr(), 0,
                              sizeof(std::uint64_t) * n_jobs, graph_.stream()),
              "clear produced counts");
    std::vector<DeviceMatchJob> jobs;
    jobs.reserve(n_jobs);
    std::uint64_t total_work = 0;
    for (std::size_t i = begin; i < end; ++i) {
      const MatchRequest& request = requests[i];
      DeviceMatchJob job = BuildJob(request);
      JobWorkspace& workspace = Workspace(i - begin);
      const std::uint64_t values = request.cap * request.spec.num_vertices;
      EnsureCapacity(&workspace.embeddings,
                     sizeof(std::uint32_t) * values, graph_.stream());
      job.output.embeddings = workspace.embeddings.GetPtr();
      job.output.produced = produced_buffer_.GetPtr() + i - begin;
      job.output.capacity = request.cap;
      job.work_offset = total_work;
      total_work += job.work_count;
      jobs.push_back(job);
    }
    EnsureCapacity(&jobs_buffer_, sizeof(DeviceMatchJob) * jobs.size(),
                   graph_.stream());
    CheckCuda(cudaMemcpyAsync(jobs_buffer_.GetPtr(), jobs.data(),
                              sizeof(DeviceMatchJob) * jobs.size(),
                              cudaMemcpyHostToDevice, graph_.stream()),
              "upload match jobs");
    LaunchMatchJobs(jobs.size(), total_work);
    ReadResults(requests, begin, end, results);
    if (std::getenv("GAR_DEBUG_GPU_COMPARE") != nullptr) {
      ValidateChunk(requests, begin, end, *results);
    }
  }

  DeviceMatchJob BuildJob(const MatchRequest& request) {
    DeviceMatchJob job;
    const std::optional<std::uint32_t> seed_edge_id =
        request.seed.has_value()
            ? std::optional<std::uint32_t>(request.seed->edge)
            : std::nullopt;
    const PreparedPattern& prepared = PreparePattern(request.spec,
                                                     seed_edge_id);
    job.plan = prepared.device;
    job.capacity = request.cap;
    job.n_pins = static_cast<int>(request.pins.size());
    int pin = 0;
    for (const auto& [vertex, value] : request.pins) {
      job.pin_vertices[pin] = static_cast<std::int32_t>(vertex);
      job.pin_values[pin] = value;
      ++pin;
    }
    if (job.plan.n_vertices < 2 || job.plan.n_edges == 0) return job;
    for (int pos = 1; pos < job.plan.n_vertices; ++pos) {
      if (prepared.host.drivers[pos] < 0) return job;
    }
    if (request.seed.has_value()) {
      job.has_seed = true;
      job.seed_src = request.seed->src;
      job.seed_dst = request.seed->dst;
      job.seed_edge = request.seed->edge;
      const bool chain2 =
          prepared.host.order.size() == 3 &&
          prepared.host.edge_src.size() == 2 &&
          prepared.host.edge_src[0] == 0 && prepared.host.edge_dst[0] == 1 &&
          prepared.host.edge_src[1] == 1 && prepared.host.edge_dst[1] == 2 &&
          job.seed_edge < 2;
      if (chain2) {
        job.chain2_delta = true;
        const std::uint32_t neighbor_slot =
            job.seed_edge == 0
                ? static_cast<std::uint32_t>(prepared.host.edge_slot[1])
                : FlipSlotHost(static_cast<std::uint32_t>(
                      prepared.host.edge_slot[0]));
        const std::uint32_t anchor =
            job.seed_edge == 0 ? job.seed_dst : job.seed_src;
        job.neighbor = graph_.HostView(neighbor_slot);
        const auto base = graph_.BaseRange(neighbor_slot, anchor);
        const auto delta = graph_.DeltaRange(neighbor_slot, anchor);
        job.base_begin = base.first;
        job.base_count = base.second;
        job.delta_begin = delta.first;
        job.delta_count = delta.second;
        job.work_count = job.base_count + job.delta_count;
      } else {
        job.work_count = 1;
      }
      return job;
    }

    const int seed_edge = prepared.host.drivers[1];
    const bool is_out =
        prepared.host.edge_src[seed_edge] == prepared.host.order[1];
    std::uint32_t scan_slot =
        static_cast<std::uint32_t>(prepared.host.edge_slot[seed_edge]);
    if (is_out) scan_slot = FlipSlotHost(scan_slot);
    std::optional<std::uint32_t> pin0;
    std::optional<std::uint32_t> pin1;
    for (const auto& [vertex, value] : request.pins) {
      if (vertex == prepared.host.order[0]) pin0 = value;
      if (vertex == prepared.host.order[1]) pin1 = value;
    }
    const bool swapped = !pin0.has_value() && pin1.has_value();
    if (swapped) scan_slot = FlipSlotHost(scan_slot);
    std::optional<std::uint32_t> anchor;
    if (pin0.has_value()) {
      anchor = *pin0;
    } else if (swapped) {
      anchor = *pin1;
    }
    const auto base = graph_.BaseRange(scan_slot, anchor);
    const auto delta = graph_.DeltaRange(scan_slot, anchor);
    job.base_begin = base.first;
    job.base_count = base.second;
    job.delta_begin = delta.first;
    job.delta_count = delta.second;
    job.work_count = job.base_count + job.delta_count;
    return job;
  }

  std::uint32_t FlipSlotHost(std::uint32_t slot) const {
    const std::uint32_t n = graph_.num_relations();
    return slot < n ? slot + n : slot - n;
  }

  JobWorkspace& Workspace(std::size_t index) {
    while (workspaces_.size() <= index) {
      workspaces_.push_back(std::make_unique<JobWorkspace>());
    }
    return *workspaces_[index];
  }

  void EnsureHostResultCapacity(std::uint64_t values) {
    if (host_results_capacity_ >= values) return;
    const std::uint64_t capacity =
        std::max(values, host_results_capacity_ * 2);
    if (host_results_ != nullptr) {
      CheckCuda(cudaFreeHost(host_results_), "free match result staging");
    }
    host_results_ = nullptr;
    host_results_capacity_ = 0;
    CheckCuda(cudaHostAlloc(reinterpret_cast<void**>(&host_results_),
                            sizeof(std::uint32_t) * capacity,
                            cudaHostAllocDefault),
              "allocate match result staging");
    host_results_capacity_ = capacity;
  }

  void ReadResults(const std::vector<MatchRequest>& requests,
                   std::size_t begin, std::size_t end,
                   std::vector<MatchResult>* results) {
    const std::size_t n_jobs = end - begin;
    std::vector<std::uint64_t> produced(n_jobs);
    CheckCuda(cudaMemcpyAsync(produced.data(), produced_buffer_.GetPtr(),
                              sizeof(std::uint64_t) * n_jobs,
                              cudaMemcpyDeviceToHost, graph_.stream()),
              "copy match counts");
    CheckCuda(cudaStreamSynchronize(graph_.stream()), "read match counts");

    std::vector<std::uint64_t> offsets(n_jobs + 1, 0);
    for (std::size_t j = 0; j < n_jobs; ++j) {
      const MatchRequest& request = requests[begin + j];
      MatchResult& result = results->at(begin + j);
      result.capped = produced[j] >= request.cap && produced[j] > 0;
      const std::uint64_t count = std::min(produced[j], request.cap);
      offsets[j + 1] = offsets[j] + count * request.spec.num_vertices;
    }

    if (offsets.back() == 0) return;
    EnsureHostResultCapacity(offsets.back());
    for (std::size_t j = 0; j < n_jobs; ++j) {
      if (offsets[j] == offsets[j + 1]) continue;
      const JobWorkspace& workspace = Workspace(j);
      CheckCuda(cudaMemcpyAsync(host_results_ + offsets[j],
                                workspace.embeddings.GetPtr(),
                                sizeof(std::uint32_t) * (offsets[j + 1] -
                                                         offsets[j]),
                                cudaMemcpyDeviceToHost, graph_.stream()),
                "copy match result");
    }
    CheckCuda(cudaStreamSynchronize(graph_.stream()), "read match results");
    for (std::size_t j = 0; j < n_jobs; ++j) {
      MatchResult& result = results->at(begin + j);
      if (offsets[j] == offsets[j + 1]) {
        result.embeddings.clear();
        continue;
      }
      result.embeddings.assign(host_results_ + offsets[j],
                               host_results_ + offsets[j + 1]);
    }
  }

  void ValidateChunk(const std::vector<MatchRequest>& requests,
                     std::size_t begin, std::size_t end,
                     const std::vector<MatchResult>& results) {
    const std::vector<MatchRequest> slice(requests.begin() + begin,
                                          requests.begin() + end);
    std::unique_ptr<MatchBackend> cpu =
        MakeMatchBackend("cpu", graph_.host_graph());
    const std::vector<MatchResult> expected = cpu->RunBatch(slice);
    for (std::size_t j = 0; j < expected.size(); ++j) {
      const int n = static_cast<int>(slice[j].spec.num_vertices);
      auto rows = [n](const MatchResult& result) {
        std::vector<std::vector<std::uint32_t>> out;
        for (std::size_t i = 0; i < result.embeddings.size(); i += n) {
          out.emplace_back(result.embeddings.begin() + i,
                           result.embeddings.begin() + i + n);
        }
        std::sort(out.begin(), out.end());
        return out;
      };
      if (rows(results[begin + j]) != rows(expected[j])) {
        throw std::runtime_error("GPU match differs from CPU oracle");
      }
    }
  }

  GpuAgentGraph graph_;
  std::vector<std::unique_ptr<PreparedPattern>> patterns_;
  std::vector<std::unique_ptr<JobWorkspace>> workspaces_;
  DeviceOwnedBuffer<DeviceMatchJob> jobs_buffer_;
  DeviceOwnedBuffer<std::uint64_t> produced_buffer_;
  std::uint32_t* host_results_ = nullptr;
  std::uint64_t host_results_capacity_ = 0;
};

}  // namespace

std::unique_ptr<MatchBackend> CreateGpuMatchBackend(
    const gar::core::graph::Graph& graph) {
  return std::make_unique<GpuMatchBackend>(graph);
}

}  // namespace gar::core::engine::agent
