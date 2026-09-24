# CMake before 3.12 ignores the policy-range suffix; APIs remain 3.6-compatible.
cmake_minimum_required(VERSION 3.6...3.13)
project(k2b-cedarc-runtime LANGUAGES C)
# File scope is required: enabling ASM inside the helper loses CMake language
# state. The root project's default-OFF option never enters this directory.
enable_language(ASM)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/K2BCedarRuntime.cmake")
k2b_add_cedar_runtime()
