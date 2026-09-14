file(GLOB_RECURSE CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

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