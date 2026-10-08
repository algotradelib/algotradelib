# ATL_FETCH_DEPS (CI only): build the C++ dependencies from source with the compiler under test, so every compiler
# and standard library is checked against a Boost built by itself, and a runner's preinstalled packages do not
# decide what is tested. Nothing is searched for first.
#
# OpenSSL is not fetched: it builds with Perl and make, not CMake, so FetchContent cannot add it. It comes from the
# system: libssl-dev on Ubuntu runners, the preinstalled OpenSSL 3 on Windows runners, and Homebrew's openssl@3 on
# macOS runners, keg-only, so with OPENSSL_ROOT_DIR=$(brew --prefix openssl@3). macOS 26 (macos-latest) has it
# installed; macOS 15 and 14 ship OpenSSL 1.1 and need `brew install openssl@3` first.
if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "ATL_FETCH_DEPS needs CMake 3.24 or newer (OVERRIDE_FIND_PACKAGE).")
endif()
include(FetchContent)

# Boost's CMake release archive, limited to what the library includes (and what those need).
set(BOOST_INCLUDE_LIBRARIES asio beast crc json)
FetchContent_Declare(Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz
    URL_HASH SHA256=7da75f171837577a52bbf217e17f8ea576c7c246e4594d617bfde7fafd408be5
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Boost)

# Eigen is header-only: its sources are fetched, its own CMake project (tests, BLAS and Fortran probes) is not run.
if(ATL_USE_EIGEN)
    FetchContent_Declare(Eigen3
        URL https://gitlab.com/libeigen/eigen/-/archive/3.4.1/eigen-3.4.1.tar.gz
        URL_HASH SHA256=b93c667d1b69265cdb4d9f30ec21f8facbbe8b307cf34c0b9942834c6d4fdbe2
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR no-cmake-project
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(Eigen3)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${eigen3_SOURCE_DIR}")
    # With no CMake project run, FetchContent writes no redirect, and find_package(Eigen3) would find an installed
    # Eigen instead; this one answers it with the target above.
    file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/eigen3-config.cmake" "set(Eigen3_VERSION 3.4.1)\n")
    file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/eigen3-config-version.cmake"
        "set(PACKAGE_VERSION 3.4.1)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")
endif()
