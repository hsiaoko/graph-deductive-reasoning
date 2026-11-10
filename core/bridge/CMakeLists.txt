cmake_minimum_required(VERSION 3.20)
project(gar_bridge LANGUAGES CXX)

#######################
# gar_bridge: MatrixGraph adapter — ImmutableCSR/attribute loading and
# rule→pattern conversion (with engine/deduction's GPU engine, one of the
# two sanctioned MatrixGraph contact points)
#######################
add_library(gar_bridge STATIC
    src/graph_loader.cpp
    src/pattern_bridge.cpp
)
add_library(gar::core::bridge ALIAS gar_bridge)

target_include_directories(gar_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_bridge PUBLIC gar::core::rule gar::core::graph matrixgraph_core)
target_compile_features(gar_bridge PUBLIC cxx_std_20)
