file(GLOB_RECURSE CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# The analytics examples include <algotradelib/analytics>, which refuses to compile
# without a linear algebra backend -- on purpose, so the diagnostic names the option to
# set. A build configured with ATL_USE_EIGEN=OFF must therefore not try to build them.
#
# Matched on the file name rather than on the glob's absolute paths: a worktree directory
# can itself be named after the branch, and a bare "analytics" regex over full paths then
# excludes every example in the directory.
if(NOT ATL_USE_EIGEN)
    set(ATL_PORTABLE_EXAMPLES "")
    foreach(CPP_FILE ${CPP_SOURCES})
        get_filename_component(ATL_EXAMPLE_NAME ${CPP_FILE} NAME)
        if(NOT ATL_EXAMPLE_NAME MATCHES "analytics")
            list(APPEND ATL_PORTABLE_EXAMPLES ${CPP_FILE})
        endif()
    endforeach()
    set(CPP_SOURCES ${ATL_PORTABLE_EXAMPLES})
endif()

if(NOT CPP_SOURCES)
    message(FATAL_ERROR "No C++ source files found in examples/")
endif()

message(STATUS "Building examples:")

foreach(CPP_FILE ${CPP_SOURCES})
    file(RELATIVE_PATH REL_PATH ${CMAKE_CURRENT_SOURCE_DIR} ${CPP_FILE})
    get_filename_component(example ${CPP_FILE} NAME_WE)

    message(STATUS "    Source: ${REL_PATH}")

    add_executable(${example} ${CPP_FILE})

    target_link_libraries(${example} PRIVATE algotradelib::algotradelib)
endforeach()