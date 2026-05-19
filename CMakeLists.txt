cmake_minimum_required(VERSION 3.27)

option(BUILD_RIP_WORKER "Build the gRPC Gutenprint worker when dependencies are available" ON)
option(BUILD_CORE_TESTS "Build host-provided worker integration tests" OFF)

project(guten-rip-worker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/RipWorker.cmake)
