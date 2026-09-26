# luaudec

A decompiler for Luau bytecode, written in C++17. It supports bytecode versions 3 to 14, the current default is 14, and
produces Luau source that compiles and behaves like the original.

Opcode numbers, instruction encodings and constant tags come straight from
`third_party/luau/Common/include/Luau/Bytecode.h` and `BytecodeUtils.h`, so the
decompiler follows the upstream definitions instead of a hand-written copy.

## Layout

```
src/
  bytecode/   parse a .luauc container into Module / Proto / Constant structures
  disasm/     decode instructions (ABC, AD, E, AUX) and print luau-compile style text
  cfg/        basic blocks and control flow edges per function
  lift/       register/liveness analysis, expression folding, control flow structuring
  emit/       Luau source printer with indentation, precedence and string escaping
  main.cpp    command line driver
test/
  hello.luau      the acceptance script
  hello.dis.txt   reference disassembly produced by luau-compile --text
  roundtrip.sh    compile -> decompile -> recompile -> run both and compare stdout
  samples/        larger scripts used while developing the lifter
third_party/luau  upstream Luau checkout (unmodified)
```

## Building

Luau is a git submodule (`third_party/luau`, pinned to the commit the decompiler was
tested against), so clone with submodules:

```bash
git clone --recurse-submodules <this repository>
```

or, in an existing checkout:

```bash
git submodule update --init --depth 1
```

The upstream compiler and interpreter are needed for the round trip test. They are
built from `third_party/luau` with CMake:

```bash
cmake -S third_party/luau -B third_party/luau/build -DLUAU_BUILD_TESTS=OFF
cmake --build third_party/luau/build --config Release --target Luau.Compile.CLI Luau.Repl.CLI
```

Then build the decompiler:

```bash
cmake -S . -B build
cmake --build build --config Release
```

`build/Release/luaudec.exe` (or `build/luaudec` with single-configuration generators)
is the result. The top level CMake file also adds the Luau tree as a subdirectory
(excluded from the default build), so `cmake --build build --target luau-compile luau`
builds the upstream tools inside the same build tree if you prefer that over the
separate build directory above. Pass `-DLUAUDEC_WITH_LUAU_TOOLS=OFF` to skip it.

## Running

```bash
luau-compile --binary script.luau > script.luauc
luaudec script.luauc              # decompile (default)
luaudec --disasm script.luauc     # text disassembly, matches `luau-compile --text` instruction for instruction
luaudec --cfg script.luauc        # basic blocks and edges per function
luaudec --info script.luauc       # container summary
luaudec -v script.luauc           # decompile with diagnostics as comments
```

When the bytecode carries local names (`-g2`) they are used. Otherwise names are
inferred where the bytecode allows it and generated as `v1`, `v2`, ... elsewhere:

- local functions keep the name the compiler recorded for them (`local function
  _cycleBuffer(...)`), which survives at the default `-g1` level;
- numeric loop variables become `i`, `j`, `k` by nesting depth, generic loops use
  `k, v` (or `i, v` over `ipairs`);
- a function stored in a field whose name is called with `obj:name(...)` anywhere in
  the module is printed as `function t:name(...)` with a `self` parameter;
- generated names never shadow a global the module reads or writes.

Single-use temporaries are folded into the expression that consumes them, table
constructors are rebuilt in source order (including closures stored in them), and
`a, b = b, a`, `x = if c then a else b`, `x = c and a or b` and value chains such as
`(p or q) and not r` (where the falsy operand is the result) are recognized as such.

Optimized bytecode (`-O2`, or a `--!optimize 2` directive) inlines small local
functions. An inlined `return` inside a loop becomes a jump past the loop exit, which
the decompiler renders with a flag: `escaped = true; break` inside the loop and
`if not escaped then ... end` around the code the return skipped. Variables the
compiler captured by value are never assigned again, so a register reused after such a
capture starts a new variable instead of clobbering the captured one.

## Testing

```bash
test/roundtrip.sh          # default optimization level
test/roundtrip.sh -O2
test/roundtrip.sh -O0 test/samples/full.luau
```

The script compiles the source, decompiles it, prints the decompiled code, recompiles
it to prove it is valid Luau, then runs the original and the decompiled file with
`luau` and asserts identical stdout. It looks for the binaries in the build
directories above; `LUAU_COMPILE`, `LUAU` and `LUAUDEC` override the paths.

`test/hello.luau` round trips at every optimization level and prints
`Hello World	32` both times. At `-O2` luau-compile does not inline `a()` because `a`
is a global function, so the output is the same as at `-O1`. On scripts with local
functions (`test/samples/full.luau`) `-O2` inlines small functions, folds their
results into constants and drops unused locals; the decompiled output reflects the
optimized bytecode (for example `print(add(1, 2))` becomes `print(3)`) but still
compiles and prints the same values.

To check the disassembler against the reference:

```bash
luau-compile --text test/hello.luau | grep -vE '^ +[0-9]+:|^REMARK' | diff - <(luaudec --disasm test/hello.luauc)
```

The reference contains source lines and compiler remarks that are not in the
bytecode; everything else (function headers, debug locals, type annotations, labels,
instructions) matches byte for byte.

## Supported constructs

Straight line code: LOADNIL, LOADB, LOADN, LOADK, LOADKX, MOVE, GETGLOBAL,
SETGLOBAL, GETUPVAL, SETUPVAL, GETIMPORT, GETTABLE, SETTABLE, GETTABLEKS,
SETTABLEKS, GETTABLEN, SETTABLEN, NEWCLOSURE, DUPCLOSURE, CAPTURE, NAMECALL, CALL
(including MULTRET argument and result propagation), RETURN, ADD, SUB, MUL, DIV,
IDIV, MOD, POW and their K variants, SUBRK, DIVRK, AND, OR, ANDK, ORK, CONCAT, NOT,
MINUS, LENGTH, NEWTABLE, DUPTABLE, SETLIST, GETVARARGS, CLOSEUPVALS, PREPVARARGS.
The FASTCALL family (FASTCALL, FASTCALL1, FASTCALL2, FASTCALL2K, FASTCALL3,
FASTPCALL) is skipped and the fallback CALL that follows is decompiled instead.
GETUDATAKS, SETUDATAKS, NAMECALLUDATA and CALLFB are treated like their plain
counterparts.

Control flow: `if`/`elseif`/`else` with `and`/`or` chains (JUMPIF, JUMPIFNOT,
JUMPIFEQ, JUMPIFLE, JUMPIFLT and their NOT forms, JUMPXEQKNIL, JUMPXEQKB,
JUMPXEQKN, JUMPXEQKS), `while`, `repeat ... until`, `while true`, numeric `for`
(FORNPREP/FORNLOOP), generic `for` (FORGPREP, FORGPREP_NEXT, FORGPREP_INEXT,
FORGLOOP), `break` and `continue` (JUMP, JUMPBACK, JUMPX), and the value forms the
compiler emits for `a and b`, `a or b`, `c and x or y`, comparisons materialized
with LOADB skips, and boolean expressions with a preloaded literal.

## Not yet supported

- NEWCLASS, NEWCLASSMEMBER and class shape constants (experimental class bytecode).
- CMPPROTO (feedback based call target checks, bytecode version 11 and up with
  `LuauEmitCallFeedback`).
- NATIVECALL, COVERAGE, BREAK and NOP are ignored.
- LOADB with a skip offset outside the boolean patterns above.
- Integer constants (LBC_CONSTANT_INTEGER) are printed as plain integer literals.
- Jumps that do not fit the recognized if/loop shapes (for example bytecode produced by
  other tools) are reported as `-- luaudec: ...` comments rather than reconstructed.
- Multiple assignment other than a two-way swap is printed as separate statements,
  and `and`/`or` chains that mix value and statement forms may come out as
  equivalent `if` statements.

If a function cannot be lifted at all, its disassembly is emitted as comments so the
rest of the module still decompiles.
