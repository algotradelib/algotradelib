# Built by .github/workflows/prefix-package.yaml against the unpacked prefix, before the prefix is pushed.
cmake_minimum_required(VERSION 3.10)
project(prefix_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(algotradelib CONFIG REQUIRED)

add_executable(prefix-consumer main.cpp)
target_link_libraries(prefix-consumer PRIVATE algotradelib::algotradelib)

enable_testing()
add_test(NAME prefix-consumer COMMAND prefix-consumer)
