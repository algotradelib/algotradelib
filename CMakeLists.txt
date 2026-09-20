cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(algotradelib VERSION 0.0.0.9000 LANGUAGES CXX)

include(GNUInstallDirs)

# atomic linking issue in llvm (macos, windows): dummy atomic
if ((APPLE OR WIN32) AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_library(atomic INTERFACE)
endif()

find_package(Boost 1.87.0 REQUIRED COMPONENTS json)
find_package(OpenSSL 3.6.2 REQUIRED)

# Header-only library
add_library(algotradelib INTERFACE)
add_library(algotradelib::algotradelib ALIAS algotradelib)

target_compile_features(algotradelib INTERFACE cxx_std_17)

if(MSVC)
    target_compile_definitions(algotradelib INTERFACE BOOST_JSON_NO_LIB)
endif()

target_include_directories(algotradelib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ATL_BUILD_EXAMPLES "Build example C++ programs" OFF)
option(ATL_BUILD_TESTS "Build test C++ programs" OFF)
option(ATL_ENABLE_COVERAGE "Enable C++ test coverage" OFF)

# The analytics layer needs a linear algebra backend; quote, order, and market-data
# behaviour does not. The option is named for the backend rather than for a feature
# because the backend is expected to change: a consumer may prefer another library, or
# this one may carry its own, and the analytics API is written so that would be a new
# implementation rather than a new public surface.
option(ATL_USE_EIGEN "Build the analytics layer against Eigen" ON)

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
    target_compile_definitions(algotradelib INTERFACE ATL_HAS_EIGEN=1)
    message(STATUS "algotradelib analytics: Eigen ${Eigen3_VERSION}")
endif()

target_link_libraries(algotradelib INTERFACE Boost::json)
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
