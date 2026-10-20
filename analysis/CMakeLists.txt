cmake_minimum_required(VERSION 3.20)
project(gar_analysis LANGUAGES CXX)

#######################
# gar_analysis: L2 analysis services — predicate table, dependency graph
# (BuildDG + SCC + serialization), rule-set statistics, goal sampling.
# Layering discipline: depends on gar::core + gar::graph ONLY; never on
# engine/, miner/, hdc/ or MatrixGraph.
#######################
add_library(gar_analysis STATIC
    src/predicate_table.cpp
    src/dependency_graph.cpp
    src/rule_stats.cpp
    src/goal_sampler.cpp
)
add_library(gar::analysis ALIAS gar_analysis)

target_include_directories(gar_analysis PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_analysis PUBLIC gar::core gar::graph)
target_compile_features(gar_analysis PUBLIC cxx_std_20)
