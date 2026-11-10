cmake_minimum_required(VERSION 3.20)

project(garagent VERSION 0.1.0 LANGUAGES CXX)

#######################
# Options
#######################
option(GAR_ENABLE_MATRIXGRAPH "Enable MatrixGraph (CUDA) integration via third_party/MatrixGraph" ON)
option(GAR_BUILD_EXAMPLES "Build examples" ON)

#######################
# Build Configuration
#######################
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif ()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

message(STATUS "[GARAgent] Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "[GARAgent] MatrixGraph integration: ${GAR_ENABLE_MATRIXGRAPH}")

#######################
# Third Party: MatrixGraph
#######################
# MatrixGraph (https://github.com/hsiaoko/MatrixGraph) is vendored under
# third_party/MatrixGraph. It bundles yaml-cpp / oneTBB / gflags and exports
# the `matrixgraph_core` target. EXCLUDE_FROM_ALL keeps its apps/tools out of
# the default build; targets are compiled only when something links them.
set(MATRIXGRAPH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/MatrixGraph)

if (GAR_ENABLE_MATRIXGRAPH)
    if (NOT EXISTS ${MATRIXGRAPH_ROOT}/CMakeLists.txt)
        message(FATAL_ERROR
            "third_party/MatrixGraph is missing. Clone it with:\n"
            "  git clone https://github.com/hsiaoko/MatrixGraph third_party/MatrixGraph\n"
            "  (cd third_party/MatrixGraph && git submodule update --init)\n"
            "Or configure with -DGAR_ENABLE_MATRIXGRAPH=OFF.")
    endif ()
    enable_language(CUDA)
    if (NOT DEFINED CUDA_ARCHITECTURES)
        set(CUDA_ARCHITECTURES "sm_70")
    endif ()
    add_subdirectory(${MATRIXGRAPH_ROOT} EXCLUDE_FROM_ALL)
else ()
    # Minimal fallback: only yaml-cpp (bundled inside MatrixGraph's tree).
    add_subdirectory(${MATRIXGRAPH_ROOT}/third_party/yaml-cpp EXCLUDE_FROM_ALL)
endif ()

#######################
# Project Components
#######################
# Everything (except third_party) lives under core/ (the gar::core library
# tree) and tools/ (executables and scripts). See docs/refactor_plan.md.
add_subdirectory(core)
add_subdirectory(core/hdc)
add_subdirectory(core/graph)
add_subdirectory(core/analysis)
if (GAR_ENABLE_MATRIXGRAPH)
    add_subdirectory(core/bridge)
endif ()
add_subdirectory(core/engine)
add_subdirectory(tools/miner)
add_subdirectory(tools/cli)

if (GAR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
