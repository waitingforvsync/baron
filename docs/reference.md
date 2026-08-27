# The Baron reference #

The tables. For the friendly walk-through, start with the [guide](guide.md); for zero-page allocation in
depth, see [Zero page allocation](zero-page-allocation.md).

## Contents ##
- [Command line reference](#command-line-reference)
- [Keyword reference](#keyword-reference)
- [Expression reference](#expression-reference)
- [Version history](#version-history)

## Command line reference ##

```
baron [-v] [--check] [-D <sym>=<expr>] [-p <path>] [--inf] [-o <image.ssd>] [--title <t>]
      [--opt <0-3>] [--cycle <0-99>] [-log<n> <file>] <source files>
```

| Switch | What it does |
|--------|--------------|
| `-v` | Print an assembly listing to stdout: every emitted byte against its source line, macro expansions and includes followed through, `PRINT` output interleaved. |
| `-D <sym>=<expr>` | Predefine a symbol before assembly, e.g. `-D DEBUG=TRUE -D version="1.0"` (no spaces inside the assignment; repeat the switch for more). The expression gets the full evaluator and may forward-reference symbols the source defines. Applies to every source file on the line. See [Predefined symbols](guide.md#predefined-symbols). |
| `--check` | Assemble and validate everything, write nothing - no binaries, no image, no logs. |
| `-p <path>` | Write every saved section as a raw binary into the directory *path* (which must already exist; `.` for the current one). Nothing is written without this or `-o`. |
| `--inf` | Write a `.inf` sidecar beside each raw binary, carrying its DFS name and load/exec addresses. Needs `-p`. |
| `-o <image.ssd>` | Gather every saved section onto a DFS single-sided disc image. Only `.ssd` for now (`.dsd`, `.adf` and `.uef` are spoken for, later). May be combined with `-p`. |
| `--title <t>` | The disc title, up to 12 characters. Needs `-o`. |
| `--opt <0-3>` | The `*OPT4` boot option: 0 none, 1 `*LOAD`, 2 `*RUN`, 3 `*EXEC !BOOT`. Needs `-o`. |
| `--cycle <0-99>` | The catalogue cycle number. Needs `-o`. |
| `-log<n> <file>` | Redirect `PRINT` channel *n* (0-9) to a file. Channel 0 otherwise goes to stdout; channels 1-9 are otherwise discarded. |
| `--help` | Print the switch summary. |
| `--version` | Print the version and author information. |

`-p` and `-o` are the two output gates, and neither is implied: a run given neither still assembles,
reports and prints (`-v`, `PRINT`, `-log<n>` are unaffected), but the assembled bytes go nowhere and a
warning says so. `--check` asks for exactly that on purpose, and stays quiet.

Anything that is not a switch is a source file, and each assembles **independently** - a fresh symbol
table per file, diagnostics reported per file as it finishes. Diagnostics go to stderr in the
`file:line:col: error: message` shape; success is silent.

The exit code is 0 exactly when every file assembled (warnings alone stay 0). Every file is assembled even
after a failure - one run reports everything - but nothing is *written* (no binaries, no image, no logs)
unless the whole batch succeeded.

Development builds also accept `baron --test [group]`, which runs the unit test suite.

## Keyword reference ##

Everything is case-insensitive. Statements are separated by newlines or `:`; comments run from `;` or `\`
to the end of the line.

### Labels, symbols and scopes ###

| Syntax | Meaning |
|--------|---------|
| `.name` | Bind a label at the current address. |
| `name = expr` | Bind a symbol to a value. Symbols are immutable: one name, one value, per scope. |
| `{` ... `}` | A scope. A label immediately before the brace names it; its symbols are reachable from outside as `name.symbol`. Anonymous scopes are truly private. |
| `.@` | Bind an anonymous local label here. Referenced by `@-` / `@+` (nearest behind / ahead, within the current scope). |

### Data ###

| Syntax | Meaning |
|--------|---------|
| `EQUB v [, v...]` | Emit bytes. Numbers emit one byte (-255..255 accepted); strings emit their characters; lists and ranges flatten. `EQUS` is an alias - the two are interchangeable. |
| `EQUW v [, v...]` | As `EQUB`, but each value is a 16-bit little-endian word. |
| `EQUD v [, v...]` | As `EQUB`, but 32-bit. |
| `SKIP n` | Reserve `n` bytes of zeroes. |
| `SKIPTO addr` | Pad with zeroes up to `addr` (the address only ever moves forwards). |
| `ALIGN n` | Pad with zeroes to the next multiple of `n`. |
| `INCBIN "file"` | Splice a binary file's bytes into the output. Path is relative to the including source file. |
| `BASIC` ... `ENDBASIC` | Emit an inline BBC BASIC program, tokenised exactly as BASIC 4 stores it (uppercase keywords only, `&8D` line-number encoding, `&0D &FF` terminator). Each line starts with a decimal line number 0-32767 and runs to the end of the physical line; between lines only blanks and comments are allowed. Lines emit in written order and a record past 255 bytes is refused. |

### Structure and control ###

| Syntax | Meaning |
|--------|---------|
| `IF e` / `ELIF e` / `ELSE` / `ENDIF` | Conditional assembly. A dead branch emits nothing and binds nothing. Nests freely. |
| `FOR v = seq` ... `NEXT` | Run the body once per element of a range or list, `v` bound to each element, each iteration in its own scope. An empty sequence runs zero times. |
| `INCLUDE "file"` | Splice another source file in textually: its code emits here, its symbols bind here. Paths resolve relative to the including file. |
| `MACRO name sig` ... `ENDMACRO` | Define a macro. The signature is parameter names, commas, and quoted literal tokens matched verbatim; one name may carry several overloads, matched by shape. An empty body forward-declares. Define before use. |
| `FUNCTION name(params) = expr` | A pure function usable in any expression - one-liner form, or a multi-line body of local assignments and `IF`s ending in a top-level `= expr` return. Overloads by parameter count. Define before use. |
| `PRINT [#n,] v [, v...]` | Write text during assembly (final pass only): values concatenated, one newline per statement, to channel `n` (default 0). |
| `ERROR [v [, v...]]` | Report the message as an assembly error and fail the build; assembly carries on to find more. |

### Sections and output ###

| Syntax | Meaning |
|--------|---------|
| `SECTION name [, attr = e ...]` ... `ENDSECTION` | A named, uniquely-named block of output. Nested sections inherit attributes. |
| `INCSECTION name` | Splice the named section's assembled bytes at the current address (a literal copy - no relocation). The source may be defined anywhere. |

Attributes Baron acts on: `org` (assembly address; default: inherited, else 0), `filename` (the request to
save; `filename = ""` cancels an inherited one), `load` (default `org`), `exec` (default `load`),
`cmos` (truthy enables the 65C02 instruction set for this section - see the note under Reserved names),
and `guard` (an address the section's emission must not reach; overrunning it is a non-halting error
reporting the overshoot). Other keys are carried through untouched.

A filename destined for a disc image follows DFS rules: an optional single-character directory prefix
(`"D.NAME"`; default `$`), then a name of up to 7 characters.

### Zero-page allocation ###

| Syntax | Meaning |
|--------|---------|
| `ZPRESERVE addrs` | Hand the allocator a pool of zero-page bytes: a comma-separated list of addresses and `..` ranges. Its presence switches the feature on. |
| `ZPAUTO1 names` | Declare one-byte auto-allocated variables. |
| `ZPAUTO2 names` | Declare two-byte variables (pointer pairs). |
| `ZPAUTO n, names` | Declare `n`-byte variables (1-256). |
| `UNREACHABLE` | Assert the preceding branch is always taken - the fall-through path is pruned. |
| `CANCALL targets` | After a computed/self-modified `JSR`: the routines it may reach. |
| `CANJUMP targets` | After a computed/indirect `JMP`: the labels it may land on. |
| `DISCARD vars` | Promise the named `ZPAUTO` variables' current values are dead - later reads see only later writes. For arrays rebuilt via `STA arr,X`, whose writes prove nothing. |
| `ZPENTRY` | Mark the routine it opens as an externally-called entry point (a reachability root). Any `ZPENTRY` replaces the default "each section starts a routine" presumption. |
| `ZPINTERRUPT` | Mark the routine it opens as an interrupt handler: a root whose variables are kept apart from the rest of the program. |

The full story - liveness, footprints, sections, every error - is in
[Zero page allocation](zero-page-allocation.md).

### Reserved names ###

`TRUE`, `FALSE` and `PI` are reserved outright - they cannot be labels or symbols. Instruction mnemonics,
directive keywords and (already-defined) macro names win over identifiers at the start of a statement, so
a label or symbol spelled like one will not parse; pick another name. All the NMOS 6502 mnemonics are
recognised, plus the 65C02 extras (`BRA`, `STZ`, `PHX`, ...) - usable inside a section carrying
`cmos = TRUE`; elsewhere the target is plain NMOS and a CMOS-only encoding is refused with its own
message.

## Expression reference ##

### Values ###

| Kind | Examples |
|------|----------|
| Number | `42`, `1.5`, `&FF` or `$FF` (hex), `%1010` (binary). All numbers are one type; booleans are just 1 and 0. |
| String | `"hello"` - write `""` for a literal quote. |
| List | `{1, 2, 3}` - nestable, mixed types welcome, newlines allowed inside the braces. Nested equal-length lists act as higher-rank arrays; `SHAPE`/`RANK` report the axes all elements agree on (ragged lists keep only their uniform leading axes). |
| Range | A compact list of consecutive integers - see [Range forms](#range-forms). |

### Range forms ###

Endpoints must be whole numbers; either or both may be any constant expression. `..` binds looser than
arithmetic and comparisons, so `0..n-1` needs no parentheses.

| Form | Meaning |
|------|---------|
| `a..b` | Inclusive at both ends. Direction is inferred: `5..1` is `{5, 4, 3, 2, 1}`. |
| `a..<b` | Excludes the end. Ascending only; `a..<b` with `a >= b` (e.g. `5..<5`) is the legal empty list `{}`. |
| `a..b..c` | Stepped: `b` is the *second element*, fixing the step at `b-a`; `c` is a limit, not a promise (`1..3..10` is `{1, 3, 5, 7, 9}`). Steps down too: `10..8..1` is `{10, 8, 6, 4, 2}`. |
| `a..b..<c` | As above, excluding `c`. The `<` goes on the final separator only. |
| `a..` / `..b` / `a..b..` / `..` | Unbounded: one or both ends left off. Not enumerable as data - used as [subscript](#subscripts) selectors, where the axis supplies the missing end (`..b` runs from the start, `a..` to the end). |

### Operators, tightest first ###

| Operators | Meaning |
|-----------|---------|
| `x[...]` | Subscript - see [Subscripts](#subscripts). |
| `^` | Power (right-associative). |
| `-x` `+x` `!x` | Unary minus / plus, and logical NOT: `!x` is 1 if `x` is 0, else 0. Binds as tightly as unary minus, so `!x = 1` compares `!x` against 1. |
| `*` `/` `DIV` `MOD` `<<` `>>` | Multiply, divide, integer divide, modulo, shifts. |
| `+` `-` | Add, subtract. `+` also concatenates strings. |
| `=` `==` `!=` `<>` `<` `>` `<=` `>=` | Comparisons, yielding 1 or 0. `=`/`==` and `!=`/`<>` are synonyms; strings compare too. |
| `AND` | Bitwise AND (32-bit). |
| `OR` `EOR` | Bitwise OR, exclusive-OR (32-bit). |
| `..` `..<` | Range construction. |
| `<x` `>x` `~x` | 6502-style low / high byte of the whole following expression: `<start+1` is `LO(start+1)`. `~x` is the same shape, giving `x` as an uppercase hex *string* - 2, 4 or 8 digits, the narrowest that holds it (`~10` is `"0A"`, `~&123` is `"0123"`, `~&123456` is `"00123456"`). No `&` prefix; the 32-bit pattern is what is formatted, so `~-1` is `"FFFFFFFF"`. |

### Subscripts ###

`x[sel, sel, ...]` - one selector per axis, outermost first; fewer selectors than axes leaves the
trailing axes whole (`M[1]` is a row; `M[1][2]` and `M[1, 2]` are the same element). Three selector
kinds:

| Selector | Behaviour |
|----------|-----------|
| Integer | Picks one element and *drops* the axis. Out of range is an error. |
| Range | Slices, *keeping* the axis. Forgiving: clipped to the axis, may come back empty. Unbounded ends take the axis bounds (`L[..]` is the whole list, `L[3..]` from index 3 on, `L[0..2..]` every other element); a descending range reverses (`L[4..0]`). |
| List | Gathers exactly the named indices, in order, keeping the axis (`L[{0, 4}]`; `L[{1, 1, 1}]` repeats). Out of range is an error. |

Ranges subscript via their elements (`(100..200)[10]` is 110). Strings are rank 1 and always yield a
string: `"hello"[1..3]` is `"ell"`, `"hello"[4..0]` is `"olleh"`.

### Broadcasting ###

Operators and element-wise functions map across lists and ranges, NumPy-fashion. The rules, in order:

1. A range becomes its list of elements.
2. A scalar pairs with everything in the other side, however deep: `{{1, 2}, {3, 4}} * 10` is
   `{{10, 20}, {30, 40}}`.
3. Two lists of the same depth pair off element-wise - lengths must match (`{1, 2} + {1, 2, 3}` is a
   shape error), except that a length-1 list repeats: `{1, 2, 3} + {10}` is `{11, 12, 13}`.
4. The shallower side is held whole and applied to each element of the deeper: a rank-1 list against a
   rank-2 one applies per row (`{{1, 2, 3}, {4, 5, 6}} * {1, 0, 0}` is `{{1, 0, 0}, {4, 0, 0}}`), and a
   column against a row is the outer product (`{{1}, {2}} + {10, 20}` is `{{11, 21}, {12, 22}}`).

The rules recurse, so nesting and ragged tails work: `{1, {2, 3}} * 2` is `{2, {4, 6}}`. User
`FUNCTION`s inherit all of it - `sqr(0..15)` is a 16-element list.

### Element-wise functions ###

All broadcast; trigonometry is in radians.

| Function | Meaning |
|----------|---------|
| `LO(n)`, `HI(n)` | The low byte (bits 0-7) / the high byte (bits 8-15). |
| `ABS(n)` | Absolute value. |
| `INT(n)`, `FLOOR(n)` | Round down (the two are synonyms). |
| `ROUND(n)`, `CEIL(n)` | Round toward zero / up. |
| `SQRT(n)` | Square root. |
| `SIN(n)`, `COS(n)`, `TAN(n)`, `ASIN(n)`, `ACOS(n)`, `ATAN(n)` | Trigonometry. |
| `LOG(n)`, `LN(n)`, `EXP(n)` | Log base 10, natural log, e^n. |
| `NOT(n)` | Bitwise complement (32-bit). |
| `RND(n)` | A random integer in 0..n-1. `RND(FULL(k, n))` makes k draws. Deterministically reseeded each pass, so it converges. |

### List functions ###

| Function | Meaning |
|----------|---------|
| `LEN(x)` | Length of the outermost axis (elements, characters, or range count). |
| `SHAPE(x)` | The axis lengths as a list, outermost first; the axes all elements agree on (`SHAPE({{1,2},{3,4,5}})` is `{2}`). `{}` for a scalar; a range counts as the rank-1 list it stands for (`SHAPE(0..9)` is `{10}`; unbounded errors); empty axes are real lengths (`SHAPE({})` is `{0}`, `SHAPE({{}})` is `{1, 0}`). |
| `RANK(x)` | The number of axes (0 for a scalar; 1 for any range, unbounded included). |
| `FULL(n, v)` | A list of `n` copies of `v`. |
| `FLATTEN(x)` | Every leaf, in order, as one flat list. |
| `CONCAT(a, b, ...)` | Join: each list contributes its elements, each scalar itself. |
| `ZIP(a, b, ...)` | Equal-length lists into a list of tuples: `ZIP({1,2},{3,4})` is `{{1,3},{2,4}}`. |
| `REVERSE(x)` | The outermost axis reversed (works on strings too). |
| `SORT(L [, key...])` | Sort ascending by numeric key; extra arguments subscript each element to find its key (`SORT(L, 0)` sorts on first items). |
| `SUM(x [, axis])`, `PRODUCT(x [, axis])` | With no axis, fold *every* leaf to one scalar (`SUM({{1,2,3},{4,5,6}})` is 21; `SUM({})` is 0). With an axis (0 = outermost), collapse just that one: axis 0 folds rows together (`{5, 7, 9}`), axis 1 folds within each row (`{6, 15}`). |
| `MIN(x [, axis])`, `MAX(x [, axis])` | Smallest / largest, same axis rules. `MIN({})` errors - no identity. |
| `DEFINED(name)` | 1 if the symbol resolves (yet), 0 if not - the one function happy to receive an undefined name. |

### Constants ###

| Constant | Meaning |
|----------|---------|
| `TRUE`, `FALSE` | 1 and 0. |
| `PI` | 3.14159... |
| `*`, `P%` | The current assembly address - of *this* statement, so `JMP *` is jump-to-self and `EQUB *,*,*` advances per byte. Read-only. |
| `@-`, `@+` | The nearest `.@` local label behind / ahead, within the current scope. |

Your own `FUNCTION` names join the table as they are defined, callable as `name(args)`.

## Version history ##

- **0.1.0** (August 2026) - first release: the full assembler described here.