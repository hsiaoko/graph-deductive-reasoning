cmake_minimum_required(VERSION 3.20)
project(gar_engine_agent LANGUAGES CXX)

#######################
# gar_engine_agent: policy-guided deduction (agent engine).
# Layering: gar::core::graph + gar::core::analysis + gar::core::hdc + gar::core::rule ONLY —
# the sibling deduction engine is never visible here (the closure
# cross-check is composed at the app layer).
#######################
add_library(gar_engine_agent STATIC
    src/motif.cpp
    src/fact_encoder.cpp
    src/witness.cpp
    src/scorer.cpp
    src/learning/policy_trace.cpp
    src/learning/proof_dataset.cpp
    src/learning/relevance_model.cpp
    src/learning/relevance_trainer.cpp
    src/learning/learned_policy.cpp
    src/ada_deduce_executor.cpp
    src/backward_executor.cpp
    src/hybrid_executor.cpp
    src/learned_strategy_executor.cpp
    src/batch_session.cpp
    src/match_backend.cpp
    src/cpu_match_backend.cpp
    src/gpu_match_backend.cu
    src/deduce_session.cpp
    src/partial_matcher.cpp
    src/working_set.cpp
    src/precondition_cache.cpp
    src/producer_tracker.cpp
    src/guide.cpp
    src/guided_policy.cpp
    src/jev_client.cpp
    src/policy.cpp
    src/value_model.cpp
)
add_library(gar::core::engine::agent ALIAS gar_engine_agent)

target_include_directories(gar_engine_agent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_engine_agent PUBLIC
    gar::core::graph gar::core::analysis gar::core::hdc gar::core::rule
)
target_compile_features(gar_engine_agent PUBLIC cxx_std_20)

# Vendored JSON for the HTTP model clients: nlohmann/json v3.11.2, MIT.
# PRIVATE — it appears in no public header (LlmClient is JSON-free).
target_include_directories(gar_engine_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
)

# MatrixGraph supplies HomMatch and generic CUDA buffers. Agent-specific
# dynamic GPU matching remains in this target.
find_package(CUDAToolkit REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(gar_engine_agent PRIVATE
    matrixgraph_core CUDA::cudart CURL::libcurl
)
