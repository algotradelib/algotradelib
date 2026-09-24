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

BINARY=$PROJECT/build/scaffold_smoke
[ -x "$BINARY" ] || BINARY=$PROJECT/build/bin/scaffold_smoke
if [ ! -x "$BINARY" ]; then
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
# Checked on the strategies and the fill count rather than on the figures. The numbers are
# reproducible, but pinning them here would make every legitimate change to the fill model fail a test
# about onboarding, in a file nobody would think to look in.
if [ "$BACKEND" = ON ]; then
    for strategy in MyStrategy BuyAndHold SmaCrossover DonchianBreakout; do
        if ! printf '%s\n' "$OUTPUT" | grep -q "$strategy"; then
            printf 'generated program printed no %s row\n' "$strategy" >&2
            exit 1
        fi
    done
    FILLS=$(printf '%s\n' "$OUTPUT" | awk '/^ *MyStrategy/ { print $NF; exit }')
else
    # Without a backend there is no report and no vanilla to compare against, so the program states
    # what it replayed instead. That line is the only evidence the engine ran, so it is what is read.
    if printf '%s\n' "$OUTPUT" | grep -q 'SmaCrossover'; then
        printf 'no backend, yet the program printed an indicator strategy\n' >&2
        exit 1
    fi
    FILLS=$(printf '%s\n' "$OUTPUT" | awk '/MyStrategy replayed/ { print $(NF-1); exit }')
fi

# MyStrategy must actually *trade*, which is asserted on its fill count and not on the presence of its
# row.
#
# The first version of this check matched `MyStrategy` followed by a digit, and it was worthless:
# mutating the strategy to return before it reads anything still printed a row reading `0.00 %` with
# `0` fills, and the test passed. A scaffold whose example does nothing is worse than no scaffold,
# because it looks like it works -- so the guard was falsified by that mutation before being trusted,
# which is the standard `ai/wall-clock-test-assertions` settled on.
#
# The fill count is the last field of the row. Zero fills means the example emitted no intent, or
# emitted intents that never became orders, and either one makes the starting point a lie.
if [ -z "$FILLS" ]; then
    printf 'generated program reported no MyStrategy fill count\n' >&2
    exit 1
fi
case "$FILLS" in
    ''|*[!0-9]*) printf 'MyStrategy fill count was not a number: %s\n' "$FILLS" >&2; exit 1 ;;
esac
if [ "$FILLS" -lt 1 ]; then
    printf 'MyStrategy filled %s orders: the scaffold ships a strategy that does not trade\n' \
        "$FILLS" >&2
    exit 1
fi

printf 'scaffold smoke test passed\n'
