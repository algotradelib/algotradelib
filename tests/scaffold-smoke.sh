#!/bin/sh
#
# Runs tools/atl-new-project.sh end to end: scaffold, configure, build, run, check the output.
#
# This is the test that makes the onboarding documentation durable. A README snippet rots silently --
# nothing compiles it, so it stays wrong until a new consumer finds out. A scaffold whose output is
# built and executed by the suite cannot: if the generated CMakeLists stops configuring, or the
# example stops compiling, or the strategy stops trading, this fails.
#
# It deliberately builds the *generated* project rather than asserting on the generated text. What a
# new consumer relies on is that the thing builds and runs, and that is the property observed here.
#
# Usage: scaffold-smoke.sh <library-source-dir> <work-dir> <cmake> <ON|OFF backend> [cmake args...]

set -eu

SOURCE_DIR=${1:?library source directory}
WORK_DIR=${2:?work directory}
CMAKE=${3:?cmake executable}
# Whether this build has a linear algebra backend, which decides what the generated program can
# print. Passed in rather than sniffed, so the test cannot quietly assert the weaker thing.
BACKEND=${4:?ON or OFF for the linear algebra backend}
shift 4

PROJECT=$WORK_DIR/scaffolded

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

printf 'scaffolding into %s\n' "$PROJECT"
sh "$SOURCE_DIR/tools/atl-new-project.sh" --name scaffold_smoke "$PROJECT"

for expected in CMakeLists.txt src/main.cpp README.md .gitignore; do
    if [ ! -f "$PROJECT/$expected" ]; then
        printf 'scaffold did not write %s\n' "$expected" >&2
        exit 1
    fi
done

# The generated main.cpp must be the example this repository compiles, byte for byte. If the two ever
# drift, the scaffold is shipping a copy nothing builds -- which is exactly the failure the
# copy-a-real-file design exists to prevent, so it is asserted rather than assumed.
if ! cmp -s "$SOURCE_DIR/examples/example-backtest-compare.cpp" "$PROJECT/src/main.cpp"; then
    printf 'generated src/main.cpp differs from examples/example-backtest-compare.cpp\n' >&2
    exit 1
fi

printf 'configuring\n'
"$CMAKE" -S "$PROJECT" -B "$PROJECT/build" -DALGOTRADELIB_DIR="$SOURCE_DIR" "$@"

printf 'building\n'
"$CMAKE" --build "$PROJECT/build"

# Searched rather than assumed. A multi-configuration generator (Xcode, Visual Studio, Ninja
# Multi-Config) puts the executable under a per-configuration directory -- `build/Debug/`, or
# `build/bin/Release/` -- and this script forwards the parent generator, so it can be handed one.
BINARY=$(find "$PROJECT/build" -name scaffold_smoke -type f -perm -u+x 2>/dev/null | head -n 1)
if [ -z "$BINARY" ]; then
    printf 'built no runnable scaffold_smoke under %s/build\n' "$PROJECT" >&2
    exit 1
fi

printf 'running\n'
OUTPUT=$("$BINARY")
printf '%s\n' "$OUTPUT"

# What the generated program can print depends on the backend, and both cases are checked rather than
# one being skipped -- a consumer building without Eigen relies on the scaffold working too, and the
# example's no-backend branch is otherwise never executed anywhere.
#
# **A strategy's name appearing is not evidence that it ran**, and two earlier versions of this check
# believed it was. The first matched `MyStrategy` followed by a digit, and passed when the strategy was
# mutated to return before reading anything, because the row still printed `0.00 %` and `0` fills. The
# second matched the row loosely with `$NF`, and it passed for a line reading
# `MyStrategy(5) skipped 40` -- which contains no report at all -- and for vanillas printed as bare
# names with no figures. Both are a guard that cooperates with the code it checks.
#
# So a row is validated structurally: the strategy name as the first field, a numeric fill count as
# the last, and every field between them one of a number, a `%`, `undefined` or `--`, which are the
# only things `percent()` and `plain()` can emit. A word like `skipped` fails, a short line fails, and
# a bare name fails. Written to accept `undefined` and `--` on purpose: a strategy that never trades
# has an undefined Sharpe, and this test should not be what makes that confusing to discover.
# Reads a comparison table from stdin and prints its strategy rows, whitespace-normalized.
#
# Used on the documentation main page (doxygen/index.md) and on the generated program's output, so the
# two are parsed by the same code and cannot disagree about what counts as a row. Tabs are normalized alongside spaces: the committed
# printer emits spaces, and a harmless change to that should not be a test failure about onboarding.
table_rows() {
    awk '
        { sub(/\r$/, "") }
        /^[ \t]*strategy[ \t]+return/ { table = 1; next }
        table && /^[ \t]*-+[ \t]*$/   { next }
        table && /^[ \t]*[A-Za-z]/ {
            gsub(/\r/, "")
            gsub(/^[ \t]+|[ \t]+$/, "")
            gsub(/[ \t]+/, " ")
            print
            next
        }
        table { table = 0 }
    '
}

# Prints any row of a normalized table that is not a complete report row. Empty output means all are.
#
# The same field rules `report_row_fills` applies to one row, applied to a file: name first, integer
# fill count last, and only figures, `%`, `undefined` or `--` between.
invalid_rows() {
    awk '
        {
            if (NF < 5) { print; next }
            if ($NF !~ /^[0-9]+$/) { print; next }
            for (field = 2; field < NF; ++field) {
                if ($field !~ /^-?[0-9]+(\.[0-9]+)?$/ &&
                    $field != "%" && $field != "undefined" && $field != "--") {
                    print
                    next
                }
            }
        }
    ' "$1"
}

# Asserts that a parsed table has exactly `want` rows, one per distinct strategy, each a complete
# report row.
#
# Applied to all three tables -- the main page's, the program's, and the perturbed program's -- because
# checking only some of them is how this file kept shipping holes. The perturbed side once passed
# `MyStrategy(5) incomplete` and a duplicated row, both of which the baseline side rejected; the rule
# existed and had not reached every place it applied.
assert_valid_table() {
    table=$1
    label=$2
    want=$3

    rows=$(grep -c '[A-Za-z]' "$table" || true)
    distinct=$(awk '{ print $1 }' "$table" | sort -u | grep -c '[A-Za-z]' || true)

    if [ "$rows" -ne "$want" ]; then
        printf '%s produced %s strategy rows, wanted %s\n' "$label" "$rows" "$want" >&2
        cat "$table" >&2
        exit 1
    fi
    if [ "$distinct" -ne "$rows" ]; then
        printf '%s produced %s rows but only %s distinct strategies\n' "$label" "$rows" "$distinct" >&2
        cat "$table" >&2
        exit 1
    fi

    malformed=$(invalid_rows "$table")
    if [ -n "$malformed" ]; then
        printf '%s produced a line that is not a complete report row:\n%s\n' "$label" "$malformed" >&2
        exit 1
    fi
}

report_row_fills() {
    awk -v want="$1" '
        $1 == want {
            found = 1
            if (NF < 5) { exit 3 }
            if ($NF !~ /^[0-9]+$/) { exit 4 }
            for (field = 2; field < NF; ++field) {
                if ($field !~ /^-?[0-9]+(\.[0-9]+)?$/ &&
                    $field != "%" && $field != "undefined" && $field != "--") {
                    exit 5
                }
            }
            print $NF
            exit 0
        }
        END { if (!found) exit 2 }
    '
}

check_row() {
    name=$1
    minimum=$2
    fills=$(printf '%s\n' "$OUTPUT" | report_row_fills "$name") || status=$?
    case "${status:-0}" in
        0) ;;
        2) printf 'generated program printed no row for %s\n' "$name" >&2; exit 1 ;;
        3) printf '%s printed a line with too few fields to be a report row\n' "$name" >&2; exit 1 ;;
        4) printf '%s row did not end in a numeric fill count\n' "$name" >&2; exit 1 ;;
        5) printf '%s row carried a field that is not a figure\n' "$name" >&2; exit 1 ;;
        *) printf 'could not read the %s row\n' "$name" >&2; exit 1 ;;
    esac
    unset status
    if [ "$fills" -lt "$minimum" ]; then
        printf '%s filled %s orders, wanted at least %s: the scaffold ships a comparison that did not happen\n' \
            "$name" "$fills" "$minimum" >&2
        exit 1
    fi
}

if [ "$BACKEND" = ON ]; then
    # Every strategy must have produced a structurally valid row. This is the first check only because
    # it gives a precise diagnostic; on its own it is not sufficient, and believing it was is the
    # mistake this file has now made three times.
    check_row 'MyStrategy(5)' 1
    check_row 'BuyAndHold' 1
    check_row 'SmaCrossover(3,8)' 0
    check_row 'DonchianBreakout(5)' 0

    # The program's table must equal the table doxygen/index.md publishes, as a set: same rows, same
    # count, nothing missing and nothing extra.
    #
    # **What this does and does not establish.** It is an output regression check, not proof that a
    # replay ran -- a program can print any literal. No assertion
    # made by reading stdout can distinguish computing a number from printing one, so this test does
    # not claim to. What it does is make a forgery reproduce every published figure, which is a great
    # deal harder than printing a plausible shape, and it is the strongest thing available from out
    # here. A test that misdescribes itself is how the next person gets talked out of a real one.
    #
    # Compared as a set rather than row by row, because the row-by-row version accepted an output that
    # printed the four rows twice, and one that added a fifth strategy the main page never published.
    # The expected side is checked for distinct names too: four copies of one published row would
    # otherwise satisfy a count of four and leave three published comparisons untested.
    PUBLISHED_ROWS=$WORK_DIR/expected-table.txt
    OUTPUT_ROWS=$WORK_DIR/actual-table.txt
    table_rows < "$SOURCE_DIR/doxygen/index.md" | sort > "$PUBLISHED_ROWS"
    printf '%s\n' "$OUTPUT" | table_rows | sort > "$OUTPUT_ROWS"

    EXPECTED_COUNT=$(grep -c '[A-Za-z]' "$PUBLISHED_ROWS" || true)
    if [ "$EXPECTED_COUNT" -lt 4 ]; then
        printf 'found %s strategy rows in doxygen/index.md, wanted at least 4 -- has the published table been removed?\n' \
            "$EXPECTED_COUNT" >&2
        exit 1
    fi
    assert_valid_table "$PUBLISHED_ROWS" 'doxygen/index.md' "$EXPECTED_COUNT"
    assert_valid_table "$OUTPUT_ROWS" 'the generated program' "$EXPECTED_COUNT"

    if ! cmp -s "$PUBLISHED_ROWS" "$OUTPUT_ROWS"; then
        printf 'the generated program did not print the table doxygen/index.md publishes\n' >&2
        printf -- '--- doxygen/index.md ---\n' >&2
        cat "$PUBLISHED_ROWS" >&2
        printf -- '--- generated program ---\n' >&2
        cat "$OUTPUT_ROWS" >&2
        exit 1
    fi

    # Regression evidence that the table responds to the strategy, rather than being printed at it.
    #
    # Everything above compares output to expected output, and an executable that printed those four
    # rows and did nothing else would satisfy all of it. This perturbs the program -- `MyStrategy`'s
    # lookback from 5 to 6 -- rebuilds, and requires the table to respond in the one way that change
    # can justify: the `MyStrategy` row differs, and the other three are untouched, because nothing
    # about that edit may reach `BuyAndHold` or either indicator vanilla.
    #
    # **Still not proof that a replay ran.** A
    # printer whose literals vary with its own source would satisfy this too. What it rules out is the
    # simplest forgery -- fixed literals -- and it does so by requiring a
    # specific, asymmetric response rather than merely a different one.
    PERTURBED_STRATEGY='MyStrategy(5)'
    PERTURBED_PROJECT=$WORK_DIR/perturbed
    rm -rf "$PERTURBED_PROJECT"
    cp -R "$PROJECT" "$PERTURBED_PROJECT"
    rm -rf "$PERTURBED_PROJECT/build"

    PERTURBED_SOURCE=$PERTURBED_PROJECT/src/main.cpp
    sed 's/lookback_{5}/lookback_{6}/' "$PERTURBED_SOURCE" > "$PERTURBED_SOURCE.new"
    if cmp -s "$PERTURBED_SOURCE" "$PERTURBED_SOURCE.new"; then
        printf 'could not perturb the example: no lookback_{5} in %s, so this check would prove nothing\n' \
            "$PERTURBED_SOURCE" >&2
        exit 1
    fi
    mv "$PERTURBED_SOURCE.new" "$PERTURBED_SOURCE"

    printf 'rebuilding with a perturbed strategy\n'
    "$CMAKE" -S "$PERTURBED_PROJECT" -B "$PERTURBED_PROJECT/build" -DALGOTRADELIB_DIR="$SOURCE_DIR" "$@"
    "$CMAKE" --build "$PERTURBED_PROJECT/build"
    PERTURBED_BINARY=$(find "$PERTURBED_PROJECT/build" -name scaffold_smoke -type f -perm -u+x 2>/dev/null | head -n 1)
    [ -n "$PERTURBED_BINARY" ] || { printf 'perturbed build produced no binary\n' >&2; exit 1; }

    # Run to a file, and check the status, before anything parses it. Piping the executable straight
    # into `table_rows | sort` made the pipeline report *sort's* status: a perturbed
    # binary that printed one row and exited 42, and the script passed, because a truncated table is
    # trivially "different from the published one" -- which was the only thing being asked.
    PERTURBED_OUTPUT=$WORK_DIR/perturbed-output.txt
    if ! "$PERTURBED_BINARY" > "$PERTURBED_OUTPUT"; then
        printf 'the perturbed program exited non-zero; its output proves nothing\n' >&2
        cat "$PERTURBED_OUTPUT" >&2
        exit 1
    fi

    PERTURBED_ROWS=$WORK_DIR/perturbed-table.txt
    table_rows < "$PERTURBED_OUTPUT" | sort > "$PERTURBED_ROWS"
    if [ ! -s "$PERTURBED_ROWS" ]; then
        printf 'perturbed program printed no table at all\n' >&2
        exit 1
    fi

    # The perturbed table is held to the same *structural* rules as the baseline: the right number of
    # rows, one per distinct strategy, each a complete report row. It is not held to the baseline's
    # minimum-fill checks, which belong to the published figures rather than to a perturbed run. A
    # perturbed run printing `MyStrategy(5) incomplete`, or an extra copy of a row, passed everything
    # here before this was added.
    assert_valid_table "$PERTURBED_ROWS" 'the perturbed program' "$EXPECTED_COUNT"

    # ...and it must still be comparing the same strategies, not four different ones.
    awk '{ print $1 }' "$PUBLISHED_ROWS" | sort -u > "$WORK_DIR/names-expected.txt"
    awk '{ print $1 }' "$PERTURBED_ROWS" | sort -u > "$WORK_DIR/names-perturbed.txt"
    if ! cmp -s "$WORK_DIR/names-expected.txt" "$WORK_DIR/names-perturbed.txt"; then
        printf 'the perturbed program did not compare the same strategies\n' >&2
        printf -- '--- expected ---\n' >&2; cat "$WORK_DIR/names-expected.txt" >&2
        printf -- '--- got ---\n' >&2;      cat "$WORK_DIR/names-perturbed.txt" >&2
        exit 1
    fi

    # The response has to be asymmetric: only the perturbed strategy may move.
    grep -v "^$PERTURBED_STRATEGY " "$PUBLISHED_ROWS" > "$WORK_DIR/others-expected.txt" || true
    grep -v "^$PERTURBED_STRATEGY " "$PERTURBED_ROWS" > "$WORK_DIR/others-perturbed.txt" || true
    if ! cmp -s "$WORK_DIR/others-expected.txt" "$WORK_DIR/others-perturbed.txt"; then
        printf 'perturbing %s changed another strategy'"'"'s row, which nothing about that edit can justify\n' \
            "$PERTURBED_STRATEGY" >&2
        exit 1
    fi

    grep "^$PERTURBED_STRATEGY " "$PUBLISHED_ROWS" > "$WORK_DIR/mine-expected.txt" || true
    grep "^$PERTURBED_STRATEGY " "$PERTURBED_ROWS" > "$WORK_DIR/mine-perturbed.txt" || true
    if [ ! -s "$WORK_DIR/mine-perturbed.txt" ]; then
        printf 'the perturbed program printed no %s row\n' "$PERTURBED_STRATEGY" >&2
        exit 1
    fi
    if cmp -s "$WORK_DIR/mine-expected.txt" "$WORK_DIR/mine-perturbed.txt"; then
        printf 'changing %s lookback from 5 to 6 left its row identical: the figures do not come from the strategy\n' \
            "$PERTURBED_STRATEGY" >&2
        exit 1
    fi
else
    # Without a backend there is no report and no indicator vanilla to compare against, so the program
    # states what it replayed instead. Read with a pattern rather than a field offset, so rewording the
    # sentence around the number cannot silently stop this from checking anything.
    if printf '%s\n' "$OUTPUT" | grep -q 'SmaCrossover'; then
        printf 'no backend, yet the program printed an indicator strategy\n' >&2
        exit 1
    fi
    FILLS=$(printf '%s\n' "$OUTPUT" | sed -n 's/.*MyStrategy replayed [0-9][0-9]* bars and filled \([0-9][0-9]*\) orders.*/\1/p' | head -n 1)
    if [ -z "$FILLS" ]; then
        printf 'generated program reported no MyStrategy fill count\n' >&2
        exit 1
    fi
    if [ "$FILLS" -lt 1 ]; then
        printf 'MyStrategy filled %s orders: the scaffold ships a strategy that does not trade\n' \
            "$FILLS" >&2
        exit 1
    fi
fi

printf 'scaffold smoke test passed\n'
