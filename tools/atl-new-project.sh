#!/bin/sh
#
# Generate a buildable algotradelib project: your strategy, replayed beside the shipped ones.
#
# The starting `src/main.cpp` is a *copy of a file this repository compiles*
# (`examples/example-backtest-compare.cpp`) rather than a heredoc written out from here. A template
# carried inside a scaffolder is a second copy of an example that nothing builds, and it is wrong
# within two branches. This way the suite compiles and runs the same source a new project starts from.
#
# It does not touch your git index. Adding a submodule writes to somebody else's repository and makes
# the script one you cannot run twice; the command is printed instead.
#
# Usage:
#   tools/atl-new-project.sh [options] <project-directory>
#
# Options:
#   -m, --mode <submodule|package>  how the project finds the library (default: submodule)
#   -n, --name <name>               CMake project and executable name (default: the directory name)
#   -f, --force                     write into a directory that already exists
#   -h, --help                      this text

set -eu

MODE=submodule
NAME=
FORCE=0
TARGET=

usage() {
    sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    printf '%s: %s\n' "$(basename "$0")" "$1" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--mode)  [ $# -ge 2 ] || die "--mode needs a value"; MODE=$2; shift 2 ;;
        -n|--name)  [ $# -ge 2 ] || die "--name needs a value"; NAME=$2; shift 2 ;;
        -f|--force) FORCE=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        -*)         die "unknown option '$1' (try --help)" ;;
        *)
            [ -z "$TARGET" ] || die "more than one project directory given"
            TARGET=$1
            shift
            ;;
    esac
done

[ -n "$TARGET" ] || { usage >&2; exit 2; }

case "$MODE" in
    submodule|package) ;;
    *) die "--mode must be 'submodule' or 'package', not '$MODE'" ;;
esac

if [ -z "$NAME" ]; then
    NAME=$(basename "$TARGET")
fi

# A CMake target name is not a free-form string. Normalizing here beats a confusing CMake error in
# generated code the person did not write.
NAME=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9_' '_')
case "$NAME" in
    [0-9]*) NAME=p$NAME ;;
esac

if [ -e "$TARGET" ] && [ "$FORCE" -eq 0 ]; then
    die "'$TARGET' already exists (use --force to write into it)"
fi

# Resolve this script's own directory, so the example can be found whether the script was invoked
# by a relative path, an absolute one, or through a symlink on PATH.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEMPLATE=$SCRIPT_DIR/../examples/example-backtest-compare.cpp

[ -f "$TEMPLATE" ] || die "cannot find $TEMPLATE -- run this script from inside an algotradelib checkout"

mkdir -p "$TARGET/src"
cp "$TEMPLATE" "$TARGET/src/main.cpp"

if [ "$MODE" = submodule ]; then
    FIND_LIBRARY='# Where the library is. The default is the submodule path suggested by the README;
# point it somewhere else with -DALGOTRADELIB_DIR=... if you vendor it elsewhere.
set(ALGOTRADELIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/algotradelib"
    CACHE PATH "Path to an algotradelib checkout")

if(NOT EXISTS "${ALGOTRADELIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "No algotradelib checkout at ${ALGOTRADELIB_DIR}. Add it as a submodule:\n"
        "    git submodule add <repository-url> external/algotradelib\n"
        "    git submodule update --init --recursive\n"
        "or configure with -DALGOTRADELIB_DIR=/path/to/algotradelib.")
endif()

# A second argument is required because the source directory may sit outside this project tree.
add_subdirectory("${ALGOTRADELIB_DIR}" algotradelib-build)'
else
    FIND_LIBRARY='# Uses an installed algotradelib. If it was installed to a prefix CMake does not search by
# default, configure with -DCMAKE_PREFIX_PATH=/your/prefix.
find_package(algotradelib REQUIRED)'
fi

cat > "$TARGET/CMakeLists.txt" <<CMAKEEOF
cmake_minimum_required(VERSION 3.16)
project($NAME LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

$FIND_LIBRARY

add_executable($NAME src/main.cpp)
target_link_libraries($NAME PRIVATE algotradelib::algotradelib)
CMAKEEOF

cat > "$TARGET/.gitignore" <<'GITEOF'
build/
compile_commands.json
GITEOF

cat > "$TARGET/README.md" <<MDEOF
# $NAME

A strategy built on [algotradelib](https://github.com/), replayed beside the library's own.

## Build and run

\`\`\`sh
cmake -S . -B build
cmake --build build
./build/$NAME
\`\`\`

The generated program needs no credentials and reaches no network: its bars are built in code, so it
prints the same table on every machine.

## Where your code goes

\`src/main.cpp\` holds \`MyStrategy\`. That struct is yours; everything around it is the harness that
feeds it bars and prints the comparison. Two things to keep when you rewrite it:

- **Handlers stay templates.** \`template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&)\`
  is what lets one strategy run in a backtest, in a dry run and in a live session without being
  rewritten. \`ATL_ASSERT_STRATEGY\` checks it at compile time.
- **Emit a target position, not an order.** A strategy emitting orders has to remember whether it
  already entered, and after a restart it does not know. Stating the position you want leaves that to
  reconciliation, which can see the portfolio and the working orders.

## Your own data

This library does not read files, on purpose: bars come from a broker adapter or from your own code.
Replace \`closes()\` with your history, keeping prices in \`Decimal64\` so nothing rounds on the way in.
See the library's README for the two routes and what each one costs you.
MDEOF

cat <<DONEEOF
Created $TARGET

  $TARGET/CMakeLists.txt
  $TARGET/src/main.cpp      <- MyStrategy is yours; the rest is the harness
  $TARGET/README.md
  $TARGET/.gitignore

Next:
DONEEOF

if [ "$MODE" = submodule ]; then
    cat <<DONEEOF
  cd $TARGET
  git init .
  git submodule add <repository-url> external/algotradelib
  cmake -S . -B build && cmake --build build && ./build/$NAME
DONEEOF
else
    cat <<DONEEOF
  cd $TARGET
  cmake -S . -B build && cmake --build build && ./build/$NAME
DONEEOF
fi
