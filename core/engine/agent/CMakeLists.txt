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
    src/ada_deduce_executor.cpp
    src/deduce_session.cpp
    src/partial_matcher.cpp
    src/working_set.cpp
    src/precondition_cache.cpp
    src/producer_tracker.cpp
    src/jev_client.cpp
    src/policy.cpp
    src/value_model.cpp
)
add_library(gar::engine::agent ALIAS gar_engine_agent)

target_include_directories(gar_engine_agent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_engine_agent PUBLIC
    gar::graph gar::analysis gar::hdc gar::core
)
target_compile_features(gar_engine_agent PUBLIC cxx_std_20)

# Vendored JSON for the Jev HTTP client: nlohmann/json v3.11.2, MIT,
# official release layout, copied into third_party/nlohmann (offline
# environment; identical to the single_include distribution). PRIVATE —
# it appears in no public header (JevClient's interface is JSON-free).
target_include_directories(gar_engine_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
)

# MatrixGraph HomMatch (CPU homomorphism matcher): the agent's matching
# primitive. Host-only header + pure-host symbols inside matrixgraph_core.
if (GAR_ENABLE_MATRIXGRAPH)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(gar_engine_agent PRIVATE
        matrixgraph_core CUDA::cudart
    )
endif ()
