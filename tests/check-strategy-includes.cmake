# Asserts that <algotradelib/strategy> pulls in no broker, network, or transport header.
#
# This is the property that makes one strategy source compile against every broker, and it is
# checked rather than trusted because it degrades silently: adding one convenience include to a
# strategy header would couple every strategy to a vendor, and nothing would fail until someone
# tried to build the same source against the other broker.
#
# Run through the compiler's own dependency output rather than by grepping source text, so a
# header reached indirectly is caught as surely as one included directly.

if(NOT DEFINED ATL_COMPILER OR NOT DEFINED ATL_INCLUDE_DIR OR NOT DEFINED ATL_WORK_DIR)
    message(FATAL_ERROR "ATL_COMPILER, ATL_INCLUDE_DIR and ATL_WORK_DIR must be set")
endif()

file(MAKE_DIRECTORY "${ATL_WORK_DIR}")
set(probe "${ATL_WORK_DIR}/strategy-include-probe.cpp")
file(WRITE "${probe}" "#include <algotradelib/strategy>\nint main() { return 0; }\n")

set(compile_flags -std=c++17 -M -MF "${ATL_WORK_DIR}/strategy-includes.d" "-I${ATL_INCLUDE_DIR}")
foreach(extra IN LISTS ATL_EXTRA_INCLUDES)
    if(extra)
        list(APPEND compile_flags "-I${extra}")
    endif()
endforeach()

execute_process(
    COMMAND "${ATL_COMPILER}" ${compile_flags} -c "${probe}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "could not read the include graph:\n${output}\n${errors}")
endif()

file(READ "${ATL_WORK_DIR}/strategy-includes.d" graph)
string(REPLACE "\\" ";" graph "${graph}")
string(REPLACE "\n" ";" graph "${graph}")
string(REPLACE " " ";" graph "${graph}")

# Each entry is a header the strategy surface reaches, directly or not. A match on any of these
# means the surface has acquired a dependency it must not have.
set(forbidden
    "src/brokers/"       # any vendor adapter
    "src/network/"       # transport, TLS, WebSocket
    "boost/beast"
    "boost/asio"
    "openssl/"
)

set(violations "")
foreach(entry IN LISTS graph)
    string(STRIP "${entry}" entry)
    if(entry STREQUAL "" OR entry MATCHES ":$")
        continue()
    endif()
    foreach(pattern IN LISTS forbidden)
        if(entry MATCHES "${pattern}")
            list(APPEND violations "${entry} (matched ${pattern})")
        endif()
    endforeach()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    string(REPLACE ";" "\n  " report "${violations}")
    message(FATAL_ERROR
        "<algotradelib/strategy> reaches headers it must not:\n  ${report}\n"
        "A strategy must name no broker and perform no I/O. If one of these is genuinely needed, "
        "it belongs in the runner, which is the only component that holds a session.")
endif()

# And a positive check, so the test cannot pass by reading an empty graph.
set(found_surface FALSE)
foreach(entry IN LISTS graph)
    if(entry MATCHES "src/strategy/intent.hpp")
        set(found_surface TRUE)
    endif()
endforeach()
if(NOT found_surface)
    message(FATAL_ERROR "the include graph does not mention the strategy headers; the probe "
                        "compiled nothing useful and the check would pass vacuously")
endif()

message(STATUS "strategy surface include graph is clean")
