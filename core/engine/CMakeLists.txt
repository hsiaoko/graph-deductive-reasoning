cmake_minimum_required(VERSION 3.20)
project(gar_engine LANGUAGES CXX)

# Engine family: deduction (full-closure chase) and agent (policy-guided
# deduction); further engines plug in as sibling subdirectories.
add_subdirectory(deduction)
add_subdirectory(agent)
