cmake_minimum_required(VERSION 3.20)
project(gar_engine LANGUAGES CXX)

# Engine family: deduction (full-closure chase) now; agent-guided engines
# plug in as sibling subdirectories later.
add_subdirectory(deduction)
