cmake_minimum_required(VERSION 3.20)
project(gar_graph LANGUAGES CXX)

#######################
# gar_graph: graph & semantics kernel (triple store, pattern matching,
# predicate evaluation, fact merging) — no MatrixGraph dependency
#######################
add_library(gar_graph STATIC
    src/triple_graph.cpp
    src/pattern_matcher.cpp
    src/predicate_evaluator.cpp
    src/fact_merger.cpp
)
add_library(gar::core::graph ALIAS gar_graph)

target_include_directories(gar_graph PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_graph PUBLIC gar::core::rule)
target_compile_features(gar_graph PUBLIC cxx_std_20)

# oneTBB (bundled with MatrixGraph) for parallel edge-absence marking;
# without it the marker falls back to a serial loop.
if (TARGET tbb)
    target_link_libraries(gar_graph PUBLIC tbb)
    target_compile_definitions(gar_graph PRIVATE GAR_GRAPH_HAS_TBB)
endif ()
