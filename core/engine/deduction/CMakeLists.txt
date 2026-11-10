cmake_minimum_required(VERSION 3.20)
project(gar_engine_deduction LANGUAGES CXX)

#######################
# gar_engine_deduction: deduction engine family (full-closure chase).
# Levelized on gar::core::graph + gar::core::rule; the GPU engine additionally touches
# MatrixGraph — the sanctioned contact point inside this module.
#######################
add_library(gar_engine_deduction STATIC
    src/cpu_chase_engine.cpp
)
add_library(gar::core::engine::chase ALIAS gar_engine_deduction)

target_include_directories(gar_engine_deduction PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_engine_deduction PUBLIC gar::core::graph gar::core::rule)
target_compile_features(gar_engine_deduction PUBLIC cxx_std_20)

# oneTBB (bundled with MatrixGraph) for rule-level parallelism in the chase;
# without it the engine falls back to a serial round loop.
if (TARGET tbb)
    target_link_libraries(gar_engine_deduction PUBLIC tbb)
    target_compile_definitions(gar_engine_deduction PRIVATE GAR_ENGINE_DEDUCTION_HAS_TBB)
endif ()

if (GAR_ENABLE_MATRIXGRAPH)
    # Host-only TU: gar_deduce.cuh declares __host__ APIs (kernels live in
    # matrixgraph_core), so plain g++ suffices; DeviceOwnedBuffer's inline
    # cudaMalloc/cudaFree make this TU need cudart at link time.
    target_sources(gar_engine_deduction PRIVATE src/gpu_deduce_engine.cpp)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(gar_engine_deduction PRIVATE
        matrixgraph_core CUDA::cudart
    )
endif ()
