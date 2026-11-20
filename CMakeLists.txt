cmake_minimum_required(VERSION 3.20)

project(garagent VERSION 0.1.0 LANGUAGES CXX)

#######################
# Options
#######################
option(GAR_BUILD_EXAMPLES "Build examples" ON)

#######################
# Build Configuration
#######################
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")
endif ()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

message(STATUS "[GARAgent] Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "[GARAgent] MatrixGraph integration: required")

#######################
# Third Party: MatrixGraph
#######################
# MatrixGraph (https://github.com/hsiaoko/MatrixGraph) is vendored under
# third_party/MatrixGraph. It bundles yaml-cpp / oneTBB / gflags and exports
# the `matrixgraph_core` target. EXCLUDE_FROM_ALL keeps its apps/tools out of
# the default build; targets are compiled only when something links them.
set(MATRIXGRAPH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/MatrixGraph)
if (NOT EXISTS ${MATRIXGRAPH_ROOT}/CMakeLists.txt)
    message(FATAL_ERROR
        "third_party/MatrixGraph is missing. Clone it with:\n"
        "  git clone https://github.com/hsiaoko/MatrixGraph third_party/MatrixGraph\n"
        "  (cd third_party/MatrixGraph && git submodule update --init)")
endif ()
enable_language(CUDA)
if (NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "70" CACHE STRING
        "CUDA architectures to compile for")
endif ()

# MatrixGraph currently forces BUILD_TESTING off in the cache. Restore the
# caller's setting after adding it so the subproject does not own root policy.
set(_gar_build_testing_defined FALSE)
if (DEFINED BUILD_TESTING)
    set(_gar_build_testing_defined TRUE)
    set(_gar_build_testing "${BUILD_TESTING}")
endif ()
add_subdirectory(${MATRIXGRAPH_ROOT} EXCLUDE_FROM_ALL)
if (_gar_build_testing_defined)
    set(BUILD_TESTING "${_gar_build_testing}" CACHE BOOL
        "Enable testing" FORCE)
else ()
    unset(BUILD_TESTING CACHE)
    unset(BUILD_TESTING)
endif ()

#######################
# Project Components
#######################
# Everything (except third_party) lives under core/ (the gar::core::rule library
# tree) and tools/ (executables and scripts). See docs/refactor_plan.md.
add_subdirectory(core)
add_subdirectory(core/hdc)
add_subdirectory(core/graph)
add_subdirectory(core/analysis)
add_subdirectory(core/engine)
add_subdirectory(tools/miner)
add_subdirectory(tools/cli)

# The single gar::core library: everything under core/ + tools/miner, one
# target to link for downstream consumers.
add_library(gar_core INTERFACE)
target_link_libraries(gar_core INTERFACE
    gar_rule gar_hdc gar_graph gar_analysis
    gar_engine_chase gar_engine_agent gar_miner)
add_library(gar::core ALIAS gar_core)

if (GAR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
