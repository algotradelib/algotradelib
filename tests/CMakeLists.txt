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
if(ATL_USE_EIGEN)
    catch_discover_tests(algotradelib-analytics-tests)
endif()
