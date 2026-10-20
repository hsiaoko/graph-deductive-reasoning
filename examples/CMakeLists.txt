cmake_minimum_required(VERSION 3.20)
project(gar_examples LANGUAGES CXX)

add_executable(load_rule load_rule.cpp)
target_link_libraries(load_rule PRIVATE gar::core)

add_executable(rule_set_info rule_set_info.cpp)
target_link_libraries(rule_set_info PRIVATE gar::core)

if (GAR_ENABLE_MATRIXGRAPH)
    add_executable(matrixgraph_bridge matrixgraph_bridge.cpp)
    target_link_libraries(matrixgraph_bridge PRIVATE gar::matrixgraph)
endif ()
