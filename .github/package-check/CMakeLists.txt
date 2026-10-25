# Built by .github/workflows/package.yaml against the unpacked package, before it is pushed.
cmake_minimum_required(VERSION 3.10)
project(package_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(algotradelib CONFIG REQUIRED)

add_executable(package-check main.cpp)
target_link_libraries(package-check PRIVATE algotradelib::algotradelib)

enable_testing()
add_test(NAME package-check COMMAND package-check)
