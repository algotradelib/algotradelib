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
# What CMake read out of version.hpp, so a test can compare it with what the preprocessor sees. The
# project-named variable rather than PROJECT_VERSION, which a project() in a fetched dependency would
# shadow within its own directory.
target_compile_definitions(algotradelib-core-tests PRIVATE
    ATL_TEST_PROJECT_VERSION="${algotradelib_VERSION}"
)

add_executable(algotradelib-broker-port-tests test-broker-port.cpp)
target_link_libraries(algotradelib-broker-port-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-capability-conformance-tests test-capability-conformance.cpp)
target_link_libraries(algotradelib-capability-conformance-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-data-only-tests test-data-only-traits.cpp)
target_link_libraries(algotradelib-data-only-tests PRIVATE
    algotradelib::algotradelib
    Catch2::Catch2WithMain
)

add_executable(algotradelib-kiwoom-tests test-kiwoom.cpp)
target_link_libraries(algotradelib-kiwoom-tests PRIVATE
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
# The live program's broker-free wiring lives beside the example that uses it, and is tested here
# against the in-memory session.
target_include_directories(algotradelib-live-tests PRIVATE "${algotradelib_SOURCE_DIR}/examples")

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
catch_discover_tests(algotradelib-capability-conformance-tests)
catch_discover_tests(algotradelib-data-only-tests)
catch_discover_tests(algotradelib-kiwoom-tests)
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

# A traits specialization that claims a capability with no verb behind it must fail `ATL_ASSERT_BROKER`,
# and the failure must name what is missing. Built by the test rather than by `all`, because it is meant
# not to build; the regular expression is the diagnostic, so a check that no longer fires fails this test
# rather than passing it silently. Each expression covers Clang's and GCC's wording.
add_library(algotradelib-conformance-probe OBJECT EXCLUDE_FROM_ALL conformance-probe.cpp)
target_link_libraries(algotradelib-conformance-probe PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-names-an-unbacked-capability
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe
            --config "$<CONFIG>"
)
set_tests_properties(conformance-names-an-unbacked-capability PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'instrument' in [^\n]*ClaimsReferenceData|'instrument' is not a member of [^\n]*ClaimsReferenceData"
    TIMEOUT 300
)

# A market-data source (DataOnlyTraits) wired where orders go must not build, and the failure must be the
# assertion that says so: as the executor's session, and as SessionSource's trading session.
add_library(algotradelib-data-only-probe-executor OBJECT EXCLUDE_FROM_ALL data-only-probe-executor.cpp)
target_link_libraries(algotradelib-data-only-probe-executor PRIVATE algotradelib::algotradelib)
add_test(
    NAME data-only-refused-as-executor
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-data-only-probe-executor
            --config "$<CONFIG>"
)
set_tests_properties(data-only-refused-as-executor PROPERTIES
    PASS_REGULAR_EXPRESSION "SessionExecutor sends orders, and this session's client is a market-data source"
    TIMEOUT 300
)
add_library(algotradelib-data-only-probe-source OBJECT EXCLUDE_FROM_ALL data-only-probe-source.cpp)
target_link_libraries(algotradelib-data-only-probe-source PRIVATE algotradelib::algotradelib)
add_test(
    NAME data-only-refused-as-trading-session
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-data-only-probe-source
            --config "$<CONFIG>"
)
set_tests_properties(data-only-refused-as-trading-session PROPERTIES
    PASS_REGULAR_EXPRESSION "SessionSource's first session is the trading session"
    TIMEOUT 300
)

# An asynchronous thunk under the right name with the wrong signature must fail `ATL_ASSERT_BROKER`, which
# calls it.
add_library(algotradelib-conformance-probe-verbs OBJECT EXCLUDE_FROM_ALL conformance-probe-verbs.cpp)
target_link_libraries(algotradelib-conformance-probe-verbs PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-verbs-reject-a-wrong-signature
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-verbs
            --config "$<CONFIG>"
)
set_tests_properties(conformance-verbs-reject-a-wrong-signature PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching[a-z ]*function for call to[^\n]*instrument"
    TIMEOUT 300
)

# A thunk that copies its token works for every copyable token and not for a move-only one, which
# a caller may pass; `ATL_ASSERT_BROKER` calls every verb with one, so it must fail here.
add_library(algotradelib-conformance-probe-move-only OBJECT EXCLUDE_FROM_ALL conformance-probe-move-only.cpp)
target_link_libraries(algotradelib-conformance-probe-move-only PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-verbs-reject-a-token-copy
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-move-only
            --config "$<CONFIG>"
)
set_tests_properties(conformance-verbs-reject-a-token-copy PROPERTIES
    PASS_REGULAR_EXPRESSION "deleted (copy )?constructor[^\n]*MoveOnlyCallback|MoveOnlyCallback[^\n]*deleted|use of deleted function[^\n]*MoveOnlyCallback"
    TIMEOUT 300
)

# A blocking account listing whose rows a range-for cannot walk must fail `ATL_ASSERT_BROKER`, which calls
# the real `list_accounts`.
add_library(algotradelib-conformance-probe-verbs-accounts OBJECT EXCLUDE_FROM_ALL conformance-probe-verbs-accounts.cpp)
target_link_libraries(algotradelib-conformance-probe-verbs-accounts PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-verbs-reject-unwalkable-account-rows
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-verbs-accounts
            --config "$<CONFIG>"
)
set_tests_properties(conformance-verbs-reject-unwalkable-account-rows PROPERTIES
    PASS_REGULAR_EXPRESSION "broker_session.hpp:[0-9]+:[0-9]+: error: (invalid operands to binary expression|no match for .operator!=)"
    TIMEOUT 300
)

# An adapter that leaves one capability unanswered must fail `ATL_ASSERT_BROKER`, naming it, even when no
# verb is gated on it: there is no default.
add_library(algotradelib-conformance-probe-constant OBJECT EXCLUDE_FROM_ALL conformance-probe-constant.cpp)
target_link_libraries(algotradelib-conformance-probe-constant PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-requires-every-capability
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-constant
            --config "$<CONFIG>"
)
set_tests_properties(conformance-requires-every-capability PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'has_history_cursor'|'has_history_cursor' is not a member"
    TIMEOUT 300
)

# A call to a verb the broker does not claim, made without a capability gate, must not compile, and the
# failure must be the session's assertion naming the capability to gate on.
add_library(algotradelib-conformance-probe-ungated OBJECT EXCLUDE_FROM_ALL conformance-probe-ungated.cpp)
target_link_libraries(algotradelib-conformance-probe-ungated PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-refuses-an-ungated-call
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-ungated
            --config "$<CONFIG>"
)
set_tests_properties(conformance-refuses-an-ungated-call PROPERTIES
    PASS_REGULAR_EXPRESSION "gate the call with BrokerCaps<Client>::has_depth"
    TIMEOUT 300
)

# A hand-written capability set that does not say whether its client takes orders must not be admitted
# as the executor's session: the binding check reads `data_only` directly.
add_library(algotradelib-conformance-probe-caps-data-only OBJECT EXCLUDE_FROM_ALL conformance-probe-caps-data-only.cpp)
target_link_libraries(algotradelib-conformance-probe-caps-data-only PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-caps-must-state-data-only
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-caps-data-only
            --config "$<CONFIG>"
)
set_tests_properties(conformance-caps-must-state-data-only PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'data_only' in [^\n]*SilentCaps|'data_only' is not a member of [^\n]*SilentCaps"
    TIMEOUT 300
)

# The same for an adapter's traits, read through `BrokerCaps`: one that does not say whether its client
# takes orders must not be admitted as the executor's session.
add_library(algotradelib-conformance-probe-traits-data-only OBJECT EXCLUDE_FROM_ALL conformance-probe-traits-data-only.cpp)
target_link_libraries(algotradelib-conformance-probe-traits-data-only PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-traits-must-state-data-only
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-traits-data-only
            --config "$<CONFIG>"
)
set_tests_properties(conformance-traits-must-state-data-only PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'data_only' in [^\n]*SilentAboutDataOnly|'data_only' is not a member of [^\n]*SilentAboutDataOnly"
    TIMEOUT 300
)

# A constant asked only of some brokers is required of those: an adapter that lists working orders must
# say whether a row can state a terminal status.
add_library(algotradelib-conformance-probe-asked-terminal OBJECT EXCLUDE_FROM_ALL conformance-probe-asked-terminal.cpp)
target_link_libraries(algotradelib-conformance-probe-asked-terminal PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-requires-an-asked-capability
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-asked-terminal
            --config "$<CONFIG>"
)
set_tests_properties(conformance-requires-an-asked-capability PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'working_orders_state_terminal' in [^\n]*SilentAboutTerminal|'working_orders_state_terminal' is not a member of [^\n]*SilentAboutTerminal"
    TIMEOUT 300
)

# And a capability set that makes the sellable inquiry must say what the figure covers.
add_library(algotradelib-conformance-probe-asked-basis OBJECT EXCLUDE_FROM_ALL conformance-probe-asked-basis.cpp)
target_link_libraries(algotradelib-conformance-probe-asked-basis PRIVATE algotradelib::algotradelib)
add_test(
    NAME conformance-requires-an-asked-basis
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-conformance-probe-asked-basis
            --config "$<CONFIG>"
)
set_tests_properties(conformance-requires-an-asked-basis PROPERTIES
    PASS_REGULAR_EXPRESSION "no member named 'sellable_inquiry_basis' in [^\n]*InquiresWithoutBasis|'sellable_inquiry_basis' is not a member of [^\n]*InquiresWithoutBasis"
    TIMEOUT 300
)

# A strategy handler that is declared and that the runner's call does not fit must not build: `dispatch` calls
# every declared handler, so the compiler refuses the call. One probe per shape, each with a valid `on_bar`,
# each failing for one reason only, in the compiler's words (Clang's and GCC's).
add_library(algotradelib-strategy-probe-nonconst-fill OBJECT EXCLUDE_FROM_ALL strategy-probe-nonconst-fill.cpp)
target_link_libraries(algotradelib-strategy-probe-nonconst-fill PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-nonconst-fill
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-nonconst-fill
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-nonconst-fill PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_fill"
    TIMEOUT 300
)
add_library(algotradelib-strategy-probe-wrong-update-event OBJECT EXCLUDE_FROM_ALL strategy-probe-wrong-update-event.cpp)
target_link_libraries(algotradelib-strategy-probe-wrong-update-event PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-wrong-update-event
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-wrong-update-event
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-wrong-update-event PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_order_update"
    TIMEOUT 300
)
add_library(algotradelib-strategy-probe-missing-phase-event OBJECT EXCLUDE_FROM_ALL strategy-probe-missing-phase-event.cpp)
target_link_libraries(algotradelib-strategy-probe-missing-phase-event PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-missing-phase-event
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-missing-phase-event
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-missing-phase-event PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_phase"
    TIMEOUT 300
)
add_library(algotradelib-strategy-probe-mutable-phase OBJECT EXCLUDE_FROM_ALL strategy-probe-mutable-phase.cpp)
target_link_libraries(algotradelib-strategy-probe-mutable-phase PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-mutable-phase
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-mutable-phase
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-mutable-phase PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_phase"
    TIMEOUT 300
)

# A handler taking its context by value cannot be called, because no context can be copied.
add_library(algotradelib-strategy-probe-context-by-value OBJECT EXCLUDE_FROM_ALL strategy-probe-context-by-value.cpp)
target_link_libraries(algotradelib-strategy-probe-context-by-value PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-context-by-value
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-context-by-value
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-context-by-value PROPERTIES
    PASS_REGULAR_EXPRESSION "call to deleted constructor of[^\n]*ProbeContextT|use of deleted function[^\n]*ProbeContextT"
    TIMEOUT 300
)
add_library(algotradelib-strategy-probe-runner-context-by-value OBJECT EXCLUDE_FROM_ALL strategy-probe-runner-context-by-value.cpp)
target_link_libraries(algotradelib-strategy-probe-runner-context-by-value PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-runner-context-by-value
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-runner-context-by-value
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-runner-context-by-value PROPERTIES
    PASS_REGULAR_EXPRESSION "call to deleted constructor of[^\n]*Context<|use of deleted function[^\n]*Context<"
    TIMEOUT 300
)

# A final strategy cannot be checked for handlers the runner would skip, so it is refused.
add_library(algotradelib-strategy-probe-final OBJECT EXCLUDE_FROM_ALL strategy-probe-final.cpp)
target_link_libraries(algotradelib-strategy-probe-final PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-final-strategy
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-final
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-final-strategy PROPERTIES
    PASS_REGULAR_EXPRESSION "a final strategy cannot be checked for a handler the runner would skip"
    TIMEOUT 300
)

# A wrapper forwarding through runner::dispatch gets its inner strategy checked too.
add_library(algotradelib-strategy-probe-wrapped-malformed OBJECT EXCLUDE_FROM_ALL strategy-probe-wrapped-malformed.cpp)
target_link_libraries(algotradelib-strategy-probe-wrapped-malformed PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-wrapped-malformed-handler
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-wrapped-malformed
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-wrapped-malformed-handler PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_phase"
    TIMEOUT 300
)

# And a final inner strategy is refused inside dispatch, not read as declaring nothing.
add_library(algotradelib-strategy-probe-wrapped-final OBJECT EXCLUDE_FROM_ALL strategy-probe-wrapped-final.cpp)
target_link_libraries(algotradelib-strategy-probe-wrapped-final PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-wrapped-final-strategy
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-wrapped-final
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-wrapped-final-strategy PROPERTIES
    PASS_REGULAR_EXPRESSION "a final strategy cannot be checked for a handler the runner would skip"
    TIMEOUT 300
)

# A handler with the wrong event that nothing calls yet fails at the assertion, which compiles every call.
add_library(algotradelib-strategy-probe-unreached-handler OBJECT EXCLUDE_FROM_ALL strategy-probe-unreached-handler.cpp)
target_link_libraries(algotradelib-strategy-probe-unreached-handler PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-an-unreached-handler
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-unreached-handler
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-an-unreached-handler PROPERTIES
    PASS_REGULAR_EXPRESSION "no matching (member )?function for call to[^\n]*on_quote"
    TIMEOUT 300
)

# Every declared handler's body is compiled by the assertion, even for an event nothing feeds: one probe per
# handler, the error in that handler's body alone.
foreach(handler RANGE 1 7)
    add_library(algotradelib-strategy-probe-handler-body-${handler} OBJECT EXCLUDE_FROM_ALL strategy-probe-handler-body.cpp)
    target_link_libraries(algotradelib-strategy-probe-handler-body-${handler} PRIVATE algotradelib::algotradelib)
    target_compile_definitions(algotradelib-strategy-probe-handler-body-${handler} PRIVATE ATL_PROBE_HANDLER=${handler})
    add_test(
        NAME strategy-compiles-handler-body-${handler}
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-handler-body-${handler}
                --config "$<CONFIG>"
    )
    set_tests_properties(strategy-compiles-handler-body-${handler} PROPERTIES
        PASS_REGULAR_EXPRESSION "no member named 'no_such_member'"
        TIMEOUT 300
    )
endforeach()

# A context a caller can copy is refused: a by-value handler would submit to a copy.
add_library(algotradelib-strategy-probe-copyable-context OBJECT EXCLUDE_FROM_ALL strategy-probe-copyable-context.cpp)
target_link_libraries(algotradelib-strategy-probe-copyable-context PRIVATE algotradelib::algotradelib)
add_test(
    NAME strategy-refuses-a-copyable-context
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target algotradelib-strategy-probe-copyable-context
            --config "$<CONFIG>"
)
set_tests_properties(strategy-refuses-a-copyable-context PROPERTIES
    PASS_REGULAR_EXPRESSION "a strategy context must not be copyable"
    TIMEOUT 300
)

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
