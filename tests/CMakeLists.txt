# A fetched-dependency build (ATL_FETCH_DEPS) fetches Catch2 too, so it is built by the compiler under test.
if(NOT ATL_FETCH_DEPS)
    find_package(Catch2 3 CONFIG QUIET)
endif()

if(NOT Catch2_FOUND)
    # Catch2 compiles its library at its own minimum (C++14) unless told otherwise, and then lacks the
    # std::string_view support a C++17 test expects (Apple Clang, whose default is older, shows it).
    if(NOT DEFINED CMAKE_CXX_STANDARD)
        set(CMAKE_CXX_STANDARD 17)
    endif()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()
include(Catch)

# A `switch` over an enum that misses one of its values is an error in the library's own builds. Every such
# `switch` in the library has no `default:`, so this is what tells a contributor who adds an enumerator where
# its cases are; each enum's list of values (`lists_every_value`) catches the rest. Only these targets: a
# consumer's build gets no flag from the library.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Werror=switch)
endif()

# ATL_ENABLE_COVERAGE instruments the test executables, never the library target, so an installed consumer
# inherits nothing. GCC and Clang both write gcov's format (`--coverage`), one data file beside each object, which
# gcovr reads for either (`gcov`, or `llvm-cov gcov` for Clang) and joins by line.
if(ATL_ENABLE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ATL_ENABLE_COVERAGE supports GCC and Clang, not ${CMAKE_CXX_COMPILER_ID}.")
endif()

# Every tests/test-*.cpp is one executable, named after the file, as examples/ builds example-*.cpp.
#
# A file ending in -isolation.cpp is not a Catch2 suite: it includes one umbrella header and nothing else,
# and asserts with #error that the header did not reach the transport or a broker adapter (see the files).
# A violation fails the build; the test that runs it only records that it was built.
#
# The analytics suite includes <algotradelib/analytics>, which refuses to compile without a linear algebra
# backend, so a build without Eigen skips it, as examples/ skips its analytics examples.
file(GLOB ATL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test-*.cpp")

foreach(source IN LISTS ATL_TEST_SOURCES)
    get_filename_component(test ${source} NAME_WE)
    if(NOT ATL_USE_EIGEN AND test MATCHES "analytics")
        continue()
    endif()

    add_executable(${test} ${source})
    target_link_libraries(${test} PRIVATE algotradelib::algotradelib)
    # Some cases assert IEEE results digit for digit; a compiler that fuses a multiply and an add (GCC does by
    # default on arm64) rounds once instead of twice and moves the last digit.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test} PRIVATE -ffp-contract=off)
    endif()
    if(ATL_ENABLE_COVERAGE)
        target_compile_options(${test} PRIVATE --coverage)
        target_link_libraries(${test} PRIVATE --coverage)
        # GCC instruments every function a file emits, Boost's, Catch2's and the standard library's included;
        # only the library's headers and the live program are reported, so only they are instrumented.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${test} PRIVATE
                "-fprofile-filter-files=.*/(include/algotradelib/.*|examples/live_program[.]hpp)")
        endif()
    endif()

    if(test MATCHES "-isolation$")
        add_test(NAME ${test} COMMAND ${test})
        continue()
    endif()

    target_link_libraries(${test} PRIVATE Catch2::Catch2WithMain)
    # What CMake read out of version.hpp, so a test can compare it with what the preprocessor sees. The
    # project-named variable rather than PROJECT_VERSION, which a project() in a fetched dependency would
    # shadow within its own directory.
    target_compile_definitions(${test} PRIVATE ATL_TEST_PROJECT_VERSION="${algotradelib_VERSION}")
    # The live program's broker-free wiring lives beside the example that uses it, and is tested against
    # the in-memory session.
    target_include_directories(${test} PRIVATE "${algotradelib_SOURCE_DIR}/examples")
    catch_discover_tests(${test})
endforeach()

# Every tests/compile-fail/test-<name>.cpp must not compile, and the compiler's diagnostic must be the
# intended one. Each file states it in a `// expect: <regex>` line (Clang's and GCC's wording, `\n`
# standing for a newline); CTest builds the file under the test name <name> and passes only on a match, so
# a check that no longer fires, or fails for another reason, fails the test. A file with a
# `// variants: <v>...` line is built once per value with ATL_PROBE_VARIANT=<v>, as <name>-<v>.
#
# Built by the test rather than by `all`, because each is meant not to build.
file(GLOB ATL_COMPILE_FAIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/compile-fail/test-*.cpp")

foreach(source IN LISTS ATL_COMPILE_FAIL_SOURCES)
    get_filename_component(stem ${source} NAME_WE)
    string(REGEX REPLACE "^test-" "" name ${stem})
    # The expectation is read at configure time, so editing it must reconfigure.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})

    file(STRINGS ${source} expect REGEX "^// expect: ")
    list(LENGTH expect count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "${source} needs exactly one '// expect: <regex>' line; it has ${count}")
    endif()
    string(REGEX REPLACE "^// expect: " "" expect "${expect}")
    string(STRIP "${expect}" expect)
    if(expect STREQUAL "")
        message(FATAL_ERROR "${source} has an empty '// expect:' line")
    endif()
    string(REPLACE "\\n" "\n" expect "${expect}")

    file(STRINGS ${source} variants REGEX "^// variants: ")
    string(REGEX REPLACE "^// variants: " "" variants "${variants}")
    separate_arguments(variants)
    if(variants)
        set(unique ${variants})
        list(REMOVE_DUPLICATES unique)
        if(NOT unique STREQUAL variants)
            message(FATAL_ERROR "${source} lists a variant twice: ${variants}")
        endif()
    else()
        set(variants "-")
    endif()

    foreach(variant IN LISTS variants)
        set(probe ${name})
        if(NOT variant STREQUAL "-")
            set(probe ${name}-${variant})
        endif()
        add_library(compile-fail-${probe} OBJECT EXCLUDE_FROM_ALL ${source})
        target_link_libraries(compile-fail-${probe} PRIVATE algotradelib::algotradelib)
        target_include_directories(compile-fail-${probe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT variant STREQUAL "-")
            target_compile_definitions(compile-fail-${probe} PRIVATE ATL_PROBE_VARIANT=${variant})
        endif()
        add_test(
            NAME ${probe}
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target compile-fail-${probe}
                    --config "$<CONFIG>"
        )
        # GCC quotes names with ‘’ in a UTF-8 locale; the patterns are written with ASCII quotes.
        set_tests_properties(${probe} PROPERTIES
            PASS_REGULAR_EXPRESSION "${expect}" TIMEOUT 300 LABELS compile-fail ENVIRONMENT "LC_ALL=C")
    endforeach()
endforeach()

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
# Observe the mechanism that does the work, not a seam that exists for the test -- and here the
# mechanism is a real build.
#
# Not run on Windows: the script is POSIX sh and the generated project is checked with `cmp`. The
# scaffold itself is portable CMake; only this driver assumes a shell.
if(NOT WIN32)
    # The sub-build must match this one, or a no-backend build would be tested against a backend and
    # the guard in the example would never be exercised.
    set(ATL_SCAFFOLD_ARGS "-DATL_USE_EIGEN=${ATL_USE_EIGEN}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    if(CMAKE_GENERATOR)
        list(APPEND ATL_SCAFFOLD_ARGS "-G${CMAKE_GENERATOR}")
    endif()
    if(CMAKE_BUILD_TYPE)
        list(APPEND ATL_SCAFFOLD_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    endif()
    # A fetched-dependency build passes that on, with the sources already downloaded.
    if(ATL_FETCH_DEPS)
        list(APPEND ATL_SCAFFOLD_ARGS "-DATL_FETCH_DEPS=ON" "-DFETCHCONTENT_SOURCE_DIR_BOOST=${boost_SOURCE_DIR}")
        if(ATL_USE_EIGEN)
            list(APPEND ATL_SCAFFOLD_ARGS "-DFETCHCONTENT_SOURCE_DIR_EIGEN3=${eigen3_SOURCE_DIR}")
        endif()
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
        PROPERTIES TIMEOUT 600 LABELS scaffold)
endif()
