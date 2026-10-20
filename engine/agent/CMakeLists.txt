cmake_minimum_required(VERSION 3.20)
project(gar_engine_agent LANGUAGES CXX)

#######################
# gar_engine_agent: policy-guided deduction (agent engine).
# Layering: gar::graph + gar::analysis + gar::hdc + gar::core ONLY —
# the sibling deduction engine is never visible here (the closure
# cross-check is composed at the app layer).
#######################
add_library(gar_engine_agent STATIC
    src/motif.cpp
    src/witness.cpp
    src/scorer.cpp
    src/motif_generator.cpp
    src/executor.cpp
)
add_library(gar::engine::agent ALIAS gar_engine_agent)

target_include_directories(gar_engine_agent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_engine_agent PUBLIC
    gar::graph gar::analysis gar::hdc gar::core
)
target_compile_features(gar_engine_agent PUBLIC cxx_std_20)
