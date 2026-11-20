#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gar/core/graph/graph.h"
#include "gar/core/graph/attributes.h"
#include "gar/core/rule.h"

namespace gar::core::graph {

struct MlQuery {
  std::vector<std::uint32_t> entities;
  std::string label;
};

using MlModel = std::function<std::vector<double>(
    const std::vector<MlQuery>& queries)>;

class MlModelRegistry {
 public:
  void Register(std::string name, MlModel model);
  [[nodiscard]] const MlModel* Find(const std::string& name) const;

 private:
  std::unordered_map<std::string, MlModel> models_;
};

// Evaluates a rule's antecedent conjunction against one embedding. ML calls
// are grouped by registered model. A missing model or malformed response is an
// error, never a false predicate or a skipped rule.
bool EvalAntecedent(const std::vector<Predicate>& antecedent,
                    const std::uint32_t* emb, const VertexAttributes* attrs,
                    const Graph& g, const MlModelRegistry* models = nullptr);

}  // namespace gar::core::graph
