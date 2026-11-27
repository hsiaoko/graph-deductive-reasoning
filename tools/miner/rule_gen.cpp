#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "gar/core/graph/graph_loader.h"
#include "gar/core/rule_store.h"
#include "gar/miner/rule_generator.h"

namespace {

struct Args {
  std::string graph_dir;
  std::string csr_dir;
  std::string out_root;
  std::size_t print_n = 20;
  bool with_inverses = true;
  gar::tools::miner::RuleGeneratorConfig cfg;
};

void Usage() {
  std::cerr << "usage: rule_gen (--graph <triple_graph_dir> | --csr "
               "<immutable_csr_dir>) --out <rules_root_dir> [--count N] "
               "[--min-supp N] [--print N] [--dataset ROOT_PATH] "
               "[--no-inverses]\n";
}

std::uint64_t ParseUnsigned(const std::string& value,
                            const std::string& option) {
  std::uint64_t parsed = 0;
  const auto [ptr, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || error != std::errc() ||
      ptr != value.data() + value.size()) {
    std::cerr << "invalid unsigned integer for " << option << ": " << value
              << "\n";
    std::exit(2);
  }
  return parsed;
}

std::size_t ParseSize(const std::string& value, const std::string& option) {
  const std::uint64_t parsed = ParseUnsigned(value, option);
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    std::cerr << "value too large for " << option << ": " << value << "\n";
    std::exit(2);
  }
  return static_cast<std::size_t>(parsed);
}

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << key << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (key == "--graph") {
      args.graph_dir = next();
    } else if (key == "--csr") {
      args.csr_dir = next();
    } else if (key == "--out") {
      args.out_root = next();
    } else if (key == "--count") {
      args.cfg.count = ParseSize(next(), key);
    } else if (key == "--min-supp") {
      args.cfg.min_support = ParseUnsigned(next(), key);
    } else if (key == "--print") {
      args.print_n = ParseSize(next(), key);
    } else if (key == "--dataset") {
      args.cfg.dataset_root = next();
    } else if (key == "--no-inverses") {
      args.with_inverses = false;
    } else {
      std::cerr << "unknown option: " << key << "\n";
      Usage();
      std::exit(2);
    }
  }
  if (args.graph_dir.empty() == args.csr_dir.empty() || args.out_root.empty() ||
      args.cfg.count == 0) {
    Usage();
    std::exit(2);
  }
  if (args.cfg.dataset_root.empty()) {
    args.cfg.dataset_root =
        args.graph_dir.empty() ? args.csr_dir : args.graph_dir;
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);
  try {
    const std::filesystem::path out_root(args.out_root);
    if (std::filesystem::exists(out_root) &&
        (!std::filesystem::is_directory(out_root) ||
         std::filesystem::directory_iterator(out_root) !=
             std::filesystem::directory_iterator())) {
      throw std::runtime_error("output directory is nonempty: " +
                               out_root.string());
    }

    gar::core::graph::Graph graph =
        args.graph_dir.empty()
            ? gar::core::graph::LoadGraph(args.csr_dir, args.with_inverses)
            : gar::core::graph::Graph::Load(args.graph_dir, args.with_inverses);
    std::cerr << "[rule_gen] |V|=" << graph.num_entities()
              << " |R|=" << graph.num_relations()
              << " |E|=" << graph.num_edges() << "\n";

    gar::tools::miner::RuleGenerator generator(graph, args.cfg);
    auto candidates = generator.Generate();
    const auto& stats = generator.stats();
    std::cerr << "[rule_gen] body pairs evaluated: "
              << stats.body_pairs_evaluated
              << ", candidates scored: " << stats.candidates_scored
              << ", rules emitted: " << stats.rules_emitted << "\n";
    if (candidates.size() < args.cfg.count) {
      std::cerr << "[rule_gen] requested " << args.cfg.count
                << " rules but only " << candidates.size()
                << " satisfied the support and novelty gates\n";
    }

    std::vector<gar::GARule> rules;
    rules.reserve(candidates.size());
    for (auto& candidate : candidates) {
      rules.push_back(std::move(candidate.rule));
    }
    gar::RuleStore::SaveDir(rules, out_root);
    std::cerr << "[rule_gen] wrote " << rules.size() << " rule dirs to "
              << out_root << "\n";

    std::cout << "===== candidate link GARs (" << args.print_n << " of "
              << rules.size() << ") =====\n";
    for (std::size_t i = 0; i < rules.size() && i < args.print_n; ++i) {
      const auto& candidate = candidates[i];
      std::cout << "supp=" << candidate.support
                << " novel=" << candidate.novel_count
                << " conf=" << candidate.confidence << "  " << rules[i].name
                << "\n";
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
