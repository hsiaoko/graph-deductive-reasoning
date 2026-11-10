cmake_minimum_required(VERSION 3.20)
project(gar_miner LANGUAGES CXX)

#######################
# gar_miner: GAR mining (AMIE-style chain rules, v1)
#######################
add_library(gar_miner STATIC
    src/chain_miner.cpp
    src/pattern_miner.cpp
)
add_library(gar::miner ALIAS gar_miner)

target_include_directories(gar_miner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gar_miner PUBLIC gar::graph)
target_compile_features(gar_miner PUBLIC cxx_std_20)

# The mine apps load ImmutableCSR graphs through the bridge module, so they
# are only available when MatrixGraph integration is enabled.
if (TARGET gar_bridge)
    #######################
    # mine_rules app
    #######################
    add_executable(mine_rules mine_rules.cpp)
    target_link_libraries(mine_rules PRIVATE gar::miner gar::bridge)

    #######################
    # mine_graph_rules app (generic topology patterns)
    #######################
    add_executable(mine_graph_rules mine_graph_rules.cpp)
    target_link_libraries(mine_graph_rules PRIVATE gar::miner gar::bridge)
endif ()
