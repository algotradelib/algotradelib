find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

add_executable(algotradelib-core-tests test-core.cpp)
target_link_libraries(algotradelib-core-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-broker-port-tests test-broker-port.cpp)
target_link_libraries(algotradelib-broker-port-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-rate-limit-tests test-rate-limit.cpp)
target_link_libraries(algotradelib-rate-limit-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-market-data-tests test-market-data.cpp)
target_link_libraries(algotradelib-market-data-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-strategy-tests test-strategy.cpp)
target_link_libraries(algotradelib-strategy-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

if(ATL_USE_EIGEN)
    add_executable(algotradelib-analytics-tests test-analytics.cpp)
    target_link_libraries(algotradelib-analytics-tests PRIVATE
        algotradelib::algotradelib
        Catch2::Catch2WithMain
    )
endif()

include(Catch)
catch_discover_tests(algotradelib-core-tests)
catch_discover_tests(algotradelib-broker-port-tests)
catch_discover_tests(algotradelib-rate-limit-tests)
catch_discover_tests(algotradelib-market-data-tests)
catch_discover_tests(algotradelib-strategy-tests)
if(ATL_USE_EIGEN)
    catch_discover_tests(algotradelib-analytics-tests)
endif()

# Guards the acceptance criterion that a strategy names no broker and performs no I/O, by
# reading the compiler's own dependency output for <algotradelib/strategy>. Not a Catch2 test
# because the property is about the include graph rather than about runtime behaviour.
get_target_property(ATL_BOOST_INCLUDES Boost::json INTERFACE_INCLUDE_DIRECTORIES)
if(NOT ATL_BOOST_INCLUDES)
    set(ATL_BOOST_INCLUDES "")
endif()
if(ATL_USE_EIGEN)
    get_target_property(ATL_EIGEN_INCLUDES Eigen3::Eigen INTERFACE_INCLUDE_DIRECTORIES)
    if(ATL_EIGEN_INCLUDES)
        list(APPEND ATL_BOOST_INCLUDES ${ATL_EIGEN_INCLUDES})
    endif()
endif()

add_test(
    NAME strategy-surface-includes-no-broker-or-transport
    COMMAND ${CMAKE_COMMAND}
        -DATL_COMPILER=${CMAKE_CXX_COMPILER}
        -DATL_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../include
        -DATL_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/strategy-include-check
        "-DATL_EXTRA_INCLUDES=${ATL_BOOST_INCLUDES}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check-strategy-includes.cmake
)
