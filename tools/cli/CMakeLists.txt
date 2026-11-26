cmake_minimum_required(VERSION 3.20)
project(gar_apps LANGUAGES CXX)

    add_executable(kg_loader kg_loader.cu)
    target_link_libraries(kg_loader PRIVATE gar::core::graph)
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
    target_link_libraries(match_pattern PRIVATE gar::core::graph)
    target_compile_options(match_pattern PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(match_pattern PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(mine_predicates mine_predicates.cu)
    target_link_libraries(mine_predicates PRIVATE gar::core::graph)
    target_compile_options(mine_predicates PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(mine_predicates PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(deduce deduce.cu)
    target_link_libraries(deduce PRIVATE gar::core::graph gar::core::engine::chase)
    target_compile_options(deduce PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(deduce PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(deduce_gpu deduce_gpu.cu)
    target_link_libraries(deduce_gpu PRIVATE gar::core::graph gar::core::engine::chase)
    target_compile_options(deduce_gpu PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(deduce_gpu PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(validate_rules validate_rules.cu)
    target_link_libraries(validate_rules PRIVATE gar::core::graph)
    target_compile_options(validate_rules PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(validate_rules PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(gar_train_policy gar_train_policy.cpp)
    target_link_libraries(gar_train_policy PRIVATE
        gar::core::graph gar::core::analysis gar::core::engine::agent
    )
    target_compile_features(gar_train_policy PRIVATE cxx_std_20)

    # Diagnostic probe: HDC action-goal similarity vs handcrafted score for
    # proof-membership AUC. Throwaway instrument, not a product surface.
    add_executable(gar_hdc_probe gar_hdc_probe.cpp)
    target_link_libraries(gar_hdc_probe PRIVATE
        gar::core::graph gar::core::analysis gar::core::engine::agent
    )
    target_compile_features(gar_hdc_probe PRIVATE cxx_std_20)

    add_executable(gar_sketch_probe gar_sketch_probe.cpp)
    target_link_libraries(gar_sketch_probe PRIVATE
        gar::core::graph gar::core::analysis gar::core::engine::agent
    )
    target_compile_features(gar_sketch_probe PRIVATE cxx_std_20)

    add_executable(gar_bulk_probe gar_bulk_probe.cpp)
    target_link_libraries(gar_bulk_probe PRIVATE gar::core::graph)
    target_compile_features(gar_bulk_probe PRIVATE cxx_std_20)

    # Diagnostic probe: multi-hop graph proximity (BFS distance) for
    # proof-membership AUC. Throwaway instrument, not a product surface.
    add_executable(gar_graph_probe gar_graph_probe.cpp)
    target_link_libraries(gar_graph_probe PRIVATE
        gar::core::graph gar::core::analysis gar::core::engine::agent
    )
    target_compile_features(gar_graph_probe PRIVATE cxx_std_20)

    # Diagnostic feature extractor for the learned strategy selector (path C).
    add_executable(gar_goal_features gar_goal_features.cpp)
    target_link_libraries(gar_goal_features PRIVATE
        gar::core::graph gar::core::analysis gar::core::engine::agent
    )
    target_compile_features(gar_goal_features PRIVATE cxx_std_20)

    add_executable(gar_cone_union gar_cone_union.cpp)
    target_link_libraries(gar_cone_union PRIVATE
        gar::core::graph gar::core::analysis
    )
    target_compile_features(gar_cone_union PRIVATE cxx_std_20)

    add_executable(gar_cone_sizes gar_cone_sizes.cpp)
    target_link_libraries(gar_cone_sizes PRIVATE
        gar::core::graph gar::core::analysis
    )
    target_compile_features(gar_cone_sizes PRIVATE cxx_std_20)

    add_executable(gar_reason gar_reason.cu)
    target_link_libraries(gar_reason PRIVATE gar::core::graph gar::core::engine::chase gar::core::engine::agent)
    target_compile_options(gar_reason PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(gar_reason PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    add_executable(gar_analyze gar_analyze.cu)
    target_link_libraries(gar_analyze PRIVATE
        gar::core::graph gar::core::engine::chase gar::core::engine::agent gar::core::analysis
    )
    target_compile_options(gar_analyze PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-mno-avx512f,-mno-avx512bf16,-mno-amx-tile>
    )
    set_target_properties(gar_analyze PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )
