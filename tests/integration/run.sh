#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BAKE_BIN="$ROOT/build/bake"
WS="$ROOT/tests/projects/ws"

if [ ! -x "$BAKE_BIN" ]; then
    echo "bake binary not found: $BAKE_BIN" >&2
    exit 1
fi

export BAKE_HOME="$ROOT/tests/tmp/bake_home"
export BAKE_THREADS=2
mkdir -p "$BAKE_HOME"

rm -rf "$WS/apps/hello/build" "$WS/libs/core/build" "$WS/libs/cppmath/build" "$WS/tests/core_tests/build" "$WS/apps/hello/deps"

cd "$WS"

$BAKE_BIN list > "$ROOT/tests/tmp/list.txt"
grep -q "ws.apps.hello" "$ROOT/tests/tmp/list.txt"

$BAKE_BIN build ws.apps.hello
"$WS/apps/hello/build/debug/bin/hello"
[ -f "$WS/apps/hello/build/debug/generated/rule.txt" ]

$BAKE_BIN test ws.tests.core
[ -f "$WS/tests/core_tests/test/generated/main.c" ]
[ -f "$WS/tests/core_tests/test/math_suite.c" ]

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

$BAKE_BIN clean ws.apps.hello -r
[ ! -d "$WS/apps/hello/build" ]

$BAKE_BIN rebuild ws.apps.hello -r

cd "$ROOT"
$BAKE_BIN build flecs
if [ -f "$ROOT/flecs/build/debug/lib/libflecs.a" ]; then
    :
elif [ -f "$ROOT/flecs/bake-build/debug/lib/libflecs.a" ]; then
    :
else
    echo "flecs artefact not found" >&2
    exit 1
fi

echo "integration tests passed"
