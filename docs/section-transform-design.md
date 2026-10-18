# Section transforms - design discussion

A running record of the design discussion that started from
[section-transform-spec.md](section-transform-spec.md). Topics are kept up to date as the
discussion moves on; settled points are listed under [Decisions](#decisions), unsettled ones under
[Open questions](#open-questions), and the [Discussion log](#discussion-log) at the end records how
we got here.

Started 2026-10-17.


## The goal

Now that sections own their bytes, let a section's bytes be transformed (compressed, encrypted,
reordered) inside one Baron run, so the transformed bytes land in the parent section and every
symbol stays available. No second invocation, no exporting addresses through `PRINT`, no external
tool. The headline case is compression, with Baron producing the best stream it can for a format
the user describes.


## Decisions

1. **Describe the compression, not the decompression.** The pitch's decoder pseudocode was a way to
   explain the format; the transform itself is written from the compressor's side.
2. **The transform runs as a final step**, once everything else has resolved (after zero-page
   allocation), and triggers another round of passes when labels depend on the transformed size.
   See [section 2](#2-when-the-transform-runs).
3. **Cycles are reported as a failure to settle**, like any other program that never converges.
4. **Target formats:** the pitch's format, and something like Exomizer, which fits its bit lengths
   to each file (see [3.2](#32-the-two-formats-to-support)).
5. **Settling again after zero-page allocation is acceptable.** (Now part of the rounds in section
   2.)
6. **Direction: unify around `FUNCTION` if at all possible** (the user's preference, and the
   recommendation in [section 6](#6-recommendation)). The `SPEC` block (Option A) is kept on record
   only as a fallback.
7. **No bit fields as strings of `0` and `1`.** Baron favours plain lists over typed arrays:
   simplicity over speed, with no differences between arrays and lists to expose.
8. **Not numbers-as-bits with strings-as-bytes either** (round 5): if a second representation is
   needed for directly-read bytes, strings for bits would be no worse. Superseded by decision 10.
9. **First implementation step: lightweight call frames** (section 6, step 1), started in round 5.
10. **Bit fields are numbers with a marker bit, packed by `BITPACK`** (the user's idea, round 6;
    see [5.5](#55-representing-bits)). **`BITPACK` stays purely about bits, for now** (round 7): bytes
    a decoder reads straight from the stream (ZX0's layout, [5.6](#56-bytes-the-decoder-reads-directly-zx0))
    are deferred.
11. **The FUNCTION recursion limit rises from 64 to 128** (round 8 chose 256 on an estimate of 2.5 KB
    of C stack a level; round 9 measured 3.5-4.8 KB, which would put 256 at or past Windows' 1 MB).
    Loops, not recursion, carry the long walks.
12. **No function values, in any spelling tried so far** (round 8). A FUNCTION is always met as its
    call token `name(`, and a bare name, a `dbl(?)` placeholder or a string each rub against that.
    What the transforms need is loops, not functions as values: see
    [5.11](#511-loops-without-function-values), which proposes a comprehension instead.
13. **The whole feature is built** (round 9): comprehensions, calls that free their working memory,
    `transform=` with rounds and a cache, `BITLEN`, `BITPACK` and `LZ_PARSE`, and `examples/compress`.
    The example uses the pitch's format; beebcrunch's v1 format, which reads literals straight from the
    stream, served as the test of the parse instead (see the round 9 log).


## 1. Reading the example format

The pitch's pseudocode describes an LZ77-style format. Restated as a stream of fields:

```
header       16 bits    LEN, the uncompressed length
             3 bits     OB, the number of low offset bits (0-7)

body         literal run, match run, literal run, match run, ... until LEN bytes are out

literal run  gamma(n)               n >= 1
             n bytes, verbatim

match run    gamma(k)               k >= 1 matches follow
             k matches

match        gamma(len - 1)                  len >= 2
             gamma(((off - 1) >> OB) + 1)    off >= 1
             (off - 1) AND (2^OB - 1), in OB bits
```

A match copies `len` bytes one at a time from `off` bytes back, so an offset shorter than the length
repeats a pattern (offset 1 is run-length encoding). The `FOR m=0..REP` loop is inclusive, so a match
copies `REP + 1` bytes and the shortest match is 2 bytes.

What works well about it:

- Elias gamma cannot encode zero, and the format never needs to. Consecutive literals merge into one
  run and consecutive matches into one run, so the strict literal/match alternation never forces a
  wasted token. Starting with a literal run costs nothing, as the first byte can never be a match.
- The `WHILE` test only runs between runs, so the last run must end exactly at `LEN`. A compressor
  produces that naturally.
- `OB` per stream is a nice touch: the compressor tries all eight values and keeps the shortest.

Details a precise description has to pin down:

- **Bit order** within a byte (most significant bit first?).
- **How bits and bytes share the stream.** Either a pure bitstream, where a literal byte is just 8
  more bits, or the 6502-friendly layout used by ZX0 and Exomizer: a bit buffer refilled one byte at
  a time from the stream, with literal bytes read straight from the stream (`LDA (src),Y`) rather than
  shifted through the carry. This changes the byte order of the output, so the compressor must know.
- **The gamma variant.** Classic (n zeros, then the n+1 bits of the value) or interlaced (each data
  bit preceded by a continue bit, as ZX0 does).
- **The header layout.** Whether `LEN` is 16 bits through the bit buffer or two bytes.
- Whether `OB` travels in the header at all: a decoder assembled with the chosen `OB` as a constant
  saves the 3 bits and some run time (see [8.1](#81-getting-results-back-into-the-source)).


## 2. When the transform runs

### 2.1 A final step

The transform runs in the **output pass**: the re-emission Baron already does after zero-page
allocation, whose sections are the result. By then every symbol has settled and every ZA_AUTO
variable holds its real address, so the transform sees exactly the bytes that would otherwise have
been saved.

The hook is `sections_close` (at `ENDSECTION`), which already copies a nested section's bytes into
its parent and moves the parent's pc past them. In the output pass, a transformed section transforms
its bytes first, then copies the result and advances by its length. A top-level section saves its
transformed bytes. A virtual section keeps no bytes, so `transform=` on one is an error.

Today the output pass only runs when ZA_AUTO or `-v` needs it. A program with a transformed section
always runs it.

### 2.2 Rounds

The transformed size is not known until the output pass, but the parent's layout depends on it: in
the pitch's example, `.entry` sits after the compressed bytes. So assembly becomes a loop of
**rounds**, each one a full assembly as Baron does it today:

1. **Settling passes.** A transformed section takes a *reserved* size in its parent: the size its
   transform produced last round, or 0 in the first round. The reserved bytes are zeroes; the
   transform does not run. Inside the section, assembly is normal.
2. **The final pass** records diagnostics and the zero-page analysis, as today.
3. **Zero-page allocation**, as today.
4. **The output pass** runs each transform for real and copies the results into the parents.
5. **The check.** If every transformed size matches its reservation, nothing moved and the program
   is done. Otherwise the new sizes become the reservations and another round begins. Labels that
   depend on a transformed size settle in that round's settling passes.

The symbol table carries over from round to round, so the next round starts from where the last one
finished. A typical program needs two rounds: the first finds the size, the second confirms it.
`-v` listings and `PRINT` output come from the last round's output pass only, since both are rebuilt
every pass.

### 2.3 Why this fixes ZA_AUTO

The difficulty was that settling passes emit `LDA var` as `A5 00` and the output pass emits `A5 70`,
so a transform run during settling would measure the wrong bytes. With rounds, the transform only
ever sees output-pass bytes. Each round runs allocation again on the new layout. The allocation
depends on the code's structure rather than its absolute addresses, so it will normally come out the
same. If it doesn't, the transformed bytes change, the sizes differ, and another round runs.

### 2.4 Cycles

If a transformed section refers to something whose address depends on its own transformed size
(code inside `Code` calling `my_decompress`, placed after the compressed bytes), the sizes may never
agree. A cap on rounds catches it, reported as a failure to settle. The message should name the
section whose size kept changing, with the last two sizes, in the way the existing diagnostic names
a symbol that kept changing and its last two values.

### 2.5 Caching

The second round usually hands the transform exactly the same bytes as the first. Baron keeps each
transformed section's last input and output in the permanent arena and reuses the output when the
input is identical byte for byte. A full compare, not a CRC: a collision would ship stale data.

### 2.6 Diagnostics from a round that guessed

A round whose reservations turn out wrong ran its final pass on the wrong layout, so its diagnostics
may be wrong too. Reserving 0 in the first round means the layout is never more spread out than the
real one, so guard, pc overflow and branch range checks cannot fail falsely. A check that demands a
*minimum* distance (an `ASSERT` on a gap, say) could. Two ways to handle it:

- **Simple:** accept it. Errors in any round stop the assembly, and the rare false failure is
  documented.
- **Exact:** errors only count in a round whose sizes matched their reservations. A round with
  errors and unconfirmed sizes measures the transforms on the bytes it has, skips allocation, and
  goes round again. An error that is still there once the sizes are confirmed is reported.

### 2.7 Nested transforms

A compressed section inside an encrypted one composes without any list syntax. In the output pass
the inner `ENDSECTION` transforms first, so the outer transform sees the inner's real output.

### 2.8 INCBIN is not a problem

The first draft worried about `INCBIN` *inside* a transformed section (compressing a screen image
pulled in with `INCBIN`, say), because settling passes reserve the file's size without reading it.
That only mattered while the transform ran on every pass. The output pass loads the real file, so
with the transform deferred there is nothing to fix.

### 2.9 Labels inside a transformed section

Labels inside keep their meaning: the addresses where the untransformed bytes will live once the 6502
has undone the transform. For a compressed section that is somewhere else, so it wants its own `org`
(as `Code` has, at `&400`). Whether to require one is open: a transform that keeps the size, such as
XOR, undone in place, could sensibly continue the parent's addresses.


## 3. Describing the compression

### 3.1 Compression is a search

A decoder is a function from compressed bits to bytes. Compressing means finding the shortest input
the decoder turns into our data. For LZ-family formats that search is well understood: the "optimal
parse" that modern packers such as ZX0, Exomizer and LZSA use. Baron walks the data from start to
end, keeping the cheapest way found so far to have produced everything up to each point, and the
candidate moves at each point are "copy some bytes verbatim" and "copy `len` bytes from `off` bytes
back", supplied by a match finder.

What the search needs from the user is a **cost model** (how many bits each move costs, which
follows from how its fields are encoded) and the **structure** of the format (what decides which kind
of move comes next). Section 7 sketches the search itself. Whichever way the user describes the
format, the search has to be native code: it is far too much work for an interpreter.

### 3.2 The two formats to support

**The pitch's format** (section 1): gamma-coded counts and lengths, the offset split into a gamma
high part and `OB` raw low bits, `OB` chosen per stream by trying all eight.

**Something like Exomizer.** As far as I know its format, Exomizer codes each length and offset as a
*bucket number* plus some raw bits, where the number of raw bits depends on the bucket. Those bit
counts are not fixed: the compressor fits them to each file's statistics and stores them in the
header. It alternates between an optimal parse using the current bit counts and refitting the bit
counts to that parse, until the result stops improving.

So the second format adds an outer loop around the search: parse, refit the encoding tables, parse
again. The parse is heavy work; the loop around it is a handful of steps.


## 4. Option A: a format description (`SPEC`) - fallback only

The first draft proposed a block that describes the format declaratively, in the decoder's read
order, with each field written as the value to encode:

```
SPEC lz
    CHOOSE ob = 0..7                        ; Baron tries each value and keeps the shortest

    BITS 16, SIZE                           ; SIZE: the uncompressed length, built in
    BITS 3, ob

    ALTERNATE                               ; a literal run, then a match run, then ...
        LITERALS n                          ; n >= 1 bytes copied verbatim
            GAMMA n
            BYTES                           ; the n bytes themselves
        MATCHES k                           ; k >= 1 back-references in a row
            GAMMA k
            EACH MATCH off, len             ; copy len bytes from off bytes back
                GAMMA len - 1
                GAMMA ((off - 1) >> ob) + 1
                BITS ob, (off - 1) AND ((1 << ob) - 1)
    ENDALTERNATE                            ; stops when SIZE bytes are out
ENDSPEC
```

Baron would work out the cost model by evaluating the field expressions, the structure from the
block's shape, and lay out the bits itself. Its strength is readability: the spec reads as the
format. Its costs: a new single-purpose sub-language and its compiler, a second mechanism for
deterministic transforms (XOR, deltas), and a dedicated mechanism for Exomizer's table fitting.
Section 5.8 shows the FUNCTION route can match its readability, so it is no longer recommended.


## 5. Option B: FUNCTIONs

`transform=` names a `FUNCTION` that takes the section's bytes as a list and returns the transformed
bytes. One mechanism for every kind of transform.

### 5.1 Is FUNCTION the right tool at all?

In principle, exactly: a transform is a pure function from bytes to bytes, and `FUNCTION` is Baron's
pure-computation language. What stands in the way is how FUNCTIONs are implemented today and what
primitives they have, not what they are.

### 5.2 What FUNCTIONs can do today

Deterministic transforms already work. Both of these were checked:

```
key = {&5A, &C3, &17}
FUNCTION crypt(data) = data EOR key[(0..<LEN(data)) MOD LEN(key)]
FUNCTION delta(data) = (data - concat(0, data[0..<LEN(data)-1])) AND &FF
```

Anything that walks the data does not, for reasons measured with a scratch build whose recursion cap
was lifted:

- **Depth.** The cap is 64. With it lifted, the C stack overflows (segfault) between 3000 and 4000
  levels: about 2.5 KB of C stack per level with an 8 MB stack. Release builds for Windows get a 1 MB
  stack by default, so roughly 400 levels there.
- **Memory per call.** Every call makes a frame in the *permanent* symbol table and copies its
  arguments into it, deep-copying lists. That is how a body's locals bind once, but it means nothing
  a call allocates is ever freed.
- **Lists passed down recursion grow quadratically.** Each level keeps a copy of its argument, so
  head-and-tail recursion over n elements stores n + (n-1) + ... copies. Measured: summing a list by
  recursion took 17 MB for 500 elements, 53 MB for 1000, 188 MB for 2000. Building a list by
  recursion behaved the same. Splitting in halves instead gives n log n: a gamma encoder mapped over
  1000 values took 34 MB, 4000 took 164 MB.
- **Running out is a panic, by design.** richc treats an arena running past its reservation as
  unrecoverable. At 16000 values the permanent arena ran past the 256 MB the CLI reserves
  (`rc_arena_make_default`), at the reservation check in `rc_arena_realloc_impl`; total memory in use
  was about 350 MB, nowhere near 4 GB. Reservations are only address space, so the CLI could reserve
  far more at no cost on a 64-bit build, but the real fix is not copying (5.4).
- **Exponential only with sharing.** Building `{x, x}` repeatedly doubles memory per level, because
  the copy turns shared structure into duplicates. Ordinary list handling is quadratic, not
  exponential.
- **Lists are fat.** A list element is a 40-byte value, so a list of bits costs 40 bytes per bit.
- **Slices copy.** `l[1..]` builds a new list, so even without the frame copies, head-and-tail
  recursion would copy quadratically.

### 5.3 Why tail recursion is not the answer

Tail recursion would lift the depth limit for loops written as recursion, but:

- Baron bodies return through an assignment (`r = f(...)` inside an `IF`, then `= r`), because there
  is no conditional expression. A recursive call is almost never syntactically in tail position, so
  it would have to be recognised through that pattern.
- It does nothing about the frame copies, so a loop of n steps carrying a list still costs n copies.

Raising the cap is a cheap improvement (64 is tight even for quicksort on sorted input), but it is
bounded by the C stack, and the Windows build is the tightest.

### 5.4 What would make FUNCTION the right tool

Three changes to the language and its runtime, all useful beyond transforms:

1. **Lightweight call frames.** Parameters and locals live in a small frame in the evaluation's own
   arena, chained to the definition scope for globals, instead of a child scope in the permanent
   symbol table. Arguments are passed as views, not copied. This turns the quadratic growth linear,
   removes the arena panic, makes calls cheaper, and keeps call frames out of the symbol dump. Contiguous
   slices (`l[1..]`) can then be views too, since values are immutable.
2. **Native iteration.** A loop Baron runs itself, so there is no depth to run out of and the results
   accumulate in linear time. Round 3 proposed function values and builtins that take them (`MAP`,
   `SCAN`, `ITERATE`); round 8 replaced that with a comprehension, `{expr FOR x = seq}`, because
   function values rub against how Baron spells functions. See 5.11.
3. **Bit fields as numbers with a marker bit**, packed natively by `BITPACK` (round 7; round 4 said
   plain lists of bits and `PACK`). See 5.5.

### 5.5 Representing bits

Three ways were considered for the bits a compressor writes.

**A native bit-field type** (an `rc_bitset` value). The impact would be moderate, not massive. It
needs no parser change, since builtins construct it and no literal syntax is required. The
type-specific code is concentrated: the switches in `value.c` (copy, equality, `PRINT` format,
JSON) and roughly twenty string-aware sites in `expression.c` and `assemble.c`, which a type
behaving like a string would mirror. A few hundred lines. But it is niche (other bit packing in BBC
work is byte-sized arithmetic on numbers), and it is the kind of typed array Baron avoids by design.

**A bit writer threaded through user code** (the user's suggestion: a list of numbers, and a
function that adds bits by appending a byte or modifying the last one). The writing logic is
exactly right, but threading it through FUNCTIONs has problems:

- The list is not enough state: the writer must also know how many bits of the current byte are
  used, and for ZX0 which byte that is (below). So the user threads a small record through every call.
- Every write depends on the previous state, so nothing can broadcast: it becomes thousands of
  sequential calls.
- Values are immutable, so each write produces a new list. That is quadratic, unless the builtin
  appends in place. Appending in place is safe (older copies just see a shorter list), but modifying
  a byte in place is not: any older copy still in use would silently change.
- A cost table would mean writing every candidate value into a fresh state to measure it.

**Plain lists, with the writer inside `PACK`** (proposed in round 4; its bits/bytes convention was
turned down in round 5, so this is open again). User code *describes* the stream in
the decoder's read order, and one native `PACK` call runs the user's writer over the description:

- A bit field is a list of bits: `GAMMA(5)` is `{0, 0, 1, 0, 1}`, `BITS(5, 4)` is `{0, 1, 0, 1}`.
  Both work element-wise, so `GAMMA({1, 5})` is `{{1}, {0, 0, 1, 0, 1}}`.
- A byte read straight from the stream (not through the bit buffer) is a string, Baron's existing
  byte sequence: `CHR(bytes)`.
- Sequence fields with braces or `CONCAT`; join per element with `ZIP` (`ZIP(lens, offs)` gives each
  match its length field then its offset field). Nesting is free, because `PACK` flattens the whole
  description and reads it in order: numbers are bits, strings are bytes.
- `PACK` refuses a number other than 0 or 1, which catches bytes passed without `CHR`.

The cost is 40 bytes per bit while the description exists. At BBC sizes that is affordable once
calls stop copying: a cost table over 20000 offsets is about 20 MB, and it is temporary. It does
mean `MAP` should release each call's temporaries once the result is taken.

If memory ever does bite, a native type could later sit behind the same builtins.

**A number with a marker bit** (the user's idea, round 6; **chosen**, decision 10). A field of N
bits holding v is the single number `2^N + v` (equivalently `(1 << N) OR v`): the most significant
set bit marks the width and is not part of the value. `BITPACK(list)` concatenates such fields into
a list of bytes.

- One number per field, not per bit, and no new type.
- The width is `BITLEN(x) - 1`, so a cost table is plain arithmetic over a whole list:
  `BITLEN(fields) - 1`.
- Encodings become ordinary broadcasting FUNCTIONs, so `GAMMA` and `BITS` need not be builtins:
  `FUNCTION bits(v, n) = 2^n + v` and `FUNCTION gamma(n) = 2^(2 * BITLEN(n) - 1) + n` (gamma's
  leading zeros are just a wider field).
- Two fields join with arithmetic too, element-wise: `(a - 1) * 2^(BITLEN(b) - 1) + b` puts b's
  bits after a's. So each match can be one number (its length field then its offset field).
- Limit: Baron's bitwise operators (`<<`, `>>`, `AND`, `OR`, `EOR`) work on 32-bit patterns, so a
  field built with them holds at most 31 bits (`1 << 32` is 0). Written with `^` and `+`, as above,
  a field reaches 52 bits, since numbers hold integers exactly up to 2^53. (Round 6 said 52 without
  this caveat.) A stream is a *list* of fields, so only single fields and joins are bounded, which
  is ample.
- Needs an exact, element-wise `BITLEN` (bit length; `BITLEN(0)` is 0), since `LN(x)/LN(2)` is
  unreliable at powers of two.
- Impossible values come for free. Anything below 1 is not a field: `gamma(0)` is `2^-1`, a half.
  If `BITLEN` of anything below 1 is 0, its width comes out as -1, which a cost table can carry to
  `LZ_PARSE` as "impossible".
- **`BITPACK` is bits only, for now** (round 7). It refuses anything that is not a field. The sign
  convention suggested in round 6 for bytes read straight from the stream (5.6) is dropped with it.

### 5.6 Bytes the decoder reads directly (ZX0)

**Deferred** (round 7): `BITPACK` packs bits only. Until this comes back, a format that would read
bytes directly writes them as 8-bit fields instead, and its decoder reads them through the bit
buffer. That is slower (eight shifts per byte instead of one `LDA`) but needs nothing new, and the
pitch's format is a pure bitstream anyway. When it does come back, it can be a separate layout step
that takes the fields plus the direct bytes, leaving the representation alone. The rest of this
section records what that step would have to do (its sketches still use round 4's lists of bits).

ZX0's decoder reads literal bytes, and each new offset's low byte, straight from the stream; it reads
bits from a buffer byte, which it refills from the stream when it runs out. A refill takes the byte
wherever the stream has got to *at the moment the next bit is needed*. Literal bytes read after that
come after the buffer byte in the stream, while the buffer byte still has bits to give.

So the writer must keep filling a byte that is no longer the last one. That is the one change to
"append, or modify the last byte": it is "append, or modify the current bit byte", where the
current bit byte is wherever the last refill put it. `PACK` walks the description in order:

- a string's bytes are appended at the end;
- a bit goes into the current bit byte; when that byte is full, a new zero byte is appended at the end
  and becomes the current bit byte.

That is how ZX0's own compressor writes its output. Two details:

- **ZX0's low-byte trick.** Bit 0 of the offset's low byte carries the first bit of the length's
  code. That is arithmetic in user code, schematically:

  ```
  g = IGAMMA(len - 1)                        ; the length's code, a list of bits
  {..., CHR((lo << 1) OR g[0]), g[1..]}      ; lo: the offset bits that go in this byte
  ```

- **Refill timing.** The sentinel-bit idiom that ZX0 and Exomizer decoders use refills lazily, when a
  bit is needed and none is left. A decoder that refills as soon as the last bit is taken would need
  a `PACK` option.

### 5.7 The toolkit

Names are placeholders. Rewritten in round 7 for marker-bit fields (5.5); round 4's version had
`GAMMA`, `BITS` and `COST` as builtins over lists of bits, and a `PACK` that also placed bytes.

| Builtin | Does |
|---|---|
| `BITLEN(x)` | The exact bit length of each element (`BITLEN(0)` is 0, as is anything below 1). A field's width is `BITLEN(f) - 1`, so a whole cost table is one expression. |
| `BITPACK(fields)` | Flattens its argument and concatenates the fields, in order, into a list of bytes, padding the last byte with zero bits. Refuses anything that is not a field (below 1, or not an integer). Bits only (5.6). |
| `LZ_PARSE(data, ...)` | The optimal parse, given cost tables (5.9). Returns the predicted size in bits and the runs. A negative cost means impossible. |
| `FIT_BUCKETS`, `BUCKET(v, widths)` | Exomizer-style tables: fit bucket bit counts to a set of values; encode a value with a table (a field, element-wise). |

The encodings are a few lines of ordinary FUNCTIONs, which could ship as a library file:

```
FUNCTION bits(v, n) = 2^n + v                           ; v in n bits
FUNCTION gamma(n)   = 2^(2 * BITLEN(n) - 1) + n         ; n >= 1: BITLEN(n) - 1 zeros, then n
FUNCTION join(a, b) = (a - 1) * 2^(BITLEN(b) - 1) + b   ; a's bits, then b's
FUNCTION cost(f)    = BITLEN(f) - 1                     ; width in bits; -1 if f is not a field
```

They use `^` and `+` rather than `<<` and `OR`, which stop at 32 bits (5.5). `bits` does not check
that `v` fits in `n` bits; a checking version needs an element-wise conditional (open question 4).
`CONCAT`, `FLATTEN`, `ZIP`, slicing and broadcasting already exist.

Because `cost` measures the encoder's own output, **the encoder is the cost model**: the format is
stated once, and impossible values follow from the encodings (a match of length 1 is impossible
because `gamma(0)` is a half, not a field). That recovers the two strongest properties of Option A.

### 5.8 The pitch's format as FUNCTIONs

The whole compressor for the pitch's format, as it would read with the pieces proposed here: the
marker-bit fields (5.5), the comprehension (5.11) and `LZ_PARSE` (section 7). Rewritten in round 8;
the round 4 version used function values.

```
; The encodings (5.7): four one-liners that could live in a library file.
FUNCTION bits(v, n) = 2^n + v                           ; v in n bits
FUNCTION gamma(n)   = 2^(2 * BITLEN(n) - 1) + n         ; Elias gamma, n >= 1
FUNCTION join(a, b) = (a - 1) * 2^(BITLEN(b) - 1) + b   ; a's bits, then b's
FUNCTION cost(f)    = BITLEN(f) - 1                     ; a field's width; -1 if impossible

; The format's two match fields, each written once.
FUNCTION len_field(len) = gamma(len - 1)                ; REP: a match copies REP + 1 bytes

FUNCTION off_field(off, ob)
    hi = gamma(((off - 1) >> ob) + 1)                   ; HI
    lo = bits((off - 1) AND (2^ob - 1), ob)             ; LO, in OB bits
= join(hi, lo)

; One run, in the order the decoder reads it.
FUNCTION run_fields(run, ob)
    IF run[0] == 0                                      ; a literal run: count, then the bytes
        r = {gamma(LEN(run[1])), bits(run[1], 8)}
    ELSE                                                ; a match run: count, then each match
        offs = run[1][0]
        lens = run[1][1]
        r = {gamma(LEN(offs)), ZIP(len_field(lens), off_field(offs, ob))}
    ENDIF
= r

; One search: hand LZ_PARSE the cost of every value each field could hold.
FUNCTION parse_with(ob, data)
    n    = LEN(data)
    runs = cost(gamma(0..n))                            ; a run header, by count
    lits = cost(bits(0..255, 8))                        ; a literal, by byte value
    lens = cost(len_field(0..n))                        ; a match length, by length
    offs = cost(off_field(0..n, ob))                    ; a match offset, by offset
= LZ_PARSE(data, runs, lits, runs, lens, offs)

; The transform: try every OB, keep the smallest, encode the winner.
FUNCTION lz(data)
    parses = {parse_with(ob, data) FOR ob = 0..7}       ; eight searches
    sizes  = parses[.., 0]                              ; each one's size in bits
    ob     = FIND(sizes, MIN(sizes))                    ; the smallest wins
    runs   = parses[ob][1]
= BITPACK({bits(LEN(data), 16), bits(ob, 3), {run_fields(r, ob) FOR r = runs}})

    SECTION Code, org=&400, transform="lz"
```

How the pieces fit:

1. **Cost tables, by broadcasting.** `parse_with` runs the format's own field encodings over every
   value a field could hold, all at once: `cost(gamma(0..n))` is the width of the gamma code for
   every count from 0 to n, with no loop. Impossible values fall out on their own. A run of 0 is
   impossible because `gamma(0)` is a half, not a field, so its cost is -1. A match of length 1 is
   impossible for the same reason, because its field is `gamma(0)`. (Offset 0 never occurs; its
   table entry is never read.)
2. **The search.** `LZ_PARSE` finds the cheapest sequence of runs by those tables (section 7). It
   knows the shape of the format (runs alternate, literals first, each run's count before its
   contents; 5.9) but nothing about how any field is written. It returns `{bits, runs}`: the body's
   size, and the runs. A literal run is `{0, bytes}`; a match run is `{1, {offsets, lengths}}`, two
   lists, so the field FUNCTIONs encode a whole run's matches in one broadcast call.
3. **Every OB.** The comprehension runs the search once per `OB`, and the smallest wins. Only the
   offset table differs between the eight.
4. **Encoding.** The second comprehension walks the winning runs and writes each one's fields in
   the decoder's order; `BITPACK` flattens the lot and packs it into bytes.

The search and the encoder cannot disagree, because both come from the same field FUNCTIONs. Each
field is stated once, in decoder order. An offset is one joined field, at most 31 bits for 16-bit
offsets. A match's length and offset stay two elements (`ZIP`) rather than one join, because a long
match far back could pass the 52-bit limit; `BITPACK` flattens the pairs, so the bits are the same.
The pitch's format is a pure bitstream, so its literal bytes are `bits(run[1], 8)`, which is all
`BITPACK` offers for now.

### 5.9 What `LZ_PARSE` has to assume

Costs are passed as data. The **structure** cannot be, so the builtin has to assume one. A structure
general enough for both target formats:

> The stream alternates between literal runs and match runs. Each run has a header whose cost
> depends on its count; each literal costs 8 bits; each match costs a length cost plus an offset cost,
> where the offset table may differ by length (Exomizer uses different offset tables for lengths 1, 2
> and 3 or more).

- **The pitch's format:** both header tables are gamma costs of the count.
- **Flag-per-move formats** (Exomizer, LZSS): the header tables are linear (one flag bit per item),
  and a long literal run's escape code is just a cheaper entry further along the literal table.

What it does not cover: formats that remember state between moves, such as "repeat the last offset"
(ZX0, LZSA), and formats whose one token byte holds two lengths (LZ4). Those need the parse to grow,
as formats demand. That is the same C work under Option A, whose engine would need the same new
states, so it is not a point in Option A's favour.

### 5.10 Exomizer-style fitting as FUNCTIONs

The outer loop is where FUNCTIONs shine: it is short, visible, and the user controls it. It is a
handful of steps, so plain recursion does it (round 8; round 3 used an `ITERATE` builtin, which took a
function value):

```
FUNCTION exo_settle(tables, data, tries)               ; refit until the tables stop changing
    next = exo_fit(exo_parse(tables, data))            ; parse with these tables, fit new ones
    IF tries == 0 OR SUM(next <> tables) == 0
        r = next
    ELSE
        r = exo_settle(next, data, tries - 1)
    ENDIF
= r

FUNCTION exo(data)
    tables = exo_settle(exo_start, data, 8)
= exo_encode(exo_parse(tables, data), tables)
```

`exo_parse` is `LZ_PARSE` with costs from `cost(BUCKET(..., tables))`; `exo_fit` is `FIT_BUCKETS` over
the parse's lengths and offsets. `SUM(next <> tables) == 0` asks whether nothing changed; a whole-value
equality builtin would read better.

### 5.11 Loops without function values

Round 8. The user wanted functions as things that can be passed around, but not at the price of how
Baron spells them, and asked why they are needed at all.

**Why loops.** The compressor walks sequences far longer than recursion allows: every run of the
winning parse (hundreds, or thousands), every `OB`. Recursion takes one level per element, and runs
into the depth limit long before the C stack gives out at 3000-4000 levels. Until calls free their
own memory (below), every level also keeps its temporaries until the whole expression is done.

**Why function values came in, and why they rub.** `MAP(f, list)` and `SCAN(f, init, list)` are the
usual functional answer: Baron runs the loop, so nothing nests. But they take a function as an
argument. In Baron a FUNCTION is only ever met as its call token, `name(`, like every builtin, and
each spelling for "the function itself" fights that:

- **A bare name**, `MAP(dbl, xs)`: a second way for a function name to lex. A symbol of the same name
  (legal today: `dbl = 3` beside `FUNCTION dbl`) would shadow it. And calling a function held in a
  parameter, `f(x)`, would make any identifier followed by `(` a call.
- **A placeholder**, `MAP(dbl(?), xs)`: keeps the `(` and gives partial application for free
  (`scale(?, 3)`), but `?` is a new marker meaning "not yet", and calling a held function would still
  need a `CALL(f, x)`.
- **A string**, `MAP("dbl", xs)`: no new syntax, but functions become text, and a misspelt name
  only shows when the call runs.

But the need is loops, not function values.

**A comprehension.** `FOR` as an expression, inside list braces:

```
{x * x FOR x = 0..7}                     ; {0, 1, 4, 9, 16, 25, 36, 49}
{run_fields(r, ob) FOR r = runs}         ; encode every run
{parse_with(ob, data) FOR ob = 0..7}     ; one search per OB
{{x * y FOR x = 1..3} FOR y = 1..3}      ; nested: a 3x3 table
```

- `seq` is a list or a bounded range, evaluated once, as for the `FOR` statement.
- The name is bound to each element in turn and is visible only inside the expression. Like a
  FUNCTION local, it hides any outer name and is gone afterwards.
- The expression can be anything, not just a call. The results come back in order; an empty
  sequence gives `{}`.
- Each element's working memory is released once its result is copied out, so the memory needed is
  for the results, not for every element's temporaries.
- The first error among the elements becomes the comprehension's value (like `FIND`'s miss).
- In a dead branch of a FUNCTION body, nothing runs.
- It is spelt like the statement, so there is nothing new to learn. And it cannot change what an
  existing program means: a list element followed by a name is a syntax error today.

The expression comes before the `FOR`, so Baron reads it once just to find where it ends (running no
calls and drawing no `RND`), then evaluates it per element. A filter, `{x FOR x = xs IF x > 3}`, would
be a natural later addition.

**Broadcasting already does part of this.** A FUNCTION whose body is arithmetic maps over a list
with no loop at all: given `FUNCTION dbl(x) = x * 2`, `dbl(0..7)` is `{0, 2, ..., 14}` today. The cost
tables in 5.8 are built this way. A vectorised `WHERE(mask, a, b)` (deferred earlier) would extend
broadcasting to per-element choices. What broadcasting cannot do is per-element structure:
`run_fields` builds differently shaped output for literal and match runs, so encoding runs needs
the comprehension.

**Running state.** `SCAN` (state carried from element to element, for LFSR keystreams or checksums)
has no comprehension form. Neither target format needs it: `BITPACK` builds the stream natively, each
run encodes on its own, the eight searches are independent, and Exomizer's refitting is a handful of
steps (5.10). If a transform does need it: recursion for short chains, a native builtin for a specific
recurrence, or a comprehension with an accumulator, say `{s + x FOR x = xs FROM s = 0}` (each result
becomes `s` for the next element). To design when a need turns up.

**Independent of the syntax.** Two parts of step 2 stand on their own:

- **Each FUNCTION call frees its working memory when it returns.** The body runs in a fresh copy of a
  second, spare arena and hands back a copy of its result. The two arenas take turns as calls nest,
  so each level's temporaries die with it. This helps plain recursion as much as loops.
- **The recursion limit rises from 64 to 256** (decision 11).


## 6. Recommendation

Unify around FUNCTION. Option A's advantages were readability and stating the format once; with
`cost` measuring the encoder, the FUNCTION version has both. Meanwhile
FUNCTIONs give one mechanism for every transform, make the outer loops (the `OB` search, Exomizer's
refitting) ordinary code, and every improvement along the way helps the rest of Baron too.

The work, in an order where each step stands on its own:

1. **Lightweight call frames** and view slices. Removes the memory blow-up behind today's arena
   panic and makes existing FUNCTIONs cheaper. **Done** (round 6): see the log for what it changed
   and what it leaves for step 2.
2. **Native loops and cheaper calls:** the comprehension `{expr FOR x = seq}` (5.11), each call freeing
   its working memory when it returns, and a recursion limit of 128. **Done** (round 9).
3. **Rounds and `transform=`** naming a FUNCTION. Deterministic transforms work from here. **Done**
   (round 9).
4. **Bit-field builtins:** `BITLEN` and `BITPACK`, with `bits`, `gamma`, `join` and `cost` as library
   FUNCTIONs (5.7). Enough to hand-write simple formats (run-length encoding, say) with no parser.
   **Done** (round 9).
5. **`LZ_PARSE`**, then the Exomizer helpers. Both target formats become writable. `LZ_PARSE` **done**
   (round 9); the Exomizer helpers (`FIT_BUCKETS`, `BUCKET`) are not started.

`SPEC` comes back only if a format turns up that FUNCTIONs over `LZ_PARSE` cannot express and a
grammar demonstrably would.


## 7. How the optimal parse works

Rewritten in round 8, step by step, including the part round 1 only sketched: what to do about the
counts. Each run's count is written before its contents, so the cost of a run is not known until the
run ends.

### What is being searched

The data is n bytes long. A *position* p means the first p bytes have been produced. A parse is a
route from position 0 to position n made of runs, and its price is the number of bits those runs
write. The cheapest route is the optimal parse.

The pitch's format has two kinds of run, which alternate, starting with literals:

- **A literal run** of n bytes costs `gamma(n) + 8n` bits.
- **A match run** of k matches costs `gamma(k)`, plus each match's cost: `gamma(len - 1)` for its
  length, and `gamma(((off - 1) >> OB) + 1) + OB` for its offset.

(A gamma code for n costs `2 * floor(log2 n) + 1` bits: 1, 3, 3, 5, 5, 5, 5, 7...)

Because runs alternate, the search has to know which kind comes next, so it keeps two prices for
every position:

- `L[p]`: the cheapest way to produce p bytes that ends with a literal run (so matches come next);
- `M[p]`: the cheapest way that ends with a match run (so literals come next). The start counts as
  this state: `M[0] = 0`, and `L[0]` is impossible.

Every run moves forward, so Baron fills in the positions from left to right, and when it reaches a
position its two prices are final. The answer is the smaller of `L[n]` and `M[n]`. Each price
remembers which run produced it, so Baron can walk back from the end and recover the runs.

### Literal runs: the count is part of the move

A literal run's count comes first, but every literal costs the same 8 bits, so a whole run from
position i to position j has a known price: `gamma(j - i) + 8(j - i)`. A literal run can therefore be
a single move, count and all:

> `L[j]` = the smallest, over every i before j, of `M[i] + gamma(j - i) + 8(j - i)`

No extra state is needed. Trying every i would be slow on long data, but gamma has only 16 distinct
costs for counts up to 65535. Within one band of equal gamma cost, the price is a constant plus 8 per
byte, so the best i in each band is a sliding-window minimum: about 16 lookups per position.

### Match runs: the count is extra state

A match run cannot be one move in the same way. Each match is chosen separately, with its own length
and offset, and there are far too many combinations to list. So the run has to be built match by
match. That is where the problem appears: halfway through a run, its header cost is still unknown,
because it depends on how many matches the run ends up with.

The fix is to charge the header when the run closes, and to carry the number of matches so far as
part of the state. Inside a match run, a state is a pair (k, c) at a position: k matches so far,
costing c bits, with the header not yet paid.

- **Opening** a match run at position i gives the pair (0, `L[i]`).
- **A match** from position p to position q turns a pair (k, c) at p into (k + 1, c + the match's
  cost) at q.
- **Closing** the run at q gives `M[q]` = the smallest, over the pairs at q with k >= 1, of
  `c + gamma(k)`.

Carrying every possible k would be expensive: up to n/2 counts at every position. Most of them are
pointless, though. Compare two partial runs that have reached the same position:

- **(3 matches, 100 bits) and (5 matches, 104 bits).** The second has more matches *and* costs more.
  Whatever comes next happens to both alike, and a header for 5 + f matches is never cheaper than one
  for 3 + f. The second can never win, so it is dropped.
- **(3 matches, 100 bits) and (5 matches, 99 bits).** The second is cheaper so far, but will pay for a
  bigger header. Closed now: 100 + `gamma(3)` = 103 against 99 + `gamma(5)` = 104, so the first wins.
  After one more match each: 100 + `gamma(4)` = 105 against 99 + `gamma(6)` = 104, so the second wins.
  Either can win, depending on what comes next, so both are kept.

So at each position Baron keeps a *staircase*: partial runs in which every extra match buys a
strictly lower cost so far. Everything else is dropped. That is exact, because a header's cost never
falls as the count grows: a partial run with fewer matches and no higher cost can never end up worse.

The staircase stays short. A partial run with fewer matches is only ever ahead by its smaller header,
and two gamma codes differ by at most about `2 * log2` of the ratio of their counts. So a partial
run with fewer matches that is already that much dearer can be dropped too. Costs are whole bits, so
each step down the staircase saves at least one bit, which bounds the staircase at about `2 * log2 n`
entries: about 32 for 64 KB of data. In practice we expect a handful; to be measured.

When a format's header cost grows by a fixed amount per item (a flag bit per literal or match, as in
Exomizer or LZSS), the header can be charged as each item is added, and the staircase never has more
than one entry. A format with variable-cost literals would build its literal runs the same way as
match runs.

### Where the matches come from

At each position, every possible length has a cheapest offset: the nearest earlier occurrence that
matches at least that many bytes, because in this format an offset's cost never falls as offsets grow.
Walking the earlier occurrences nearest-first (hash chains, or a suffix array), and noting each time a
longer match turns up, gives every useful (length, offset) pair. BBC-sized inputs are small.

Very long matches, such as a run of zeros, offer many candidate lengths at every position; that is
the main cost of being exact. A length's cost only changes at powers of two, which `LZ_PARSE` can
exploit. To be measured.

### Every OB

Each `OB` changes only the offset costs, so the search runs once per value, sharing the match finding,
and the smallest result wins. In 5.8 that is the comprehension `{parse_with(ob, data) FOR ob = 0..7}`.

### Bits into bytes

The search works in bits. Laying the bits out into bytes, whether as a pure bitstream (`BITPACK`) or
as a bit buffer interleaved with byte reads (deferred, 5.6), is a final step that replays the moves the
way the decoder reads them. The layout changes the order of bytes, not the cost.

### Speed

Sections are at most a few tens of kilobytes. An optimal parse at that size in C should be fast, eight
times over for the `OB` values, but we would measure rather than promise. With rounds and caching, it
runs about once per assembly.


## 8. Smaller points

### 8.1 Getting results back into the source

- **Sizes need nothing new.** A label before and after the transformed section in the parent gives
  the transformed size; labels at the start and end inside it give the original size.
- **Chosen parameters** (`OB`) are open. The example writes `OB` into the stream header, so the
  decoder needs no feedback. But a decoder assembled with `OB` as a constant is smaller and faster,
  and the feedback is harmless when the decoder sits outside the transformed section. Some shapes:
  pin a parameter from the section (`transform="lz", ob=5`, passed to the FUNCTION), or have the
  transform return extra values that Baron binds as symbols.

### 8.2 Listing and symbol dump

- `-v` lists a transformed section's source with its untransformed bytes (those are what runs), and
  adds a summary line at its `ENDSECTION`, along the lines of `lz: 4096 -> 2311 bytes`. The parent
  never re-lists a child's bytes anyway.
- The symbol dump's section entry gains the transform's name and both sizes.

### 8.3 Other ideas considered

- **An attribute that runs an external tool.** Clunky in its own way (portability, finding the tool),
  and it means assembling someone else's source could run arbitrary programs. If ever added, it
  should need an explicit command-line opt-in.
- **Built-in named compressors** (`transform="zx0"`). Under the FUNCTION route these become library
  files of FUNCTIONs shipped with matching 6502 decoders.
- **Checking the decoder at assembly time.** The likeliest bug with a user-defined format is a
  mismatch between the compressor and the hand-written 6502 decoder, which only shows up on the
  machine. A small 6502 emulator in Baron could run the real decoder over the real output and
  compare. A big addition, recorded as a possible future.


## Open questions

1. **The staged plan** (section 6): agreed, and in that order?
2. **Loops** (5.11): the comprehension `{expr FOR x = seq}` is built (round 9). Still open: an `IF`
   filter, and running state when a transform needs it. Round 9 also showed the example does not need
   comprehensions at all (divide-and-conquer recursion builds an identical disc), so keeping them is a
   readability call.
3. **Recursion cap:** 128 (decision 11). More depth would come from trimming the parser struct copies on
   the recursion path, not from compiler attributes.
4. **A conditional expression** (`IF(c, a, b)` as a function) would shorten many bodies. Wanted?
5. **`LZ_PARSE`'s structure.** Is "alternating runs with count-dependent headers" (5.9) a good
   enough starting point, growing as formats demand?
6. **Diagnostics from a round that guessed** (2.6): built the simple way - a round whose transform sizes
   moved has its diagnostics thrown away. An *error* in a guessed round still stops the assembly, even if
   the right sizes would have avoided it; reserving 0 bytes the first time makes that unlikely (layouts
   only grow), but an exact rule may be wanted later.
7. **How to represent bits** (5.5): settled for now (decision 10): numbers with a marker bit, and
   `BITPACK` packs bits only. Bytes read straight from the stream (5.6) come back later, as a
   separate layout step.
8. **`BITPACK`'s details:** built fixed - most significant bit first, last byte padded with zeroes. An
   option for least-significant-first (`LSR` decoders, as beebcrunch uses) is easy to add.
9. **Own `org`:** not required (round 9). A transformed child continuing its parent's addresses gets
   labels at addresses its transformed bytes do not occupy, which only makes sense for same-size
   transforms; worth a warning later, perhaps.
10. **Chosen parameters.** How should values the compressor picks reach the source, if at all (8.1)?
11. **Naming** of `transform=` and the builtins.
12. **Wider bitwise operators?** `<<`, `>>`, `AND`, `OR` and `EOR` work on 32-bit patterns, so
    marker-bit FUNCTIONs use `^` and `+` to reach 52 bits (5.5). Widening the operators to 53 bits
    would allow either spelling, but changes existing results (`1 << 32` is 0 today, and `NOT(0)`
    is &FFFFFFFF). Leave them as they are?


## Discussion log

**Round 1 (2026-10-17).** Read the pitch. Restated the example format and its open details. Argued
that a compressor cannot be derived from an arbitrary decoder program, but can be from a description
with built-in moves and fields written as values to encode; sketched a `SPEC` block (now Option A).
Raised the assembler problems: convergence, ZA_AUTO placeholders, `INCBIN`'s settling-pass shortcut,
caching.

**Round 2 (2026-10-17).** The user's answers: defer the transform to a final step and trigger
another pass for dependent labels; `INCBIN` is not a problem (correct once the transform is deferred:
the concern was `INCBIN` inside a transformed section, under the run-every-pass draft); cycles are a
failure to settle; describe compression, not decompression; support the pitch's format and something
like Exomizer; settling again after allocation is fine. The user asked whether FUNCTIONs alone, with
new high-level builtins and tail recursion or iteration, could do the job. Turned the deferral into
rounds (section 2). Tested FUNCTIONs: deterministic transforms work today; compression hits the depth
cap and the per-call cost (and an arena-exhaustion crash).

**Round 3 (2026-10-17).** The user allowed local experiments with function handling, suggested naive
recursive list handling might be exponential in memory, and asked for an opinion on FUNCTION versus
`SPEC`, preferring to unify around FUNCTION. Measured in a scratch build with the cap lifted: C stack
overflow between 3000 and 4000 levels; list recursion quadratic, not exponential (exponential only
when shared structure is copied); 40-byte list elements. Found that `+` broadcasts over lists of
strings, which makes strings of `0`/`1` a compact, broadcast-friendly bit field. Recommended unifying
around FUNCTION with lightweight frames, function values plus native `MAP`/`SCAN`, and a bit-field
toolkit in which `COST` measures the encoder (section 6). No changes made to the working tree.

**Round 4 (2026-10-17).** The user turned down bit fields as strings and asked whether a native
`rc_bitset` type would hit the evaluator hard, whether it is too niche, and whether a plain list of
numbers with a function that appends a byte or modifies the last one would do; also how ZX0's
directly-read literal bytes fit; and noted that the arena panic is by design, presumably from a
4 GB allocation. Answers: a native type is moderate work (no parser change, about twenty dispatch
sites) but niche; a writer threaded through user code needs extra state, cannot broadcast, and is
quadratic or unsafe under immutable values, so the same writer belongs inside `PACK`, run over a
plain-list description (numbers are bits, strings are directly-read bytes, `ZIP` joins per element).
ZX0 needs the writer to keep filling a bit byte that is no longer the last byte, which `PACK` does.
The panic was the CLI's 256 MB reservation, not 4 GB (about 350 MB in use in total).

**Round 5 (2026-10-17).** The user turned down numbers-as-bits with strings-as-bytes (with two
representations anyway, strings for bits would be no worse), parking the bit representation for
later, and asked to start on lightweight call frames.

**Round 6 (2026-10-17).** The user suggested representing an N-bit field as the single number
`N`-bit value `OR (1 << N)` (the top set bit marks the width), with a `BITPACK` function to
concatenate a list of them into bytes. Recorded in 5.5 as the leading candidate: compact, no new
type, encodings and joins become plain broadcasting arithmetic, costs are `BITLEN(x) - 1`. Open:
telling directly-read bytes apart (a sign convention suggested).

Step 1 implemented in the same round. A call's parameters and locals now live in a frame in the
evaluation's arena, and step-1 slices are views. Measured with the recursion cap lifted in a scratch
copy:

- Head-and-tail recursion over 2000 items: 188 MB and 0.68 s before, 13 MB and 0.02 s after.
- Building a list by repeated `CONCAT` is still quadratic (each `CONCAT` copies), 177 MB down to 97 MB
  at 2000 items.
- Temporaries still accumulate for the whole of one evaluation: the 16000-call halving map still runs
  out, now in the 256 MB scratch reservation. Step 2's native `MAP`, freeing each call's temporaries,
  is what addresses this.
- The C stack still overflows between 3000 and 4000 levels.
- The examples use less memory (spritescale 30 MB down to 21 MB) and assemble byte-identically.

Visible changes: a body local can be read only after its assignment runs (it used to be readable from
the second pass on, from the previous pass's frame); a dotted local is refused; `FUNCTION`
parameters and locals no longer appear in `--symbols`; `@-` and `@+` in a body search the definition's
scope.

**Round 7 (2026-10-17).** The user ruled that `BITPACK` should stay purely about bits, for simplicity,
for the moment. Recorded as decision 10 with the marker-bit representation; the sign convention for
directly-read bytes is dropped and ZX0's buffered layout (5.6) is deferred, to come back as a
separate layout step if needed. Rewrote the toolkit (5.7) and the pitch's format (5.8) for
marker-bit fields: `BITLEN` and `BITPACK` are the only bit builtins, and `bits`, `gamma`, `join` and
`cost` are four one-line library FUNCTIONs. Corrected round 6's 52-bit claim: Baron's bitwise
operators work on 32-bit patterns, so fields reach 52 bits only when written with `^` and `+`
(open question 12). A side benefit: anything below 1 is not a field (`gamma(0)` is a half), so with
`BITLEN` 0 there, an impossible value costs -1 and needs no error elements.

**Round 8 (2026-10-17).** The user asked to move on to step 2. Probing showed that a bare FUNCTION
name is an undefined symbol today, that a symbol and a FUNCTION can share a name, and that `name (5)`
with a space is not a call. The user first chose bare names and a limit of 256, then objected
mid-implementation: functions always come with their `(`, and parsing them without it rubs against
Baron. Placeholder (`dbl(?)`) and string (`"dbl"`) spellings were offered too, and the user was happy
with none, asking why function values are needed at all. The answer: they are not; loops are
(5.11). Proposed a comprehension, `{expr FOR x = seq}`. The per-call memory release and the limit of
256 stand regardless. Rewrote 5.8 (the pitch's compressor) and 5.10 without function values, and
rewrote section 7 to explain the optimal parse step by step, including how a match run's count is
carried as state (the staircase). Nothing implemented yet; the start of a `function` value type was
backed out of the working tree.

**Round 9 (2026-10-18).** The user asked to build the whole feature in one go, with a new
`examples/compress` that INCBINs a MODE 5 title screen (Blurp, from beebcrunch's corpus) into a section
compressed in the pitch's format, as a child of a loader that unpacks it to the screen. Mid-way the user
said beebcrunch's own spec was fine as the test case. Its v1 codec is the pitch's format with byte-aligned
literals (read straight from the stream, the deferred layout of 5.6), counts capped at 256, `B` in 1..8 and
the match fields in another order, so the example keeps the pitch's pure bitstream and v1 became the test
of the parse: under v1's cost model, `lz_parse` finds exactly v1's 3142 bytes (at `B` = 7), and the pitch's
format packs the screen to the same 3142.

Built:

- `src/lzparse.{h,c}`: the parse of section 7, with a unit test against an independent exact search (every
  offset tried, every count kept apart) on 60 small inputs.
- Comprehensions; every call and comprehension step works in a second "spare" arena and hands back a copy
  of its result, so its working memory goes as soon as it returns (`expression_parse` takes the spare;
  assemble passes `per_pass`).
- `BITLEN`, `BITPACK`, `LZ_PARSE`.
- `transform=`: sections keep a cross-pass record per transformed section (last input and output, in
  permanent); settling passes reserve the last output's size in zeroes; the output pass (now forced
  whenever a transform exists) runs the FUNCTION unless the cache has the same input; a size change
  starts another round (`run_passes` now loops `run_round`, at most 8 rounds), discarding that round's
  diagnostics. Errors for an unknown FUNCTION, a non-string name, a non-byte result, a virtual section, a
  reopened one, and sizes that never settle. `-v` notes `; transform "lz": 8320 -> 3142 bytes`.

The example assembles in 0.4 s (release). Checked three ways: a Python decoder written from the pitch's
pseudocode rebuilds the screen exactly from the packed bytes; a Python 6502 interpreter running the loader
rebuilds it in screen memory exactly; and beebjit, booting the `.ssd` headless, shows the title screen.

Along the way: the recursion limit came down from the agreed 256 to 128 after measuring the stack (decision
11). A noinline attribute that trimmed frames was taken out at the user's request; the frames it saved
(about 1.3 KB a level, mostly the comprehension code inlined into `parse_precedence`) are the place to
look if more depth is ever wanted. The user asked whether comprehensions were needed at all: a
comprehension-free `lz.6502` (divide-and-conquer recursion over the runs, a recursion over the eight `OB`
values) builds a byte-identical disc.

Then, at the user's suggestion, the example's decompressor was rewritten in the manner of their richcrunch
`decompress_lz.6502`: a self-modified `LDY &FFFF` as the source pointer, the sentinel refill that needs no
`PHA`/`PLA` (shifting the sentinel out leaves carry set, so one `ROL` puts it back), values built in A and
`temp`, the gamma decoder that counts zeroes from X = -2 and tail-calls `getbits`, `PLA : PLA : RTS` once
the output is complete, and a Y-indexed match copy. That copy needs matches of at most 256 bytes, which
`lz.6502` arranges by ending its length table at 256 (a longer repeat takes several matches, so the format
is unchanged, and the screen still packs to 3142 bytes). The decompressor went from 250 to 205 bytes and
from about 429,000 to 318,000 instructions; all three checks pass again.

**Round 10 (2026-10-18).** The user accepted the need for a loop but not the syntax `{expr FOR x = seq}`:
braces suggest wrapping a list when the result already is one, and the binding should come before the
expression. Reframings discussed: Baron's operators broadcast all the way down while FUNCTIONs take values
whole (APL's "pervasive" versus "each"); a per-argument `EACH(...)` marker (rejected: less general than a
comprehension); `FOR x IN seq -> expr` (rejected: `IN` is not how statement `FOR` binds); and
`FOR x = seq -> expr`, which keeps the statement's binding at the price of two `=` in `y = FOR x = ...`.
No decision yet, so comprehensions were **reverted** (parser, scan mode, frame parent links, the
RND-in-scan rule, tests and docs) until the syntax feels right; the user also found the change to
expression parsing too big. Kept: the spare-arena call frames, `expression_call_function` (the transform's
only way to call a FUNCTION by name), the 128 recursion limit and the three builtins. `lz.6502` is now the
comprehension-free version (a recursion over `OB` and a halving recursion over the runs) and builds the
same disc. Also documented a third `LZ_PARSE` assumption that the guide, reference and `lzparse.h` had
missed: a match run's count never gets cheaper as it grows (the staircase pruning in section 7 relies on
it).

**Round 11 (2026-10-18).** The user asked for an Exomizer-style compressor beside the LZ one:
`compress.6502` became `compress_lz.6502` (disc name LZ, still what `!BOOT` runs), and the new
`compress_exo.6502` (disc name EXO) unpacks the same screen through `exo.6502`. The format keeps the
alternating runs; every number is written with a fitted table of 16 buckets (bucket i holds 2^w numbers
from 1 up, and is written as i ones and a zero, then w bits). Three tables, 4-bit widths in the header:
run counts (literal and match runs share one: 4 bytes smaller here than separate tables), lengths - 1,
offsets. A first cut put the fitting in C as the planned `FIT_BUCKETS`; the user asked for exo "entirely
with lists and LZ_PARSE", so it was deleted and the fit is FUNCTIONs: the histogram is
`FIND(SORT(CONCAT(values, 0..e)), 0..e) - (0..e)`, the search a backward pass over buckets with every
width tried as one list step per width (shifts are range slices, so views; per-width counts computed once
per fit; an element-wise minimum `b + (a < b) * (a - b)`), and a short trace forward. It matched the C fit
wherever the C fit's "never cheaper" rule did not bind; it does not enforce that rule, so the final offset
table can make a farther bucket cheaper and `LZ_PARSE` is then not guaranteed exact (the stream came out the
same size as with the rule). The refit loop stops as soon as a round saves nothing. Results: 3066 bytes
against LZ's 3142, decompressor 252 bytes against 205 (it rebuilds width and base tables from the header,
and `getbit` now keeps Y, at one byte's cost, so `getbits` can count in Y); 1.30M cycles against 1.22M.
Building the disc takes 2.1 s in release (the exo fit is most of it). Checked by a Python decoder, a
Python 6502 interpreter running both loaders, and beebjit booting each. Found on the way: `DIV` works in
32 bits, so `2^40 DIV 2` is 0.
