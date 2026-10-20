cmake_minimum_required(VERSION 3.20)
project(gar_reason LANGUAGES CXX)

#######################
# gar_reason: GAR deduction (chase / logical closure), pure CPU
#######################
add_library(gar_reason STATIC
    src/chase.cpp
)
add_library(gar::reason ALIAS gar_reason)

target_include_directories(gar_reason PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_reason PUBLIC gar::miner)
target_compile_features(gar_reason PUBLIC cxx_std_20)

# oneTBB (bundled with MatrixGraph) for rule-level parallelism in the chase;
# without it the engine falls back to a serial round loop.
if (TARGET tbb)
    target_link_libraries(gar_reason PUBLIC tbb)
    target_compile_definitions(gar_reason PRIVATE GAR_REASON_HAS_TBB)
endif ()
