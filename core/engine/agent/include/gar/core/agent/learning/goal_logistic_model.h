#pragma once

// Goal-level logistic model shared by goal-conditioned selectors (the learned
// strategy router and the learned star-order selector). It scores a goal from
// two structural features -- dependency-cone rule count and log1p(predicate
// base degree) -- and thresholds the probability into a binary decision. The
// meaning of the decision (route to backward, or pick input star order) is
// owned by the caller; this header only loads parameters and evaluates the
// logistic, so both selectors share one format and one implementation.

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gar::core::engine::agent::learning {

struct GoalLogisticModel {
  double w0 = 0.0;  // cone_size coefficient
  double w1 = 0.0;  // log1p(predicate degree) coefficient
  double b = 0.0;
  double threshold = 0.5;

  // `magic` is the format tag the caller expects (e.g. "gar_strategy_v1" or
  // "gar_star_order_v1"); a mismatch is a hard error so a model trained for one
  // decision is never silently reused for the other.
  static GoalLogisticModel Load(const std::string& path,
                                const std::string& magic) {
    std::ifstream in(path);
    if (!in) throw std::invalid_argument("cannot read goal model: " + path);
    std::string line;
    std::getline(in, line);
    if (line != magic)
      throw std::invalid_argument("bad goal model format: " + path);
    GoalLogisticModel m;
    while (std::getline(in, line)) {
      std::istringstream ss(line);
      std::string key;
      ss >> key;
      if (key == "w0") ss >> m.w0;
      else if (key == "w1") ss >> m.w1;
      else if (key == "b") ss >> m.b;
      else if (key == "threshold") ss >> m.threshold;
    }
    return m;
  }

  double Probability(double cone_size, double degree) const {
    const double z = w0 * cone_size + w1 * std::log1p(degree) + b;
    return 1.0 / (1.0 + std::exp(-z));
  }
};

}  // namespace gar::core::engine::agent::learning