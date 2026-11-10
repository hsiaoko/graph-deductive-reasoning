cmake_minimum_required(VERSION 3.20)
project(gar_core LANGUAGES CXX)

#######################
# gar_rule: rule representation (flat edge arrays + YAML attributes)
#######################
add_library(gar_rule STATIC
    src/csr.cpp
    src/predicate.cpp
    src/rule.cpp
    src/rule_loader.cpp
    src/rule_store.cpp
)
add_library(gar::core::rule ALIAS gar_rule)

target_include_directories(gar_rule PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_rule PUBLIC yaml-cpp)
target_compile_features(gar_rule PUBLIC cxx_std_20)
