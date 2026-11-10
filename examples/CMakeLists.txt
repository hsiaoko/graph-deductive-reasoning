cmake_minimum_required(VERSION 3.20)
project(gar_examples LANGUAGES CXX)

add_executable(load_rule load_rule.cpp)
target_link_libraries(load_rule PRIVATE gar::core::rule)

add_executable(rule_set_info rule_set_info.cpp)
target_link_libraries(rule_set_info PRIVATE gar::core::rule)

add_executable(value_model_check value_model_check.cpp)
target_link_libraries(value_model_check PRIVATE gar::core::engine::agent)

add_executable(value_model_bench value_model_bench.cpp)
target_link_libraries(value_model_bench PRIVATE gar::core::engine::agent)

if (GAR_ENABLE_MATRIXGRAPH)
    add_executable(matrixgraph_bridge matrixgraph_bridge.cpp)
    target_link_libraries(matrixgraph_bridge PRIVATE gar::core::bridge)
endif ()
