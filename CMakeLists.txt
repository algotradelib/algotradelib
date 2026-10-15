cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# The version is written once, in include/algotradelib/src/version.hpp, and read from there -- as
# Eigen does -- rather than generated into it with configure_file. A generated header would live in the
# build directory, so a consumer who adds include/ to their own build would have no version at all.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/algotradelib/src/version.hpp" ATL_VERSION_LINES
    REGEX "^#define ATL_VERSION_(MAJOR|MINOR|PATCH|DEV) [0-9]+$")
# Re-run the configure step when the header changes, or a build directory keeps the old version in the
# package version file and the documentation stamp.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/algotradelib/src/version.hpp")
foreach(part MAJOR MINOR PATCH DEV)
    set(ATL_VERSION_${part} "")
    foreach(line IN LISTS ATL_VERSION_LINES)
        if(line MATCHES "^#define ATL_VERSION_${part} ([0-9]+)$")
            set(ATL_VERSION_${part} "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(ATL_VERSION_${part} STREQUAL "")
        message(FATAL_ERROR "include/algotradelib/src/version.hpp defines no ATL_VERSION_${part}")
    endif()
endforeach()
# A release has three parts; a development version carries the fourth.
if(ATL_VERSION_DEV EQUAL 0)
    set(ATL_PROJECT_VERSION "${ATL_VERSION_MAJOR}.${ATL_VERSION_MINOR}.${ATL_VERSION_PATCH}")
else()
    set(ATL_PROJECT_VERSION
        "${ATL_VERSION_MAJOR}.${ATL_VERSION_MINOR}.${ATL_VERSION_PATCH}.${ATL_VERSION_DEV}")
endif()

project(algotradelib VERSION ${ATL_PROJECT_VERSION} LANGUAGES CXX)

option(ATL_USE_EIGEN "Build the analytics layer against Eigen" ON)
option(ATL_FETCH_DEPS "Fetch all depending libraries using FetchContent" OFF)

if(ATL_FETCH_DEPS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake")
endif()

include(GNUInstallDirs)

# atomic linking issue in llvm (macos, windows): dummy atomic
if ((APPLE OR WIN32) AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_library(atomic INTERFACE)
endif()

find_package(Boost 1.87.0 REQUIRED COMPONENTS json)
find_package(OpenSSL 3.0 REQUIRED)

# Header-only library
add_library(algotradelib INTERFACE)
add_library(algotradelib::algotradelib ALIAS algotradelib)

target_compile_features(algotradelib INTERFACE cxx_std_17)

if(MSVC)
    target_compile_definitions(algotradelib INTERFACE BOOST_JSON_NO_LIB)
endif()

# Beast, Asio and the library's templates exceed cl.exe's default section limit in one object file (C1128).
# Decided by the consumer's compiler, not this build's, since an installed package is exported with it; Clang
# (clang-cl or clang++) needs no flag, and clang++ rejects it.
target_compile_options(algotradelib INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

target_include_directories(algotradelib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ATL_BUILD_EXAMPLES "Build example C++ programs" OFF)
option(ATL_BUILD_TESTS "Build test C++ programs" OFF)
option(ATL_ENABLE_COVERAGE "Instrument the tests for coverage (Clang or GCC)" OFF)
if(ATL_ENABLE_COVERAGE AND NOT ATL_BUILD_TESTS)
    message(FATAL_ERROR "ATL_ENABLE_COVERAGE measures the tests; configure with ATL_BUILD_TESTS=ON too.")
endif()

# `cmake --build build --target docs` runs doxygen/Doxyfile with PROJECT_NUMBER set to this version, so
# the version is still written only in version.hpp; `doxygen doxygen/Doxyfile` by hand still works,
# unstamped. Only in a top-level build: a consumer that adds this directory with add_subdirectory may
# have its own `docs` target, and CMake refuses two targets of one name. (PROJECT_IS_TOP_LEVEL says
# this directly, but needs CMake 3.21.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    find_package(Doxygen QUIET)
    if(TARGET Doxygen::doxygen)
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.version.in
            ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.version
            @ONLY
        )
        # The Doxyfile's paths are relative to the source root, and its output goes to doxygen/docs
        # there, where a hand run puts it. WARN_AS_ERROR makes a warning fail the target.
        add_custom_target(docs
            COMMAND Doxygen::doxygen ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.version
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating the HTML documentation in doxygen/docs/html"
            VERBATIM
        )
    else()
        message(STATUS "algotradelib: Doxygen not found, so there is no docs target")
    endif()
endif()

# The analytics layer needs a linear algebra backend; quote, order, and market-data
# behaviour does not. The option is named for the backend rather than for a feature
# because the backend is expected to change: a consumer may prefer another library, or
# this one may carry its own, and the analytics API is written so that would be a new
# implementation rather than a new public surface. (ATL_USE_EIGEN is declared above, before
# cmake/deps.cmake reads it.)

if(ATL_USE_EIGEN)
    # Requested without a version on purpose. Eigen's package config declares
    # same-major-version compatibility, so asking for "3.4 or newer" is *rejected* by
    # Eigen 5 rather than satisfied by it. The minimum is therefore checked afterwards,
    # which accepts every release that actually works.
    find_package(Eigen3 REQUIRED CONFIG)
    if(Eigen3_VERSION AND Eigen3_VERSION VERSION_LESS 3.4)
        message(FATAL_ERROR
            "algotradelib analytics needs Eigen 3.4 or newer; found ${Eigen3_VERSION}. "
            "Configure with -DATL_USE_EIGEN=OFF to build without the analytics layer.")
    endif()
    target_link_libraries(algotradelib INTERFACE Eigen3::Eigen)

    # Two names, and they are not redundant: ATL_USE_EIGEN is a CMake option and ATL_HAS_EIGEN is a
    # C++ preprocessor macro. Only the second can cross into a compile.
    #
    # ATL_USE_EIGEN exists at configure time, in *this* build, and is what a person sets. A consumer
    # of the installed package has no such variable -- it was never their option -- so a header that
    # tested it would force every consumer to restate, correctly, how the library had been built.
    # That is exactly the manual duplication that goes wrong. ATL_HAS_EIGEN travels on the exported
    # interface target instead, so a consumer gets the fact automatically and cannot get it wrong.
    #
    # The distinction is also visible in the #error text in <algotradelib/analytics>: it *names*
    # ATL_USE_EIGEN, because that is the action a person takes, while the #if tests ATL_HAS_EIGEN,
    # because that is the state the compiler is in. A request and a fact, which is the same shape as
    # _OPENMP (what the flags set) versus EIGEN_HAS_OPENMP (what Eigen derives and tests internally).
    target_compile_definitions(algotradelib INTERFACE ATL_HAS_EIGEN=1)
    message(STATUS "algotradelib analytics: Eigen ${Eigen3_VERSION}")

    # Eigen threads its dense matrix products when, and only when, `_OPENMP` is defined in
    # the translation unit. That is the consumer's decision, not this library's, and it is
    # left alone -- no EIGEN_DONT_PARALLELIZE here or anywhere else. But a build whose flags
    # define `_OPENMP` without linking an OpenMP runtime fails at link time, on `__kmpc_*`
    # and `omp_get_*` symbols emitted from deep inside an Eigen template, with nothing in the
    # message to connect it to a matrix product. `-Xpreprocessor -fopenmp` alone does exactly
    # that, and it is a common way to reach OpenMP with Apple's clang.
    #
    # So the runtime is supplied when the flags say it is needed. This is not enabling
    # anything: the consumer already asked for OpenMP, and this only makes the request work.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
#ifndef _OPENMP
#error openmp is not enabled in these flags
#endif
int main() { return 0; }
" ATL_OPENMP_IN_FLAGS)
    if(ATL_OPENMP_IN_FLAGS)
        find_package(OpenMP QUIET COMPONENTS CXX)
        if(OpenMP_CXX_FOUND)
            target_link_libraries(algotradelib INTERFACE OpenMP::OpenMP_CXX)
            message(STATUS
                "algotradelib analytics: _OPENMP is set, so Eigen will thread its matrix "
                "products; linking OpenMP::OpenMP_CXX")
        else()
            message(FATAL_ERROR
                "The C++ flags define _OPENMP, so Eigen will emit calls into an OpenMP "
                "runtime from its matrix products, but no OpenMP runtime could be found. "
                "Install one (libomp), or remove the OpenMP flag from CMAKE_CXX_FLAGS and "
                "the CXXFLAGS environment variable.")
        endif()
    endif()
endif()

target_link_libraries(algotradelib INTERFACE Boost::json)
# A Boost built from source (ATL_FETCH_DEPS) has a target per library, and its Boost::headers does not cover them all.
# Only there: an installed config resolves Boost::json alone, so an exported Boost::asio would break its consumers.
if(ATL_FETCH_DEPS)
    foreach(component asio beast crc)
        target_link_libraries(algotradelib INTERFACE Boost::${component})
    endforeach()
endif()
if(TARGET Boost::headers)
    target_link_libraries(algotradelib INTERFACE Boost::headers)
elseif(TARGET Boost::boost)
    target_link_libraries(algotradelib INTERFACE Boost::boost)
endif()

target_link_libraries(algotradelib INTERFACE
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(ATL_BUILD_EXAMPLES)
    add_subdirectory(examples examples)
endif()

if(ATL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests tests)
endif()

# A build with fetched dependencies (CI) installs nothing: the fetched Boost is not installed, so no package could
# name it.
if(ATL_FETCH_DEPS)
    return()
endif()

# Install header files
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(TARGETS algotradelib EXPORT algotradelibTargets)

# Generate config files
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/algotradelibConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)
# Recorded so an installed consumer resolves the same dependency the build used, and
# so a consumer of a build made without the backend is not asked for it.
set(ATL_INSTALLED_WITH_EIGEN ${ATL_USE_EIGEN})

# Normalized to a literal rather than passed through, because the OpenMP check only runs
# when the backend is enabled: without this, a no-backend install writes a bare `if()` into
# the package config, whose meaning depends on the consumer's CMake version.
if(ATL_OPENMP_IN_FLAGS)
    set(ATL_INSTALLED_WITH_OPENMP ON)
else()
    set(ATL_INSTALLED_WITH_OPENMP OFF)
endif()

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/algotradelibConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/algotradelibConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/algotradelib
)

export(TARGETS algotradelib
    NAMESPACE algotradelib::
    FILE algotradelibTargets.cmake
)
install(EXPORT algotradelibTargets
    NAMESPACE algotradelib::
    FILE algotradelibTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/algotradelib
)

# Install the generated config files
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/algotradelibConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/algotradelibConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/algotradelib
)
