#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BAKE_BIN="$ROOT/build/bake"
WS="$ROOT/tests/projects/ws"
ENVPKGS="$ROOT/tests/projects/envpkgs"
C_PROJ_ROOT="$ROOT/tests/projects/c"
CPP_PROJ_ROOT="$ROOT/tests/projects/cpp"
TMP="$ROOT/tests/tmp"
BACKUP_DIR="$TMP/backup"
CORE_TEST_DIR="$WS/tests/core_tests/test"
CORE_GEN_DIR="$CORE_TEST_DIR/generated"
CORE_JSON="$CORE_TEST_DIR/tests.json"
CORE_SUITE="$CORE_TEST_DIR/math_suite.c"
CORE_MAIN="$CORE_GEN_DIR/main.c"
CORE_RT_C="$CORE_GEN_DIR/bake_test_runtime.c"
CORE_RT_H="$CORE_GEN_DIR/bake_test_runtime.h"
LIST_TXT="$ROOT/tests/tmp/list.txt"

if [ ! -x "$BAKE_BIN" ]; then
    echo "bake binary not found: $BAKE_BIN" >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
cp "$CORE_JSON" "$BACKUP_DIR/core_tests.json"
cp "$CORE_SUITE" "$BACKUP_DIR/core_math_suite.c"
cp "$CORE_MAIN" "$BACKUP_DIR/core_main.c"
cp "$CORE_RT_C" "$BACKUP_DIR/core_runtime.c"
cp "$CORE_RT_H" "$BACKUP_DIR/core_runtime.h"
if [ -f "$LIST_TXT" ]; then
    cp "$LIST_TXT" "$BACKUP_DIR/list.txt"
fi

restore_backups() {
    cp "$BACKUP_DIR/core_tests.json" "$CORE_JSON"
    cp "$BACKUP_DIR/core_math_suite.c" "$CORE_SUITE"
    cp "$BACKUP_DIR/core_main.c" "$CORE_MAIN"
    cp "$BACKUP_DIR/core_runtime.c" "$CORE_RT_C"
    cp "$BACKUP_DIR/core_runtime.h" "$CORE_RT_H"
    if [ -f "$BACKUP_DIR/list.txt" ]; then
        cp "$BACKUP_DIR/list.txt" "$LIST_TXT"
    fi
    rm -f "$TMP/info.txt"
    rm -rf "$BACKUP_DIR"
}

trap restore_backups EXIT

export BAKE_HOME="$ROOT/tests/tmp/bake_home"
export BAKE_THREADS=2
mkdir -p "$BAKE_HOME"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        LIB_EXT=".lib"
        LIB_PREFIX=""
        EXE_EXT=".exe"
        OS_TAG="Windows"
        ;;
    Darwin)
        LIB_EXT=".a"
        LIB_PREFIX="lib"
        EXE_EXT=""
        OS_TAG="Darwin"
        ;;
    Linux)
        LIB_EXT=".a"
        LIB_PREFIX="lib"
        EXE_EXT=""
        OS_TAG="Linux"
        ;;
    *)
        LIB_EXT=".a"
        LIB_PREFIX="lib"
        EXE_EXT=""
        OS_TAG="$(uname -s)"
        ;;
esac

case "$(uname -m)" in
    arm64|aarch64)
        ARCH_TAG="arm64"
        ;;
    x86_64|amd64)
        ARCH_TAG="x64"
        ;;
    i386|i686|x86)
        ARCH_TAG="x86"
        ;;
    *)
        ARCH_TAG="$(uname -m)"
        ;;
esac

BUILD_DEBUG="$ARCH_TAG-$OS_TAG-debug"
BUILD_RELEASE="$ARCH_TAG-$OS_TAG-release"
BUILD_PROFILE="$ARCH_TAG-$OS_TAG-profile"
BUILD_SANITIZE="$ARCH_TAG-$OS_TAG-sanitize"
PLATFORM_TAG="$ARCH_TAG-$OS_TAG"

rm -rf \
    "$BAKE_HOME" \
    "$TMP/stale_proj" \
    "$TMP/c_amalgamate.o" \
    "$TMP/cpp_amalgamate.o" \
    "$TMP/flecs_amalgamate.o" \
    "$C_PROJ_ROOT"/*/.bake \
    "$CPP_PROJ_ROOT"/*/.bake \
    "$C_PROJ_ROOT/pkg_amalgamate/distr" \
    "$CPP_PROJ_ROOT/pkg_amalgamate/distr" \
    "$WS/apps/hello/.bake" \
    "$WS/apps/hello/deps" \
    "$WS/apps/use_env/.bake" \
    "$WS/libs/core/.bake" \
    "$WS/libs/cppmath/.bake" \
    "$WS/tests/core_tests/.bake" \
    "$ENVPKGS/libmath/.bake" \
    "$ROOT/flecs/distr/flecs.c" \
    "$ROOT/flecs/distr/flecs.h"
mkdir -p "$BAKE_HOME"

cd "$WS"

$BAKE_BIN list > "$ROOT/tests/tmp/list.txt"
grep -q "^Listing projects for platform:" "$ROOT/tests/tmp/list.txt"
grep -q "^ \\* $PLATFORM_TAG$" "$ROOT/tests/tmp/list.txt"
grep -q "^Packages & Applications:" "$ROOT/tests/tmp/list.txt"

$BAKE_BIN build ws.apps.hello
"$WS/apps/hello/.bake/$BUILD_DEBUG/hello$EXE_EXT"
[ -f "$WS/apps/hello/.bake/$BUILD_DEBUG/generated/rule.txt" ]
[ -f "$BAKE_HOME/meta/ws.apps.hello/project.json" ]
[ -f "$BAKE_HOME/meta/ws.apps.hello/source.txt" ]
[ -f "$BAKE_HOME/meta/ws.apps.hello/dependee.json" ]
grep -q "$WS/apps/hello" "$BAKE_HOME/meta/ws.apps.hello/source.txt"
[ -f "$BAKE_HOME/$PLATFORM_TAG/debug/bin/hello$EXE_EXT" ]

$BAKE_BIN test ws.tests.core
[ -f "$WS/tests/core_tests/test/generated/main.c" ]
[ -f "$WS/tests/core_tests/test/math_suite.c" ]
[ ! -d "$BAKE_HOME/meta/ws.tests.core" ]

grep -q "void math_add(void)" "$WS/tests/core_tests/test/math_suite.c"

cat > "$WS/tests/core_tests/test/tests.json" <<'JSON'
{
    "suites": [
        {
            "id": "math",
            "testcases": ["add", "add_again"]
        }
    ]
}
JSON

$BAKE_BIN build ws.tests.core

grep -q "void math_add(void)" "$WS/tests/core_tests/test/math_suite.c"
grep -q "void math_add_again(void)" "$WS/tests/core_tests/test/math_suite.c"

$BAKE_BIN --standalone build ws.apps.hello
ls "$WS/apps/hello/deps"/*_amalgamated.c >/dev/null

$BAKE_BIN info ws.apps.hello > "$TMP/info.txt"
grep -q "^id:[[:space:]]*ws.apps.hello" "$TMP/info.txt"
grep -q "^kind:[[:space:]]*application" "$TMP/info.txt"

$BAKE_BIN --cfg debug build ws.libs.core
$BAKE_BIN --cfg release build ws.libs.core
$BAKE_BIN --cfg profile build ws.libs.core
$BAKE_BIN --cfg sanitize build ws.libs.core
$BAKE_BIN --strict build ws.libs.core
[ -f "$WS/libs/core/.bake/$BUILD_DEBUG/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$WS/libs/core/.bake/$BUILD_RELEASE/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$WS/libs/core/.bake/$BUILD_PROFILE/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$WS/libs/core/.bake/$BUILD_SANITIZE/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$BAKE_HOME/meta/ws.libs.core/project.json" ]
[ -f "$BAKE_HOME/include/ws.libs.core/ws/core.h" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/debug/lib/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/release/lib/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/profile/lib/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/sanitize/lib/${LIB_PREFIX}ws_core${LIB_EXT}" ]
[ -f "$BAKE_HOME/meta/ws.libs.core/dependee.json" ]
grep -q '"defines"[[:space:]]*:[[:space:]]*\["WS_CORE_DEP"\]' "$BAKE_HOME/meta/ws.libs.core/dependee.json"

$BAKE_BIN build ws.config.shared
[ -f "$BAKE_HOME/meta/ws.config.shared/project.json" ]

$BAKE_BIN clean ws.apps.hello -r
[ ! -d "$WS/apps/hello/.bake" ]

$BAKE_BIN rebuild ws.apps.hello -r

cd "$C_PROJ_ROOT"
for pj in $(find . -name project.json | sort); do
    id=$(awk -F'"' '/"id"[[:space:]]*:/ { print $4; exit }' "$pj")
    if [ -n "$id" ]; then
        $BAKE_BIN build "$id"
    fi
done

cd "$CPP_PROJ_ROOT"
for pj in $(find . -name project.json | sort); do
    id=$(awk -F'"' '/"id"[[:space:]]*:/ { print $4; exit }' "$pj")
    if [ -n "$id" ]; then
        $BAKE_BIN build "$id"
    fi
done

[ -f "$C_PROJ_ROOT/pkg_amalgamate/distr/examples_c_pkg_amalgamate.h" ]
[ -f "$C_PROJ_ROOT/pkg_amalgamate/distr/examples_c_pkg_amalgamate.c" ]
[ -f "$CPP_PROJ_ROOT/pkg_amalgamate/distr/examples_cpp_pkg_amalgamate.h" ]
[ -f "$CPP_PROJ_ROOT/pkg_amalgamate/distr/examples_cpp_pkg_amalgamate.cpp" ]

"${CC:-cc}" \
    -I"$C_PROJ_ROOT/pkg_amalgamate/distr" \
    -c "$C_PROJ_ROOT/pkg_amalgamate/distr/examples_c_pkg_amalgamate.c" \
    -o "$TMP/c_amalgamate.o"

"${CXX:-c++}" -std=c++17 \
    -I"$CPP_PROJ_ROOT/pkg_amalgamate/distr" \
    -c "$CPP_PROJ_ROOT/pkg_amalgamate/distr/examples_cpp_pkg_amalgamate.cpp" \
    -o "$TMP/cpp_amalgamate.o"

cd "$ENVPKGS"
$BAKE_BIN build env.libs.math
[ -f "$ENVPKGS/libmath/.bake/$BUILD_DEBUG/${LIB_PREFIX}envmath${LIB_EXT}" ]
[ -f "$BAKE_HOME/meta/env.libs.math/project.json" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/debug/lib/${LIB_PREFIX}envmath${LIB_EXT}" ]

cd "$WS"
$BAKE_BIN build ws.apps.use_env
"$WS/apps/use_env/.bake/$BUILD_DEBUG/use_env$EXE_EXT"
[ -f "$BAKE_HOME/$PLATFORM_TAG/debug/bin/use_env$EXE_EXT" ]

cd "$ROOT"
$BAKE_BIN build flecs
[ -f "$ROOT/flecs/.bake/$BUILD_DEBUG/libflecs.a" ]
[ -f "$ROOT/flecs/distr/flecs.h" ]
[ -f "$ROOT/flecs/distr/flecs.c" ]
[ -f "$BAKE_HOME/meta/flecs/project.json" ]
[ -f "$BAKE_HOME/meta/flecs/dependee.json" ]
[ -f "$BAKE_HOME/include/flecs/flecs.h" ]
[ -d "$BAKE_HOME/template/flecs/c" ]
[ -d "$BAKE_HOME/template/flecs/cpp" ]
[ -f "$BAKE_HOME/$PLATFORM_TAG/debug/lib/libflecs.a" ]
grep -q '"lang.c"' "$BAKE_HOME/meta/flecs/dependee.json"
grep -q '"lang.cpp"' "$BAKE_HOME/meta/flecs/dependee.json"
grep -q '"\${cfg sanitize}"' "$BAKE_HOME/meta/flecs/dependee.json"

"${CC:-cc}" \
    -I"$ROOT/flecs/distr" \
    -c "$ROOT/flecs/distr/flecs.c" \
    -o "$TMP/flecs_amalgamate.o"

if [ ! -f "$ROOT/flecs/.bake/$BUILD_DEBUG/libflecs.a" ]; then
    echo "flecs artefact not found" >&2
    exit 1
fi

$BAKE_BIN list > "$ROOT/tests/tmp/list.txt"
grep -q "^Listing projects for platform:" "$ROOT/tests/tmp/list.txt"
grep -q "^ \\* $PLATFORM_TAG$" "$ROOT/tests/tmp/list.txt"
grep -q "^Packages & Applications:" "$ROOT/tests/tmp/list.txt"
grep -q "^Templates:" "$ROOT/tests/tmp/list.txt"
grep -q "^P  flecs => \\[debug\\]$" "$ROOT/tests/tmp/list.txt"
grep -q "^P  ws.libs.core => \\[debug, profile, release, sanitize\\]$" "$ROOT/tests/tmp/list.txt"
grep -q "^A  ws.apps.hello => \\[debug\\]$" "$ROOT/tests/tmp/list.txt"
grep -q "^C  ws.config.shared => \\[all\\]$" "$ROOT/tests/tmp/list.txt"
grep -q "^T  flecs => \\[c, cpp\\]$" "$ROOT/tests/tmp/list.txt"
grep -Eq "^applications: [0-9]+, packages: [0-9]+, templates: [0-9]+$" "$ROOT/tests/tmp/list.txt"

mkdir -p "$TMP/stale_proj/src"
cat > "$TMP/stale_proj/project.json" <<'JSON'
{
    "id": "tmp.stale",
    "type": "application",
    "value": {
        "output": "tmp_stale"
    }
}
JSON
cat > "$TMP/stale_proj/src/main.c" <<'SRC'
int main(void) { return 0; }
SRC

cd "$TMP/stale_proj"
$BAKE_BIN build tmp.stale >/dev/null
cd "$ROOT"
rm -rf "$TMP/stale_proj"

$BAKE_BIN cleanup
[ ! -d "$BAKE_HOME/meta/tmp.stale" ]
[ ! -f "$BAKE_HOME/$PLATFORM_TAG/debug/bin/tmp_stale$EXE_EXT" ]

$BAKE_BIN reset
[ ! -d "$BAKE_HOME/meta" ]
[ ! -d "$BAKE_HOME/include" ]
[ ! -d "$BAKE_HOME/template" ]
[ ! -d "$BAKE_HOME/$PLATFORM_TAG" ]

echo "integration tests passed"
