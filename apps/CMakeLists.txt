cmake_minimum_required(VERSION 3.20)
project(gar_apps LANGUAGES CXX)

if (GAR_ENABLE_MATRIXGRAPH)
    add_executable(kg_loader kg_loader.cu)
    target_link_libraries(kg_loader PRIVATE gar::matrixgraph)
    # Same nvcc/GCC-12 workaround as MatrixGraph: AVX512/AMX intrinsics
    # headers break nvcc, disable them for CUDA compilation.
    target_compile_options(kg_loader PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(kg_loader PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(match_pattern match_pattern.cu)
    target_link_libraries(match_pattern PRIVATE gar::matrixgraph gar::miner)
    target_compile_options(match_pattern PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(match_pattern PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(mine_predicates mine_predicates.cu)
    target_link_libraries(mine_predicates PRIVATE gar::matrixgraph gar::miner)
    target_compile_options(mine_predicates PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(mine_predicates PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(deduce deduce.cu)
    target_link_libraries(deduce PRIVATE gar::matrixgraph gar::reason)
    target_compile_options(deduce PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(deduce PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(deduce_gpu deduce_gpu.cu)
    target_link_libraries(deduce_gpu PRIVATE gar::matrixgraph gar::reason)
    target_compile_options(deduce_gpu PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(deduce_gpu PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(validate_rules validate_rules.cu)
    target_link_libraries(validate_rules PRIVATE gar::matrixgraph gar::reason)
    target_compile_options(validate_rules PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(validate_rules PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )
endif ()
