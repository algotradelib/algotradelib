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
# second matched the row loosely with `$NF`, and review found it passed for a line reading
# `MyStrategy(5) skipped 40` -- which contains no report at all -- and for vanillas printed as bare
# names with no figures. Both are the cooperating guard `ai/wall-clock-test-assertions` set a rule
# about, found twice in the same file.
#
# So a row is validated structurally: the strategy name as the first field, a numeric fill count as
# the last, and every field between them one of a number, a `%`, `undefined` or `--`, which are the
# only things `percent()` and `plain()` can emit. A word like `skipped` fails, a short line fails, and
# a bare name fails. Written to accept `undefined` and `--` on purpose: a strategy that never trades
# has an undefined Sharpe, and this test should not be what makes that confusing to discover.
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

    # The sufficient check: the program must reproduce, exactly, the table the README publishes.
    #
    # Nothing read from stdout can distinguish "replayed and printed" from "printed", so no assertion
    # about a row's *shape* can ever be enough -- review defeated the last one with a `compare()` that
    # printed `name 0 % 0 0 % 1` and replayed nothing, and it would defeat any successor. What cannot
    # be forged without doing the work is the figures themselves: -10.22, -0.375, 13.11, 2, and nine
    # more. A mutation that reproduces those has run the replay.
    #
    # The expected values are read from README.md rather than written here, so there is one copy and
    # not two. That makes the coupling deliberate: if the fill model changes, this fails, and it
    # *should* fail, because the README publishes this exact table as what a consumer will see. The
    # test failing is the mechanism that stops the documentation going stale -- the same reason the
    # scaffold copies the example instead of carrying its own template. My earlier note in this file
    # argued the opposite, that pinning figures would make a legitimate fill-model change fail a test
    # about onboarding. That was wrong, and it is what let a forged table survive two review rounds.
    EXPECTED=$(awk '
        /^  strategy  *return/ { table = 1; next }
        table && /^  -+$/      { next }
        table && /^  [A-Za-z]/ { gsub(/^ +| +$/, ""); gsub(/  +/, " "); print; next }
        table                  { table = 0 }
    ' "$SOURCE_DIR/README.md")

    EXPECTED_COUNT=$(printf '%s\n' "$EXPECTED" | grep -c '[A-Za-z]' || true)
    if [ "$EXPECTED_COUNT" -lt 4 ]; then
        printf 'found %s strategy rows in README.md, wanted at least 4 -- has the published table been removed?\n' \
            "$EXPECTED_COUNT" >&2
        exit 1
    fi

    NORMALIZED=$(printf '%s\n' "$OUTPUT" | sed 's/^ *//; s/ *$//; s/  */ /g')
    printf '%s\n' "$EXPECTED" | while IFS= read -r want; do
        [ -n "$want" ] || continue
        if ! printf '%s\n' "$NORMALIZED" | grep -Fqx "$want"; then
            printf 'the generated program did not reproduce the row README.md publishes:\n  %s\n' \
                "$want" >&2
            exit 1
        fi
    done || exit 1
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
