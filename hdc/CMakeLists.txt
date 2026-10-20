cmake_minimum_required(VERSION 3.20)
project(gar_hdc LANGUAGES CXX)

#######################
# gar_hdc: HDC numeric library (bit-packed hypervectors) — the future
# deduction scorer's numeric kernel. Level 1 discipline: zero in-repo
# dependencies (no gar::core / gar::graph includes; the Fact -> SymbolId
# binding is the caller's job, symbols are bare uint64 ids). Pure CPU
# C++20, plain .cpp only: no TBB, no CUDA.
#######################
add_library(gar_hdc STATIC
    src/hypervector.cpp
    src/encoder.cpp
    src/evidence.cpp
)
add_library(gar::hdc ALIAS gar_hdc)

target_include_directories(gar_hdc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(gar_hdc PUBLIC cxx_std_20)
