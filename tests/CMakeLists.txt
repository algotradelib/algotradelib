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

add_executable(algotradelib-loop-tests test-loop.cpp)
target_link_libraries(algotradelib-loop-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-backtest-tests test-backtest.cpp)
target_link_libraries(algotradelib-backtest-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-vanilla-tests test-vanilla.cpp)
target_link_libraries(algotradelib-vanilla-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-live-tests test-live.cpp)
target_link_libraries(algotradelib-live-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-reconcile-orders-tests test-reconcile-orders.cpp)
target_link_libraries(algotradelib-reconcile-orders-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-journal-tests test-journal.cpp)
target_link_libraries(algotradelib-journal-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-runner-tests test-runner.cpp)
target_link_libraries(algotradelib-runner-tests PRIVATE
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
catch_discover_tests(algotradelib-runner-tests)
catch_discover_tests(algotradelib-journal-tests)
catch_discover_tests(algotradelib-reconcile-orders-tests)
catch_discover_tests(algotradelib-live-tests)
catch_discover_tests(algotradelib-vanilla-tests)
catch_discover_tests(algotradelib-backtest-tests)
catch_discover_tests(algotradelib-loop-tests)
if(ATL_USE_EIGEN)
    catch_discover_tests(algotradelib-analytics-tests)
endif()

# The strategy surface's isolation is asserted in source, by a translation unit that includes
# only <algotradelib/strategy>; see tests/test-strategy-isolation.cpp. It is built rather than
# merely run, because a violation should fail the build.
add_executable(algotradelib-strategy-isolation test-strategy-isolation.cpp)
target_link_libraries(algotradelib-strategy-isolation PRIVATE algotradelib::algotradelib)
add_test(NAME strategy-surface-stays-isolated COMMAND algotradelib-strategy-isolation)

# The same guarantee for the dry runner: a dry run links no transport, so a dry-run binary has no
# code path that could place an order.
add_executable(algotradelib-runner-isolation test-runner-isolation.cpp)
target_link_libraries(algotradelib-runner-isolation PRIVATE algotradelib::algotradelib)
add_test(NAME dry-runner-stays-isolated COMMAND algotradelib-runner-isolation)

# Every header must compile on its own.
#
# This is what makes include *order* irrelevant: a header that includes everything it uses can be
# included first, last, or alone, and no caller has to know a required predecessor. Without it the
# library would work only in the orders that happen to have been tried, and a new consumer would
# hit an error in a header they did not write.
#
# Checked by generating one translation unit per header, because the property is per-header and a
# single unit including them all would prove nothing: the first include would satisfy the second.
# A violation fails the build rather than a test run.
file(GLOB_RECURSE ATL_ALL_HEADERS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/algotradelib/*.hpp"
)

set(ATL_STANDALONE_SOURCES "")
foreach(header IN LISTS ATL_ALL_HEADERS)
    # Some headers require a linear-algebra backend on purpose: the analytics umbrella refuses to
    # compile without one, naming the option to set. Checking those in a build that has no backend
    # would be asserting that a deliberate diagnostic does not happen.
    #
    # Decided from each header's own text rather than from a path list, so a new header that reaches
    # analytics is skipped without anyone remembering to extend a regex here -- which is exactly the
    # maintenance this check exists to avoid needing.
    if(NOT ATL_USE_EIGEN)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../include/${header}" ATL_HEADER_TEXT)
        if(header MATCHES "src/analytics/" OR
           header MATCHES "src/math/(matrix|eigen)" OR
           ATL_HEADER_TEXT MATCHES "include <algotradelib/analytics>" OR
           ATL_HEADER_TEXT MATCHES "include \"[./]*math/matrix.hpp\"")
            continue()
        endif()
    endif()
    string(MAKE_C_IDENTIFIER "${header}" slug)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/standalone/${slug}.cpp")
    file(WRITE "${generated}"
        "// Generated: asserts <${header}> compiles with nothing included before it.\n"
        "#include <${header}>\n"
    )
    list(APPEND ATL_STANDALONE_SOURCES "${generated}")
endforeach()

list(LENGTH ATL_STANDALONE_SOURCES ATL_STANDALONE_COUNT)
message(STATUS "algotradelib headers checked standalone: ${ATL_STANDALONE_COUNT}")

if(ATL_STANDALONE_SOURCES)
    add_library(algotradelib-header-standalone OBJECT ${ATL_STANDALONE_SOURCES})
    target_link_libraries(algotradelib-header-standalone PRIVATE algotradelib::algotradelib)
endif()

# The onboarding scaffold, exercised end to end: generate a project, configure it against this
# checkout, build it, run it.
#
# Built rather than inspected, because what a new consumer depends on is that it compiles and runs.
# This is the same standard `ai/wall-clock-test-assertions` settled on -- observe the mechanism that
# does the work, not a seam that exists for the test -- and here the mechanism is a real build.
#
# Not run on Windows: the script is POSIX sh and the generated project is checked with `cmp`. The
# scaffold itself is portable CMake; only this driver assumes a shell.
if(NOT WIN32)
    # The sub-build must match this one, or a no-backend build would be tested against a backend and
    # the guard in the example would never be exercised.
    set(ATL_SCAFFOLD_ARGS "-DATL_USE_EIGEN=${ATL_USE_EIGEN}")
    if(CMAKE_GENERATOR)
        list(APPEND ATL_SCAFFOLD_ARGS "-G${CMAKE_GENERATOR}")
    endif()
    if(CMAKE_BUILD_TYPE)
        list(APPEND ATL_SCAFFOLD_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    endif()

    add_test(
        NAME scaffold-generates-a-project-that-builds-and-runs
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/scaffold-smoke.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}/.."
                "${CMAKE_CURRENT_BINARY_DIR}/scaffold"
                "${CMAKE_COMMAND}"
                "$<IF:$<BOOL:${ATL_USE_EIGEN}>,ON,OFF>"
                ${ATL_SCAFFOLD_ARGS}
    )
    # A full configure and build of a generated project, so it needs more than a unit test's budget.
    set_tests_properties(scaffold-generates-a-project-that-builds-and-runs
        PROPERTIES TIMEOUT 600)
endif()
