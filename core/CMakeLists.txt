cmake_minimum_required(VERSION 3.20)
project(gar_core LANGUAGES CXX)

#######################
# gar_core: rule representation (CSR topology + YAML attributes)
#######################
add_library(gar_core STATIC
    src/csr.cpp
    src/predicate.cpp
    src/rule.cpp
    src/rule_loader.cpp
    src/rule_store.cpp
)
add_library(gar::core ALIAS gar_core)

target_include_directories(gar_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_core PUBLIC yaml-cpp)
target_compile_features(gar_core PUBLIC cxx_std_20)

#######################
# gar_matrixgraph: bridge into MatrixGraph (CUDA graph engine)
#######################
if (GAR_ENABLE_MATRIXGRAPH)
    add_library(gar_matrixgraph STATIC
        src/matrixgraph_bridge.cpp
    )
    add_library(gar::matrixgraph ALIAS gar_matrixgraph)

    target_link_libraries(gar_matrixgraph PUBLIC gar_core matrixgraph_core)
    target_compile_definitions(gar_matrixgraph PUBLIC GAR_WITH_MATRIXGRAPH)
endif ()
