# The Baron guide #

Baron is a 6502 cross-assembler for the BBC Micro. You feed it assembly source, and it hands you back
binaries, `.inf` sidecars or a ready-to-boot DFS disc image. It is the successor to BeebAsm, rebuilt from
the ground up: a proper multi-pass assembler with a rich expression language, lexical scoping, macros,
pure functions - and a register allocator for your zero page, which we are rather proud of.

This guide walks through the language a feature at a time, teaching by example. The dry lists of every
switch, keyword and operator live in the [reference](reference.md).

## Contents ##
- [Coming from BeebAsm](#coming-from-beebasm)
- [A first program](#a-first-program)
- [The shape of a line](#the-shape-of-a-line)
- [Labels and scopes](#labels-and-scopes)
- [Sections](#sections)
- [Includes](#other-includes)
- [Inline BASIC](#inline-basic)
- [Expressions](#expressions)
- [Lists and ranges](#lists-and-ranges)
- [Subscripts](#subscripts)
- [Broadcasting](#broadcasting)
- [Zero-page allocation](#zero-page-allocation)
- [IF and FOR](#if-and-for)
- [Macros](#macros)
- [Functions](#functions)
- [Logging](#logging)
- [Saving your work](#saving-your-work)

## Coming from BeebAsm ##

If you know BeebAsm, you already know most of Baron - the mnemonics, the `.label` syntax, `EQUB`, `&` hex,
`{ }` scopes and colon-separated statements all carry straight over. The differences are where Baron earns
its keep:

- **`ORG`, `SAVE`, `GUARD` and `CLEAR` are gone.** Their jobs are done by [sections](#sections): a named
  block carrying attributes (`org`, `load`, `exec`, `filename`). Giving a section a filename *is* the
  request to save it.
- **As many passes as it takes.** BeebAsm makes two passes; Baron reassembles until the program converges.
  Forward references work nearly everywhere - in a `FOR` count, an `INCLUDE` filename, a section attribute.
- **Errors accumulate.** One mistake no longer stops the show; you get the lot in one run, GCC-style
  (`file:line:col: error: ...`), so your editor can jump to them.
- **The expression language got richer.** Values include lists and ranges, and operators broadcast across
  them element-wise - a whole sine table in one `EQUB`. See [Expressions](#expressions).
- **Zero-page allocation.** Declare zero-page variables by name and let Baron pack them, with a liveness
  analysis proving the packing safe. See [Zero-page allocation](#zero-page-allocation).
- **User-defined `FUNCTION`s**, and **macro overloading** - several `MACRO` signatures on one name,
  selected by shape.
- **Local labels.** `.@` binds an anonymous label; `@-` and `@+` reference the nearest one either side.
- **`PRINT` has ten channels**, redirectable to files from the command line - build logs, symbol dumps,
  whatever you like.
- **One file, one assembly.** Each source file on the command line assembles in a fresh world; symbols
  never leak between them.

A few BeebAsm conveniences have no Baron equivalent yet: `PUTBASIC`/`PUTTEXT`/`PUTFILE`, `MAPCHAR` and
`COPYBLOCK`. `ASSERT` is spelled with the tools you have:
`IF weird : ERROR "oh frak" : ENDIF`.

## A first program ##

Let us print something. Save this as `hello.6502`:

```
oswrch = &FFEE                  ; the OS character-out routine

SECTION Main, org = &2000, filename = "HELLO"

.start
    LDX #0
.loop
    LDA message,X
    BEQ done
    JSR oswrch
    INX
    BNE loop
.done
    RTS

.message
    EQUS "Hello, Beeb lovers!", 13, 0

ENDSECTION
```

Then assemble it:

```
$ baron hello.6502
```

Success is silent, and a file `HELLO` appears in the current directory (naming the file in the section is
what asked for it).

More usefully, we can go straight to a disc image:

First create `boot.6502`:
```
SECTION Boot, filename = "!BOOT", exec = &FFFFFFFF
EQUS "*RUN HELLO", 13
ENDSECTION
```

Then assemble it:
```
$ baron -o hello.ssd --title HELLO --opt 3 boot.6502 hello.6502
```

That builds a DFS `.ssd` with the two files on it, titled `HELLO`, set to `*EXEC !BOOT`. Load it
into your favourite emulator, hit Shift-Break, and there we are.

While we are here, try `baron -v hello.6502` - the listing shows every byte against its source line:

```
.start
  2000  A2 00           LDX #0
.loop
  2002  BD 0E 20        LDA message,X
  2005  F0 06           BEQ done
  ...
```

## The shape of a line ##

The basics, quickly:

- Comments run from `;` or `\` to the end of the line.
- `:` separates statements, so `LDA #0 : STA &70` is two statements on one line.
- Everything is case-insensitive: `lda`, `LDA` and `Lda` are all the same instruction.
- Numbers are decimal (`42`, `1.5`), hex (`&FF` or `$FF`) or binary (`%1010`).
- Strings are double-quoted; write `""` inside one for a literal quote.
- `name = expr` binds a symbol. Symbols are immutable - one name, one value - which is what lets Baron
  re-run passes freely. (Loops that want a changing variable use `FOR`.)

## Labels and scopes ##

`.name` defines a label at the current address. Braces open a scope, and a label immediately before the
brace names it:

```
.reset
{
    .loop               ; this loop is private to reset...
    DEX
    BNE loop
    RTS
}

.wipe
{
    LDA #0
    .nonzero
    {
      .loop             ; ...so this one does not collide with it
      STA &3000,X
      BNE loop
    }
}
```

From outside, a named scope's symbols are reachable by their dotted path: `JSR wipe.nonzero`. This is
good for when you want to provide additional entry points to a subroutine, or access a scoped symbol.
Anonymous `{ }` scopes work too, and are genuinely private - there is no path to name them by.

For tiny hops there are local labels: `.@` binds an anonymous label, and `@-` / `@+` refer to the nearest
one behind / ahead:

```
    LDA &FE4F : BPL @+      ; skip forward to the nearest .@
    INC speed+1
.@
```

Locals never leak out of their scope, so use them freely.

## Sections ##

A section is a named block of output: where the code assembles (`org`), and how it is saved (`filename`,
`load`, `exec`). Attributes are `name = expr` pairs on the `SECTION` line:

```
SECTION Code, org = &1100, filename = "CODE", exec = start
    ; ... the program ...
ENDSECTION
```

- `org` sets the assembly address. Within a section the address only ever moves forwards (`SKIP`,
  `SKIPTO` and `ALIGN` pad with zeroes; there is no rewinding).
- `filename` asks for the section to be saved - no filename, no file. `load` defaults to `org`, and
  `exec` defaults to `load`.
- Sections **nest**, and a child inherits its parent's attributes - handy for one outer section holding
  the file attributes and inner ones organising the code. A nested `filename = ""` opts back out of an
  inherited filename.
- `cmos = TRUE` enables the 65C02 instruction set for the section (and, by inheritance, its children;
  `cmos = FALSE` opts a child back out): `PHX`, `STZ`, `BRA`, `LDA (zp)` and friends. Everywhere else is
  plain NMOS 6502, and a CMOS instruction there says so: `CMOS-only instruction (needs cmos=TRUE on the
  section)`.
- Section names are their own namespace (they never collide with your symbols) and must be unique.
- Two sections may sit at the *same* address - sideways banks, swap-in overlays - without complaint;
  each keeps its own program counter.

Code outside any section lands in a nameless default section starting at address 0. That is fine for
scratch and experiments, but it can never be saved - anything you want out of the assembler needs a named
section with a filename.

`INCSECTION <name>` splices another section's assembled bytes at the current address - the relocation
workflow. Assemble the code at its *runtime* address in its own section, then splice those bytes into the
loadable section behind a copy-down stub:

```
SECTION Loader, org = &1200, filename = "GAME", exec = entry
.payload
    INCSECTION Code
.entry
    ; ... copy `payload` down to &400 and jump to it ...
    LDX #HI(entry-payload+&FF)
    LDY #0
    .loop
    LDA payload,X : STA &400,X
    INY : BNE loop
    INC loop+2 : INC loop+5
    DEX : BNE loop
    JMP codeentry
ENDSECTION

SECTION Code, org = &400        ; assembled for &400, carried inside GAME
    ; ...
ENDSECTION
```

The copy is literal - the bytes still expect to run at the source section's addresses - and the source may
be defined before or after the splice, in the same file or not.

## Other includes ##

`INCLUDE "file.6502"` splices another source file in textually (paths resolve relative to the including
file), and `INCBIN "file.dat"` splices a binary file's bytes into the output.

## Inline BASIC ##

`BASIC` ... `ENDBASIC` drops a BBC BASIC program straight into the output, tokenised byte-for-byte as
BASIC 4 itself would store it - handy for a loader that sets the mode and runs the main binary:

```
SECTION loader, org=&1900, filename="Loader"
BASIC
10REM My loader
20MODE 7
30*RUN MAIN
ENDBASIC
ENDSECTION
```

Each line starts with a decimal line number (0 to 32767) and runs to the end of the physical line. The
whole line belongs to BASIC: `:` separates BASIC statements there and `;` is a print separator, not a
Baron comment. The tokeniser is the ROM's own algorithm, quirks included - keywords must be uppercase
(`print` is a variable name, exactly as it would be on the Beeb), abbreviations like `P.` expand in the
ROM's order, line numbers after `GOTO` and friends get the compact three-byte encoding, and `REM`,
`DATA`, strings and `*` commands pass through untouched. `ENDBASIC` finishes the program with its
`&0D &FF` terminator, so the emitted bytes, loaded at `PAGE`, are a complete program ready to `RUN`.

Blank lines and Baron comment lines may sit between BASIC lines; anything else inside the block is an
error. Lines are emitted exactly in the order written - Baron does not sort them, so keep the numbers
ascending if you want `GOTO` and `LIST` to behave - and a line whose tokenised record would pass 255
bytes is refused.

## Expressions ##

Anywhere Baron wants a value, you get the full expression language: the usual arithmetic (`+ - * / ^`),
integer `DIV` and `MOD`, shifts `<<` `>>`, bitwise `AND` `OR` `EOR` `NOT()`, comparisons, and a ton of
functions (`SIN`, `SQRT`, `LO`, `HI`, ...) - the whole list is in the
[reference](reference.md#expression-reference).

```
LDA #LO(screenbase)             ; low byte - or the 6502-style  LDA #<screenbase
LDA #HI(312*64-2)               ; expressions anywhere an operand goes
```

`*` (or its BBC spelling `P%`) reads the current assembly address:

```
.spin  JMP *                    ; jump-to-self
```

Where it gets interesting is *lists* and *ranges*, which get the next three sections to themselves -
they are the most powerful part of the language, and the part worth slowing down for.

## Lists and ranges ##

A **list** is values in braces - any values, nested however you like:

```
{1, 2, 3}
{"one", 2, {3, 4}}              ; mixed types and nesting are fine
```

Lists nest, so a list of equal-length lists behaves as a 2D table - rows of columns. `SHAPE` tells you
the axis lengths, outermost first, and `RANK` how many axes there are:

```
SHAPE({1, 2, 3})                ; {3}
SHAPE({{1, 2, 3}, {4, 5, 6}})   ; {2, 3} - two rows of three
RANK({{1, 2, 3}, {4, 5, 6}})    ; 2
SHAPE(5)                        ; {} - a scalar has no axes
SHAPE({})                       ; {0} - one axis, nothing on it
SHAPE(0..255)                   ; {256} - a range is a rank-1 list
SHAPE({{1, 2}, {3, 4, 5}})      ; {2} - ragged rows agree on one axis only
```

Ragged lists are legal; they just have less shape for the shapely operations to work with.

A **range** is a compact list of consecutive integers, written with `..`:

```
1..5            ; {1, 2, 3, 4, 5} - inclusive at both ends
5..1            ; {5, 4, 3, 2, 1} - descending is inferred from the endpoints
0..<5           ; {0, 1, 2, 3, 4} - ..< excludes the end
5..<5           ; {}              - an empty range is a legal empty list
```

Give a range its *second element* and you have set its step:

```
0..2..10        ; {0, 2, 4, 6, 8, 10}   - "0, then 2, up to 10"
1..3..10        ; {1, 3, 5, 7, 9}       - the end is a limit, not a promise
10..8..1        ; {10, 8, 6, 4, 2}      - stepping down
0..2..<10       ; {0, 2, 4, 6, 8}       - ..< goes on the final separator
```

Either end may be left off - `2..`, `..7`, `0..2..` or just `..` - giving an *unbounded* range. There is
nothing to enumerate, so these are not data; they exist to be [subscripts](#subscripts), where the axis
supplies the missing end.

One habit to note: `..` binds looser than arithmetic and comparisons, so `0..n-1` means `0..(n-1)` with
no parentheses needed.

Since `EQUB` flattens whatever it is given into bytes, lists and ranges make data tables one-liners:

```
EQUB 1..5                       ; 01 02 03 04 05
EQUB FULL(16, &FF)              ; sixteen &FFs
EQUB RND(FULL(8, 256))          ; eight random bytes
EQUW table + (0..<32) * 8       ; a table of 32 pointers, stride 8
```

(`EQUB` and `EQUS` are the same directive - strings emit their characters, either name takes anything -
and `EQUW` / `EQUD` emit 16- and 32-bit little-endian units.)

## Subscripts ##

`x[...]` picks elements out of a list, range or string. There are three kinds of selector, and they do
different things:

- an **integer** picks one element;
- a **range** slices - it is forgiving, clipped to the ends, and hands back whatever falls inside;
- a **list** gathers exactly the named elements, in the order you name them (out of range is an error).

```
L = {10, 20, 30, 40, 50}

L[1]            ; 20
L[1..3]         ; {20, 30, 40}
L[{0, 4}]       ; {10, 50}
L[{1, 1, 1}]    ; {20, 20, 20} - gathers may repeat
```

Unbounded ranges shine here - the axis fills in the missing end:

```
L[3..]          ; {40, 50}      - from index 3 to the end
L[..1]          ; {10, 20}      - from the start up to index 1
L[..]           ; the whole thing
L[0..2..]       ; {10, 30, 50}  - every other element
L[4..0]         ; {50, 40, 30, 20, 10} - a descending slice reverses
L[3..10]        ; {40, 50}      - slices clip; no error
```

For nested lists, separate a selector per axis with commas, outermost axis first. An integer *drops* its
axis; a range or list *keeps* it:

```
M = {{1, 2, 3}, {4, 5, 6}}

M[1]            ; {4, 5, 6}     - row 1
M[1, 2]         ; 6             - row 1, column 2 (M[1][2] means the same)
M[.., 0]        ; {1, 4}        - every row's column 0
M[.., 1..]      ; {{2, 3}, {5, 6}} - all rows, columns 1 onward
```

Fewer selectors than axes just leaves the trailing axes whole - which is why `M[1]` is a row.

Strings subscript like rank-1 lists of characters, and always give back a string:

```
"hello"[1]      ; "e"
"hello"[1..3]   ; "ell"
"hello"[4..0]   ; "olleh"
"hello"[{0,4}]  ; "ho"
```

## Broadcasting ##

Operators (and the element-wise functions) work on lists by mapping across them, NumPy-fashion. The
rules, in the order they apply:

1. A range becomes its list of elements.
2. A scalar pairs with *everything* in the other side, however deep.
3. When both sides are lists of the *same* depth, elements pair off - lengths must match, except that a
   length-1 list repeats itself.
4. When one side is *shallower*, it is held whole and applied to each element of the deeper side.

```
{1, 2, 3} * 2               ; {2, 4, 6}                 - rule 2
{1, 2, 3} + {10, 20, 30}    ; {11, 22, 33}              - rule 3
{1, 2, 3} + {10}            ; {11, 12, 13}              - rule 3, the 10 repeats
{1, 2} + {1, 2, 3}          ; error: shapes do not match
{{1, 2}, {3, 4}} * 10       ; {{10, 20}, {30, 40}}      - rule 2, all the way down
```

Rule 4 is where it gets properly useful. A rank-1 list against a rank-2 one applies to *each row*:

```
{{1, 2, 3}, {4, 5, 6}} * {1, 0, 0}   ; {{1, 0, 0}, {4, 0, 0}} - masks every row
```

and a column against a row gives you all the combinations - the outer product:

```
{{1}, {2}} + {10, 20}       ; {{11, 21}, {12, 22}}
```

The reductions collapse the results back down: with no axis they fold *everything* to one scalar; given
an axis (0 = outermost) they collapse just that one:

```
SUM({{1, 2, 3}, {4, 5, 6}})         ; 21        - the lot
SUM({{1, 2, 3}, {4, 5, 6}}, 0)      ; {5, 7, 9} - down the columns
SUM({{1, 2, 3}, {4, 5, 6}}, 1)      ; {6, 15}   - along the rows
SUM(M * v, 1)                       ; matrix x vector, in two functions
```

And now the party trick from earlier makes complete sense - a range broadcasts through the maths and
each result lands as a byte:

```
ALIGN &100
.sintable
    EQUB ABS(SIN((0..255) / 128 * PI)) * 255 + 0.5
```

A full 256-entry sine table, one line.

## Zero-page allocation ##

The headline feature. Hand Baron a pool of zero-page bytes and declare variables by name; it runs a
liveness analysis over your actual code and packs them like a compiler allocating registers - provably
safely, or it refuses:

```
ZPRESERVE &70..&8F              ; the pool

.multiply
{
    ZPAUTO1 a2, b2              ; one-byte variables
    ZPAUTO2 ptr                 ; a two-byte pair - a pointer
    STA a2
    ...
}
```

Variables whose lifetimes never overlap share bytes automatically; anything held live across a `JSR` is
kept clear of everything the callee touches. It is entirely opt-in - no `ZPRESERVE`, no allocator - and it
has a whole guide to itself: [Zero page allocation](zero-page-allocation.md).

## IF and FOR ##

`IF` / `ELIF` / `ELSE` / `ENDIF` assembles conditionally. The condition is any expression (`TRUE` and
`FALSE` are built in), and a dead branch emits nothing at all:

```
debugrasters = FALSE

IF debugrasters
    LDA #&00 + PAL_red : STA &FE21
ENDIF
```

`FOR ... NEXT` runs its body once per element of a range or list, with the loop variable bound to each
element in turn:

```
FOR n = 0..7
    LDA #n : STA &FE21          ; unrolled eight times
NEXT

FOR addr = {&FE44, &FE45, &FE4B}
    STA addr
NEXT
```

Each iteration gets its own scope, so labels inside the body do not collide from one iteration to the
next. And because these are statements, not preprocessing, they nest freely with everything else -
macros, includes, other loops.

## Macros ##

A macro is a named statement sequence, expanded wherever its name appears:

```
MACRO ST16 addr, val
    LDA #LO(val) : STA addr
    LDA #HI(val) : STA addr+1
ENDMACRO

ST16 &70, screenbase
```

Arguments are *values*, evaluated at the call site; the body expands in its own scope, so its labels are
private. Define a macro before its first use - a call earlier in the file will not recognise the name.

One name can carry several **overloads**, distinguished by shape. Punctuation in a signature is quoted and
matched verbatim at the call, which lets a macro mimic addressing-mode syntax:

```
MACRO ADD n            : CLC : ADC n  : ENDMACRO
MACRO ADD "#" n        : CLC : ADC #n : ENDMACRO

ADD &70                 ; picks the first
ADD #3                  ; the '#' picks the second
```

Recursion works, with an `IF` for the base case:

```
MACRO FILL n
    IF n > 0 : EQUB n : FILL n-1 : ENDIF
ENDMACRO
```

## Functions ##

Where a macro emits statements, a `FUNCTION` computes a value - usable in any expression. The one-liner
form:

```
FUNCTION sqr(x) = x*x

EQUB sqr(0..15)                 ; broadcasting works through functions too
```

Longer functions take local assignments and `IF`s, with a top-level `=` as the return:

```
FUNCTION ball_ypos(n)
    x = (n - numdots/2 + 0.5) / (numdots/2)
    y = (x - SIN(x*PI) * 0.1) * radius
= 128 + y
```

Functions are pure - they compute, they never emit - and lexically scoped: a body sees its parameters and
the globals of where it was *defined*, never the caller's locals. Overload by parameter count, and recurse
away (`IF` for the base case, as with macros). Definition before use, as with macros.

Functions are powerful enough to be able to implement something like the quicksort algorithm, taking a list
and returning it sorted! 

## Logging ##

`PRINT` writes text during assembly - values concatenated with no separator, one newline per statement:

```
PRINT "code ends at ", *, " (", * - start, " bytes)"
```

Strings print raw and numbers in decimal; spacing is yours to supply. A `PRINT` speaks exactly once per
assembly, on the final pass, with everything resolved.

There are ten channels, `#0` to `#9`, selected by a leading `#n,`:

```
PRINT #1, "symbol dump: start = ", start
```

Channel 0 is the default and goes to stdout (interleaved with the `-v` listing, if you asked for one).
Channels 1-9 go nowhere unless the command line redirects them: `-log1 symbols.txt` collects everything
printed to `#1` into that file. `-log0` captures channel 0 too, if you want stdout kept clear.

`ERROR` is `PRINT`'s grumpy sibling: it reports its message as a genuine assembly error and fails the
build (though assembly carries on, so you still get the full report):

```
IF * > &3000 : ERROR "code overran the screen by ", * - &3000, " bytes" : ENDIF
```

## Saving your work ##

Naming a file is the request to save: every section with a `filename` attribute is written, and nothing
else is. There are two destinations:

- **Loose files** (the default): each named section becomes a binary in the current directory. Add
  `--inf` to write a `.inf` sidecar beside each (`$.CODE 001100 001100 001F40` style), which most
  emulators and transfer tools read.
- **A disc image**: `-o game.ssd` gathers the same sections onto a DFS single-sided disc image instead.
  `--title` sets the disc title, `--opt` the `*OPT4` boot option (`3` execs a `!BOOT` you have supplied
  as just another named section), `--cycle` the catalogue cycle number.

A multi-file run (`baron main.6502 loader.6502`) assembles each file independently and pools all their
saved sections into the output. If *anything* failed to assemble, nothing at all is written - no
half-built discs.

That is the tour. The full switch, keyword and operator tables await in the [reference](reference.md).
