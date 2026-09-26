#!/usr/bin/env bash
# Round-trip acceptance test:
#   compile test/hello.luau -> decompile -> compile again -> run both and compare stdout
#
# Usage: test/roundtrip.sh [-O<n>] [file.luau]
# Environment overrides: LUAU_COMPILE, LUAU, LUAUDEC (paths to the binaries)

set -u

cd "$(dirname "$0")/.."

OPT=""
SRC="test/hello.luau"
for arg in "$@"; do
    case "$arg" in
        -O*) OPT="$arg" ;;
        *) SRC="$arg" ;;
    esac
done

find_bin() {
    # $1 = env override value, $2... = candidate paths (first existing wins)
    if [ -n "$1" ] && [ -x "$1" ]; then echo "$1"; return; fi
    shift
    for c in "$@"; do
        if [ -x "$c" ]; then echo "$c"; return; fi
    done
}

LUAU_COMPILE=$(find_bin "${LUAU_COMPILE:-}" \
    third_party/luau/build/Release/luau-compile.exe third_party/luau/build/Release/luau-compile \
    third_party/luau/build/luau-compile.exe third_party/luau/build/luau-compile \
    build/third_party/luau/Release/luau-compile.exe build/third_party/luau/luau-compile)
LUAU=$(find_bin "${LUAU:-}" \
    third_party/luau/build/Release/luau.exe third_party/luau/build/Release/luau \
    third_party/luau/build/luau.exe third_party/luau/build/luau \
    build/third_party/luau/Release/luau.exe build/third_party/luau/luau)
LUAUDEC=$(find_bin "${LUAUDEC:-}" build/Release/luaudec.exe build/luaudec.exe build/luaudec)

for v in LUAU_COMPILE LUAU LUAUDEC; do
    if [ -z "${!v}" ]; then
        echo "error: could not find $v binary (set the $v environment variable)" >&2
        exit 2
    fi
done

base="${SRC%.luau}"
BIN="$base.luauc"
DEC="$base.dec.luau"
DECBIN="$base.dec.luauc"

echo "== compiling $SRC ${OPT:-(default optimization)}"
"$LUAU_COMPILE" --binary $OPT "$SRC" > "$BIN" || { echo "compile failed"; exit 1; }

echo "== decompiling $BIN -> $DEC"
"$LUAUDEC" "$BIN" > "$DEC" || { echo "luaudec failed"; exit 1; }

echo "== decompiled output:"
cat "$DEC"
echo

echo "== recompiling $DEC"
if ! "$LUAU_COMPILE" --binary $OPT "$DEC" > "$DECBIN"; then
    echo "FAIL: decompiled output does not compile"
    "$LUAU_COMPILE" --text "$DEC" 2>&1 | head -20
    exit 1
fi

echo "== running both"
orig_out=$("$LUAU" $OPT "$SRC" 2>&1 | tr -d '\r')
orig_rc=$?
dec_out=$("$LUAU" $OPT "$DEC" 2>&1 | tr -d '\r')
dec_rc=$?

echo "--- original stdout:"
echo "$orig_out"
echo "--- decompiled stdout:"
echo "$dec_out"

if [ "$orig_out" == "$dec_out" ] && [ "$orig_rc" == "$dec_rc" ]; then
    echo "PASS: identical output"
    exit 0
else
    echo "FAIL: output differs"
    diff <(echo "$orig_out") <(echo "$dec_out")
    exit 1
fi
