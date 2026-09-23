# Zero-page allocation: internals #

The technical companion to [the usage guide](zero-page-allocation.md), for anyone changing the allocator -
or just curious how it works. No compiler background is assumed: the ideas are introduced as they are
needed, and every example here is a real program with the addresses Baron actually assigns.

## Contents ##
- [The problem](#the-problem)
- [Live ranges, interference, colouring](#live-ranges-interference-colouring)
- [Why an assembler makes this interesting](#why-an-assembler-makes-this-interesting)
- [The pipeline](#the-pipeline)
- [Recording: what the analyses see](#recording-what-the-analyses-see)
- [Easy checks first: bounds and indexed access](#easy-checks-first-bounds-and-indexed-access)
- [The control-flow graph](#the-control-flow-graph)
- [Liveness: walking backward](#liveness-walking-backward)
- [Calls](#calls)
- [Roots, reachability and interrupts](#roots-reachability-and-interrupts)
- [Colouring](#colouring)
- [The output pass](#the-output-pass)
- [The certainty contract](#the-certainty-contract)
- [Known gaps](#known-gaps)
- [Map of the code](#map-of-the-code)

## The problem ##

The user declares named zero-page variables and hands us a pool of bytes. We must give each variable a
real address such that the program still behaves exactly as written - and we want to be *stingy*: two
variables should share a byte whenever that is provably safe.

When is it safe? Exactly when their values are never needed at the same moment. That one sentence is the
whole design; everything below is the machinery for making "never needed at the same moment" precise, and
for proving it from the instruction stream rather than guessing.

Compiler folk will recognise this as **register allocation by graph colouring**, with the zero page
standing in for the register file. If that phrase means nothing to you, don't worry - the next section builds it
from scratch.

## Live ranges, interference, colouring ##

**A variable is *live* when its current value is still needed** - some instruction later on the path will
read it before anything overwrites it. It becomes live when written, and dies at its last read. The span
in between is its *live range*.

Here is a real program (it is test `za_auto_allocates_and_reuses` in `assemble.c`), with each variable's
live range drawn beside the code:

```
                                 in1   tmp   out1
    LDA in1                       |                  in1 is an input: live from entry
    ASL A                         |
    STA tmp                       |     |            tmp born while in1 still lives
    LDA in1                       x     |            in1's LAST read: it dies here
    CLC                                 |
    ADC tmp                             x            tmp's last read
    STA out1                                  |      out1 born - nobody else is alive
    RTS
```

**Two variables *interfere* when their live ranges overlap** - at some moment both values matter, so they
must not share a byte. Draw every variable as a node and every overlap as an edge and you have the
*interference graph*:

```
    in1 --- tmp        out1
```

`in1` and `tmp` overlap (look at the `STA tmp` line: both bars present), so they get an edge. `out1` is
born after both are dead - no edges at all.

**Colouring is assigning addresses so that neighbours differ.** Walk the nodes, give each the lowest pool
byte that no neighbour already holds:

```
    in1  = &70
    tmp  = &71     (in1 is a neighbour, so not &70)
    out1 = &70     (no neighbours - reuse the first byte)
```

Three variables, two bytes, provably safe. That is the whole idea. Everything else in this document is
about computing the live ranges *correctly* - a range that comes out too short lets the colourer
overlap two values that both matter, and that is a silent runtime corruption. So the governing rule
throughout: when in doubt, a range gets **longer** (costing at worst a byte), never shorter.

## Why an assembler makes this interesting ##

A compiler allocates registers with the source structure in its hands. We have a raw instruction stream,
which changes the game in a few ways:

- **There is no structure to lean on.** Braces are *naming*, not lifetime: control happily branches out
  of a `{ }` block or into one, and one block can hold three routines. We must recover the real control
  flow from the branches themselves.
- **Layout settles over many passes.** A forward reference can change an instruction's size, shifting
  every later address - so we cannot allocate mid-assembly. Instead, `ZA_AUTO` binds its name to a
  **typed placeholder value** (`value_type_za_auto` in `src/value.h`) carrying the variable's identity
  and a byte offset, never a number.

  Every context that would let the layout depend on an address - a condition, a count, a section
  `org` - *refuses* the placeholder with an eager error. The contexts that accept it (instruction
  operands, immediates, data elements) emit the same number of bytes whatever the address turns out
  to be: `LDA var` is a two-byte zero-page instruction from the very first pass.

  So the layout provably never depends on the allocation. The analysis runs **once, after
  convergence** (`zeropage_finalize` in `src/assemble.c`), and one final re-emission - the
  [output pass](#the-output-pass) - produces the true bytes.
- **Subroutines, computed jumps, paged banks and recursion** all threaten to hide a read from us - and a
  hidden read is a too-short range. Each gets its own machinery below.

## The pipeline ##

Everything runs inside `zeropage_finalize` (`src/assemble.c`), once, after the final pass:

```
  during assembly (every pass)          zeropage_finalize (once, post-convergence)
  ----------------------------          ------------------------------------------
  ZA_POOL    -> reserved-byte set       1. resolve vregs   (operand identity -> variable id)
  ZA_AUTO    -> typed placeholder       2. bounds + indexed-access checks
  final pass -> record the IR           3. build the CFG   (basic blocks + edges)
                                        4. liveness        (ranges -> interference graph)
                                        5. call analyses   (footprints + the call checks)
                                        6. colour          (first-fit-decreasing)
                                        7. rewrite symbols (or refuse)
                                        8. the OUTPUT pass re-emits with the real addresses
```

If any check refuses, we record diagnostics, rewrite nothing, and the assemble fails. Only a fully
analysable, colourable program gets real addresses.

## Recording: what the analyses see ##

Only the final pass records anything - earlier passes exist to let the layout settle. The IR
(`src/zeropage.c`, filled by `record_insn` in `src/opcodes.c`) is four flat arrays:

- `zp_var` - one per declaration: name, owning scope, width (1-256), and its **def cursor** (the source
  position of the `ZA_AUTO` statement).
- `zp_insn` - one per *instruction*: its address and size, its control-flow class (`zp_flow`), which
  variable it touches and how (read or write, at what offset, indexed or through a pointer), and
  where a branch, jump or call goes.
- `zp_cflow` - the annotations: `ZA_UNREACHABLE`, `ZA_CANCALL`, `ZA_CANJUMP`, `ZA_RETURN`, `ZA_RETURNTO`.
- `zp_label` - each label's identity mapped to its physical `(section, pc)`.

How an instruction touches its operand comes straight from the opcode table: `LDA zp` reads its byte,
`STA zp` writes it, `INC zp` does both. A not immediately obvious case is `STA (zp),Y` is a zp *read*,
not a write.

One case needs help. Some indexed accesses have no zero-page encoding at all (`LDA var,Y`, say), so
the assembler quietly emits the absolute form instead - and the absolute table entries carry no
read/write flags, because an absolute operand normally points well above the zero page. Here it
doesn't (the base is a pool variable, or a literal address under &100), so the recorder takes the
flags from the mnemonic's zero-page entry.

Without the borrowed flags these accesses would be invisible: a table read only via `,Y` would look
unused, and its bytes could be handed to another variable while still in use.

A few non-instructions share the stream. The `BITZP`/`BITABS` skip trick records as a real one-byte
instruction (`zp_flow_skip`); plain `EQUB` data records nothing - which is exactly why the raw
spelling of the trick cannot be modelled. Labels, `ZA_DISCARD` and `ZA_WIPE` record as size-0 markers
(`zp_insn.marker`); see [Liveness](#liveness-walking-backward) for what they mean. Finally, an
operand with a fixed zero-page address that names no variable keeps that address in `literal_addr` -
the raw material for the pool-store check below.

Two details worth knowing:

- **A variable's identity is the pair (scope, def cursor)** - not the def cursor alone. A macro or `FOR`
  body shares one `ZA_AUTO` across every instantiation, but each instantiation runs in its own child
  scope, so the pair tells the copies apart (each iteration's `loc` is a genuinely distinct variable).
  Operands record the pair; `zeropage_resolve_vregs` maps pairs to dense ids once the registry is
  complete, which is also why a use may precede its declaration.
- **Attribution comes from the VALUE.** A ZA_AUTO reference evaluates to the typed placeholder, which
  carries its own identity - so `record_insn` reads the touched variable straight off the evaluated
  operand. Dotted paths (`STA sub.wid`), locals, and even *aliases* (`x = var : LDA x` - the identity
  rides through the assignment) all attribute through one uniform channel. Only control-transfer
  TARGETS still resolve by lexing the operand text (labels are plain numbers, with nothing to carry).

  One instruction plays both roles at once: a `JMP` through a ZA_AUTO vector names the variable as
  its target *and* reads its bytes, so it is recorded as a pointer read too. Without that, liveness
  would let another variable take the vector's bytes between its last store and the jump.

## Easy checks first: bounds and indexed access ##

Some mistakes are visible in a single instruction, so they are caught before any graph is built (the
first loop in `zeropage_finalize`).

**`var + n` must stay inside the variable.** During assembly a variable's symbol is a placeholder
whose value is 0, so an operand like `var+3` evaluates to plain 3 - which *is* its offset within the
variable. An offset past the declared width would reach a byte reserved for someone else, so Baron
refuses.

A pointer dereference like `(var),Y` reads two bytes, and trying it on a one-byte variable gets its
own pointed message: declare it `ZA_AUTO2`. The same bounds rule covers the fixed part of an indexed
access - `table+4,X` on a 4-wide table is off the end before X has any say.

**An indexed access is a warning, not a refusal.** The whole variable is reserved, and liveness
treats an indexed touch as using all of it, so an index that stays within the declared width is safe.
Whether it *does* stay within the width is the one thing no assembly-time check can know. That part
is trusted to the user, and said out loud with a default-level warning.

`ZA_INDEXEDBY` turns that trust into a check. Declaring the possible index values stamps the largest
one onto the access record, and this loop then verifies the whole reach: the fixed offset plus the
largest index must fit inside the variable, with two bytes of room for the pointer forms `(var,X)`
and `JMP (table,X)`. Out of range is refused; in range, the warning is dropped - there is nothing
left to warn about. Liveness is unchanged either way: the whole variable stays live on an indexed
touch, declared or not.

**A store to a fixed address inside the pool is a warning too** (opt-in, `--warn 2`). Such a store -
a plain fixed address, or an indexed store's fixed base - names no variable, so it writes memory the
allocator believes it owns without any analysis seeing it.

The check runs here rather than while recording because `ZA_POOL` statements fill the reserve set in
source order, and a pool declared *after* the store would be missed mid-pass. The one blessed
exception is the boot-time wipe: a `ZA_WIPE` marker after the sweep declares it deliberate, and its
stores stay quiet - the blessing runs from each store forward to the marker, reaching through the
rest of the wipe loop but stopping at a label, jump, call or return, so a stray pool store anywhere
else still warns.

## The control-flow graph ##

To trace lifetimes we must know where control can go. The unit for this is the **basic block**: a
maximal straight-line run of instructions - one way in (the top), one way out (the bottom). Within a
block, execution is a simple sequence; all the interesting control flow lives on the **edges** between
blocks.

`cfg_build` (`src/cfg.c`) recovers blocks from the stream in three passes:

1. **Find the leaders** - every instruction where a block must start: the first instruction, every
   branch/jump/call target, the instruction after any branch, jump or return, every section boundary,
   and every annotation-declared target (`ZA_CANCALL`/`ZA_CANJUMP`/`ZA_RETURNTO`).
2. **Cut the stream** into blocks at the leaders - and after every branch, jump or return, so a
   transfer is always the last instruction of its block. (A call cuts too when an annotation
   reroutes or severs its continuation, so the next pass can express that by leaving an edge out.)
3. **Wire the edges** from each block's last instruction.

A tiny loop, and its graph:

```
    .loop                        +------------------+
        DEX                +---->| b0:  DEX         |
        BNE loop           |     |      BNE loop    |
        RTS                |     +---+----------+---+
                           |  taken |          | fall through
                           +--------+          v
                                        +-----------+
                                        | b1:  RTS  |
                                        +-----------+
```

`b0` has two successors: itself (the branch taken) and `b1` (the fall-through). `b1` ends in `RTS` and
has none. Liveness will walk these edges; braces never appear anywhere in the picture.

That is the skeleton. The rest of this section is the rules that keep the graph matching what the
CPU will really do.

### Blocks are named by section and address ###

Two paged banks may both sit at `&8000`, so a block's identity is the pair `(section, pc)`, never
the address alone. Within one section the pc only ever moves forward (asserted in debug builds), so
the pair is unambiguous.

### A JSR is not an edge ###

A call stays *inside* its block: the callee is reached through the call analyses, not a graph
edge, and an `RTS` ends a block with no successor at all. A "routine" is simply whatever is
reachable from an entry point without crossing a call or return. See why that is the right shape - and
what it buys - over at the [Calls](#calls) section.

### Targets resolve by label ###

A branch or jump to a label finds the label's block through the recorded `zp_label` markers
(`cfg_target_block`) - which is how a transfer crosses sections and still lands in the right bank.
A bare numeric target resolves within its own section only.

An indirect `JMP`'s operand is different: it names the vector *cell* the address will be fetched
from, not the destination (`zp_insn.target_via`), so the cell's own address is never mistaken for
somewhere control goes.

### Constant destinations outside the program are exits ###

`JSR &FFEE`, a tail `JMP &FFEE`, a jump through a fixed OS vector like `JMP (&FFFC)`: the
destination matches nothing we assembled, so control has left the program
(`cfg_target_is_external`). External code cannot touch the pool - `ZA_POOL` names precisely the
bytes nothing else uses - so an external exit is as clean as an `RTS`, and needs no annotation.

### Flow we cannot place taints its block ###

An unannotated indirect `JMP` through our own vector, or an indexed dispatch `JMP (table,X)`, could
land anywhere. Its block is marked `unknown_succ`, which later forces "everything live" out of it -
and if any variables are in play at all, the program is refused (`za_auto_computed_flow`) rather
than guessed at.

A vector that is itself a `ZA_AUTO` variable counts as ours (`zp_insn.target_is_zpvar`): `JMP (vec)`
on a `ZA_AUTO2` is computed flow wanting a `ZA_CANJUMP`, never mistaken for a constant OS cell.

### Inline data leaves a gap, and fall-through hops it ###

```
    JSR pstring
    EQUS "HELLO", 0     ; data records no instructions - a gap in the stream
    LDA #0              ; the data-consuming callee resumes here
```

Data records nothing, so the pc just past the `JSR` holds no instruction and no block. The call's
fall-through edge therefore goes to the next recorded instruction in the same section - exactly
where the callee resumes, which is what makes this idiom sound with no annotation. Nothing found at
all is the end of the stream: a clean dead end. (This gap is also why pass 2 cuts after every
transfer on its own authority - the leader pass 1 plants just past a `JMP` may sit on empty air.)

A *branch's* not-taken edge gets no such favour: it stays pure pc arithmetic, because a not-taken
branch into data is a broken program, not a continuation.

### The BIT-skip records ###

`BITZP`/`BITABS` record as a real one-byte instruction with `zp_flow_skip`: a lone `BIT` opcode
whose operand fetch swallows the next one or two bytes. An example and the graph it produces:

```
        BEQ over
        ADC first
        BITABS              ; swallows the ADC below on the fall-through path
    .over
        ADC second
        STA total

    +------------------+   branch taken   +----------------+
    | b0: ... BEQ over |----------------->| b2: ADC second |
    +--------+---------+                  +--------+-------+
             | fall through                        |
             v                                     v
    +------------------+   skip edge      +---------------+
    | b1: ADC first    |----------------->| b3: STA total |
    |     BITABS       |   (never to b2)  +---------------+
    +------------------+
```

The swallowed instruction (`b2`) is a block of its own, entered *only* by an explicit branch - the
marker wires no edge to it. The marker's one successor is the resume point, `pc + 1 + skip_bytes`
(exact arithmetic: a BIT always fetches its full operand). Downstream, nothing needs a special
case - the swallowed store or read simply lives in a branch-only block, so liveness and must-write
come out right by structure.

One shape is refused: a recorded instruction *straddling* the resume point, such as a 3-byte
instruction under a `BITABS`. The two instruction streams would then interleave mid-instruction,
which no block structure can express (`error_type_skip_spans_instruction`).

### Annotations adjust the graph ###

Each annotation is one small edit to the edges:

- `ZA_UNREACHABLE` prunes a dead fall-through edge - a branch's not-taken arm, or the continuation
  of a never-returning `JSR`.
- `ZA_CANJUMP` wires a computed jump's (or self-modified branch's) declared targets as real
  successors, **in place of** any literal edge - the programmer's word beats a placeholder operand.
  External arms contribute nothing.
- `ZA_RETURN` declares a jump or branch a return to the routine's own caller: no edge at all - the
  return machinery in [Calls](#calls) supplies the meaning.
- `ZA_RETURNTO` reroutes a call's continuation to the declared resumption points. An arm with no
  block is off the stream: no edge, and finalize warns.

An `RTS` carrying a `ZA_CANJUMP` is the dispatch trick - push a target address, "return" into it -
and is wired exactly like an annotated jump. An *unannotated* dispatch is indistinguishable from a
real return, so it remains a trusted precondition, never a taint.

## Liveness: walking backward ##

Now the central question: at each point, which variables are live? The natural direction is
**backward** - liveness is about the *future* ("will this value be read again?"), and walking from the
end toward the start means that by the time we reach an instruction, we already know what the code after
it needs.

### One block ###

Walk a block bottom-to-top with a set of live bytes in hand:

- at a **read**, the value clearly matters: *add* the read bytes to the set;
- at a **write**, the old value is gone: *remove* the written bytes;
- at a **call**, apply the call's transfer (the [next section](#calls)).

Here is the walk over the earlier example - each line shows the bytes still wanted *after* it:

```
    LDA in1                       { in1 }
    ASL A                         { in1 }
    STA tmp                       { in1, tmp }
    LDA in1                       { tmp }
    CLC                           { tmp }
    ADC tmp                       { }
    STA out1                      { }
    RTS                           { }        <- start here, with nothing
```

Start at the `RTS` with the empty set and climb. `ADC tmp` reads `tmp`, so add it: it stays in the
set everywhere above, until `STA tmp` - the write that created it - removes it again. The second
`LDA in1` adds `in1`, which rides all the way to the top, since nothing above writes it. The result
is exactly the live-range bars of the [first example](#live-ranges-interference-colouring),
computed. (`out1` never enters the set: its reads are in the *caller*, which is rule 4 of
[Calls](#calls).)

### Chaining the blocks ###

A block's *live-out* - the set at its bottom - is the union of its successors' *live-in*: any
successor might read a value, so every one gets a say. A tainted (`unknown_succ`) block's live-out
is simply everything.

A loop makes this circular: the loop block's live-out depends on its own live-in. No cleverness
required - `liveness_analyze` (`src/liveness.c`) sweeps every block, then sweeps again until nothing
changes: a *fixpoint*. The sets only ever grow, so it terminates.

### Bytes, not variables ###

The dataflow tracks individual bytes, not whole variables. The case that forces this is a two-byte
pointer:

```
    ZA_AUTO2 ptr
        LDA #&20 : STA ptr+1    ; high byte written once, at setup
        ...
        STA ptr                 ; low byte refreshed before each use
        LDA (ptr),Y             ; ...but the deref reads BOTH bytes
```

`STA ptr` rewrites only the low byte; the high byte set up long ago still matters. Treating any
store as a whole-variable kill would sever the high byte from its dereferences and let another
variable land on it. Per byte, the lone low-byte store keeps the pointer live, while a
`STA ptr : STA ptr+1` pair accumulates into a genuine full kill.

`insn_window` maps each touch to its read/write byte windows; the public results are projected back
to variable level, since whole variables are what we place.

### Collecting interference ###

One more backward sweep builds the graph: at every write, the written variable gains an edge to the
owner of every other live byte. That single rule generates the entire graph - "the output can reuse
the dead input's byte" and all its friends fall out with no special cases.

### Classification ###

Each variable is finally classed `input` / `output` / `temp` / `unused`. Only `unused` matters in
production (the unused-variable warning, and the colourer's skip). The `input`/`output` split hangs
off an `entry_block` parameter that only the analysis's own tests pass; the driver passes none,
since there is no single presumed program entry. A value read at an entry before anything writes
it has no defined value and no protection, which is what the
[input warning](#roots-reachability-and-interrupts) is for.

### The size-0 markers ###

Some records in the stream are not instructions at all: size-0 markers, distinguished by
`zp_insn.marker` (`zp_marker_discard` / `zp_marker_wipe` / `zp_marker_label`; size == 0 exactly
when marker != none).

#### ZA_DISCARD: a declared kill ####

The problem it solves: an array rebuilt only through indexed stores has no provable write anywhere.

```
    .redraw
        ZA_DISCARD slots    ; the old contents are garbage from here on
        LDX #7
    .fill
        LDA newval
        STA slots,X         ; indexed - provably writes no PARTICULAR byte
        DEX
        BPL fill
```

Without the marker, later reads of `slots` keep the OLD value live clear back to the routine entry
(and, through a `JMP` entry, around the caller's loop). The marker's window "writes" the variable's
whole width while reading nothing, so the backward walk sees a full kill exactly where the
programmer says the old value stops mattering.

A discard is a promise, not a store, and three details follow from that:

- the interference sweep clears the bytes *without pinning* them - nothing is actually stored, so
  another variable may own the bytes at that instant;
- the footprint walk skips it entirely - a callee that merely discards touches nothing;
- the must-write analysis counts it in full - the caller's pre-call value is equally dead whichever
  path runs, so a discard-then-rebuild callee kills at its call sites like a provable rewrite.

#### ZA_WIPE: a declared pool-wide store ####

The same mechanism with every polarity flipped: where a discard kills one variable's value without
supplying one, a wipe *supplies* every byte in the pool. It sits just after the sweep:

```
        LDA #0 : TAX
    .clear
        STA &70,X : INX
        CPX #16 : BNE clear
        ZA_WIPE             ; every pool byte was just written
```

The marker names no variable, so each engine special-cases it just ahead of the per-variable window
machinery:

- the backward dataflow and the interference sweep reset the whole live set - clearing without
  pinning, like the discard, but of everything;
- must-write unions the full byte set: a wiping callee definitely rewrites its callers' values
  (when the wipe is on every path);
- may-write coverage does the same: a read after the wipe is initialised, which is what lets a
  program rely on the wipe for its zeroes;
- the footprint walk - which *skips* a discard - counts a wipe as touching and freshly writing
  everything, so a value held live across a call into a maybe-wiping routine conflicts with every
  variable, as it must.

Classification deliberately ignores it: a variable only the wipe ever writes is still unused.

#### Labels: binding a marker to its path ####

At a join, the same marker means two different things depending on which side of the label it sits:

```
        BEQ done
        ZA_DISCARD buf     ; ABOVE the label: only the path falling in discards
    .done
        ZA_DISCARD buf     ; BELOW the label: every path arriving here discards
```

Both markers sit at the same pc, so the address alone cannot tell them apart. The fix: labels
record their own size-0 marker in the stream (`record_label_marker`, beside the `zp_label` entry),
and stream order says which side of the label a marker was written on.

The CFG's block cut then lands on the label record itself (or, where there is no label, on the
first real instruction) - never on a discard or wipe. So a marker written above the label stays in
the block falling in, where the back edge and the other arms never pass it; a marker written below
the label belongs to the join block, and every arriving path passes it.

One exception: a marker just past a terminator has no fall path to bind to, so it rides the
terminator's carried cut into the NEXT block - which is precisely what the RTS-dispatch discard
idiom wants.

The engines skip label records for free (no variable, no read/write flags). The one place a label
record could mislead is "does code start here?" - a label on a data run records a marker, and a
leader-marked one opens a block - so the marker-on-data checks (`za_entry_no_code`,
`za_returnto_no_code`) test for a real instruction at exactly that address (`code_at`), never for a
block.

## Calls ##

A `JSR` is where naive liveness falls apart, because the callee's code is not on the caller's CFG path.

### Why a call is an instruction, not an edge ###

Because a call *comes back*. A graph edge means "control continues there", and an edge into the
callee would need matching edges from its `RTS` back to every call site's continuation. On one
shared graph an `RTS` cannot know which caller it is serving, so everything live around one call
site would smear into every other - and the four-variables-in-one-byte relay below survives only
because each call site is judged on its own.

So a `JSR` stays inside its block: an ordinary instruction with an unusually large effect. When the
backward walk steps over it, it applies a *summary* of the callee - inputs consumed, results
delivered, footprint clobbered (the rules below). External calls fall out for free - `JSR &FFEE`
has no code to wire an edge to, and its summary is simply empty - and a computed call is the union
of its arms' summaries rather than a fan of edges.

A block therefore holds several calls happily, in order - and the ordering is exactly what the
backward walk wants. The usage guide's relay is one straight-line block containing two calls; as
before, each line shows the bytes still wanted *after* it:

```
    LDA #5 : STA scale.input      { scale.input }    wanted by the call below
    JSR scale                     { scale.result }   input consumed, result delivered
    LDA scale.result              { }                the value is in A now
    STA offset.input              { offset.input }
    JSR offset                    { offset.result }
    LDA offset.result             { }
    RTS                           { }                <- start here, walking upward
```

Stepping upward, the walk crosses `JSR offset`'s summary before `JSR scale`'s, so each argument is
born just before its own call and each result dies at its read: four ranges that tile perfectly,
one byte for the lot. A value the caller kept in hand across both calls would instead ride through
both summaries - and interfere with both callees' footprints, which is rule 1 below.

### The four rules ###

Four rules describe a call's effect on the walk: what the callee touches, what it consumes, what
it delivers, and where its returns lead.

```
   caller                                 callee
   ------                                 ------
   STA sub.wid  ------ argument ------.
   ...                                |
   JSR sub  ..... consumes In(sub) ...+....... kills MustWrite(sub) .....
     |                                v
     |                        .sub:  LDA wid
     |                               STA res -----------------.
     |                               RTS  <-- return edge     | result
     |                                        sees live-after |
   LDA sub.res  <---------------------------------------------+
```

**1. A call clobbers its callee's footprint** (`src/footprint.c`). A routine's *footprint* is every
variable it touches, transitively through everything it calls (`fp_visit`, a walk over the call graph).
Any variable live *across* a call interferes with the whole footprint - the **across-call check** in
`zeropage_finalize`. In this real test (`za_auto_interprocedural_allocation`):

```
    LDA #1 : STA keep
    JSR sub             ; keep is live across this call...
    LDA keep
    RTS
.sub { ZA_AUTO1 loc : STA loc : LDA loc : RTS }
```

`keep` gets `&70` and `loc` is forced to `&71` - without the rule both would take `&70` and the call
would corrupt `keep`. A variable already dead at the call is free to reuse the callee's bytes. `ZA_CANCALL` supplies the
targets of a call the analysis cannot follow; a call *out of the program* has an empty footprint.

**2. A call consumes its inputs.** The callee's live-in set - what it reads before writing - is treated
as *used by the call itself* (`liveness_analyze`, at each `zp_flow_call`). So an argument stored by the
caller stays live from the store to the `JSR`:

```
    LDA #3 : STA sub.wid    ; wid live from here...
    LDA #9 : STA t          ; ...so t cannot take wid's byte...
    LDA t
    JSR sub                 ; ...until the call consumes it
```

Without this rule `t` could take `wid`'s byte, and its store would silently overwrite the argument
before the call reads it. Since a callee's own live-in includes whatever *its* calls inject, the
rule chains through nested calls for free.

**3. A call delivers its results.** The dual rule: a forward *must-write* analysis (also in
`liveness_analyze`) computes, per routine, the bytes written on **every** path to a returning exit. A
call *kills* the intersection over its arms (`call_kill_bytes`) - the caller's read after the call can
only see the callee's value, so the pre-call byte is dead and a result's range *starts at its call*.
This is what lets the usage guide's two-stage pipeline run four interface variables in one byte. And
it really is a proof - make the store conditional:

```
.offset { ZA_AUTO1 xin, res : LDA xin : CLC : ADC #7 : BEQ @+ : STA res : .@ RTS }
```

Now `res` is not written on every path, so it is preserved across the whole journey on its own byte
(test `za_auto_conditional_result_is_preserved`).

What counts as a "returning exit" (`block_returns`): an `RTS` or `RTI`; a transfer out of the
program (the OS routine's own `RTS` returns to our caller - the tail-call idiom); and a jump or
branch wearing a `ZA_RETURN`. An `RTS` wearing a `ZA_CANJUMP` does not count - its control continues
at the declared targets.

**4. Returns see the caller.** Rule 3 creates a hazard: an escaping result's only reads are in the
caller, so inside its producer it would look dead the moment it is stored - and the producer's *later*
code could take its byte:

```
.sub { ZA_AUTO1 res, t : LDA #1 : STA res : LDA #2 : STA t : LDA t : RTS }
                                  ^ res "dead" here?     ^ then t may land on it!
```

So every returning block's live-out also unions the live-*after* set of each call site that reaches its
routine (the `ret_from` map and per-call `after` snapshots, maintained inside the fixpoint). `res` stays
live from its store to the `RTS`, and `t` gets its own byte (test
`za_auto_result_survives_producer_tail`).

**Recursion** gets one extra check: a value the cycle writes *afresh* at each level and reads back
after the recursive call would need a byte per level, which one static address cannot give - so it
is refused (`za_auto_recursion`). The check keys on the footprint's write-only `killed` set against
the live set *before* the must-write reduction, so the kill cannot hide the pattern. A counter
merely `DEC`ed through the recursion is a single running value and rides one byte happily.

## Roots, reachability and interrupts ##

Everything so far analyses the stream as it lies; nothing asks *where control can enter it*. Two
markers answer that: `ZA_ENTRY` ("the outside world calls in here") and `ZA_INTERRUPT` ("the
hardware jumps here"). Each records its `(section, pc)` (a `zp_entry` record); `cfg_build` marks it
a block leader, so a mid-run entry starts its own block; a marker that resolves to no instruction
is refused as marking no code.

### Reachability ###

The root set is every marker's block. With no `ZA_ENTRY` anywhere, the defaults apply instead:
each section's first recorded block (first sighting in stream order is its earliest code). A
`ZA_INTERRUPT` alone keeps the defaults - a handler says nothing about where the mainline starts.

From the roots, `reach_from` walks successor edges plus each call's resolved arms - the same edge
set control can actually take, `ZA_CANJUMP`/`ZA_CANCALL` arms included; unknown and external arms
contribute nothing.

A block that touches a `ZA_AUTO` variable and is not reached warns (`za_auto_unreachable`,
default-visible). The shape the warning exists to catch:

```
    .main
        ...                 ; the mainline, ZA_AUTO variables everywhere
        RTS

    .irq                    ; nothing in the program jumps here - the HARDWARE does
        INC framectr
        RTI
```

No instruction targets `.irq`, so the analysis believes its code never runs: an invisible island
whose variables would silently be allocated over the mainline's. The warning names it; marking it
`ZA_INTERRUPT` both roots it and [pins it](#interrupt-pinning). Plain dead code draws the same
warning.

One warning per island, not per block: heads are unreachable blocks with no unreachable
predecessor, each head's closure is claimed once, and a headless cycle is mopped up separately.
Warnings never refuse - unreachable code is still fully analysed and coloured.

### Interrupt pinning ###

A `ZA_INTERRUPT` handler preempts between *any* two instructions, so there is no safe moment the
analysis could reason about. Instead, two blanket rules are injected as plain edges into the
interference graph (exactly like the across-call check's):

- **the handler's live-in is pinned against everything**, its own temps included. Live-in is the
  state the mainline feeds it - a frame counter, a "vsync happened" flag - and the mainline may
  rewrite it at any moment relative to the handler, so no instant of "dead" is reusable;
- **the handler's transitive [footprint](#calls)** (`footprint_compute` at its entry) **interferes
  with every variable outside it** - the handler may run, and touch all of it, between any two
  mainline instructions.

What the pinning leaves alone: reuse *inside* the handler (ordinary liveness governs its own
temps), and two handlers separate each other for free (each is outside the other's footprint). A
footprint the walk cannot bound - an unannotated computed call in the handler - is refused with
the usual `za_auto_across_call`, at the marker.

### ZA_ENTRY is only a root ###

Deliberately so. An external API's inputs and outputs belong at fixed addresses outside the pool -
an auto-allocated address moves between builds - and a strict poke-`CALL`-peek transaction is
already protected exactly as far as ordinary liveness provides. So a sync entry contributes
reachability and nothing else.

### The input warning ###

A sync root that reads a variable nothing in the program could yet have written is expecting its
*caller* to have set it:

```
    .entry
        LDA speed        ; no path from .entry writes speed before this read
```

That contract is impossible at an auto-allocated address (it moves between builds), so each such
variable warns (`za_entry_input`, default-visible). Handler blocks are exempt: their live-in is
exactly the comm-var pattern the pinning supports.

#### Why not just read live-in at the root? ####

Because the backward fixpoint's return edges are context-insensitive. Consider a helper called
both before initialisation and inside the main loop:

```
    .entry
        JSR helper       ; call site A - nothing initialised yet
        ...init...
    .loop
        JSR helper       ; call site B - loop state live all around it
        JMP loop
```

The helper's one `RTS` sees the live-after of BOTH call sites, so everything live around the loop
smears through the shared return into call site A - and every loop-carried variable looks live at
the root along a path that cannot execute.

#### The forward may-write walk ####

So `liveness_read_before_write` (`src/liveness.c`) walks *forward* from the root instead, tracking
the bytes at least one route so far MAY have written (union over predecessors, seeded empty). A
read the covered set misses is a genuine input, and warns.

A call applies its callees' summaries in place: the callee's inputs count only where the caller's
coverage has not already supplied them, and the callee's own may-writes extend the coverage. That
is per-call-site precise in exactly the spot the return edges were not. It runs in two phases to
stay sound: the may-write summaries grow to their fixpoint first; then, with coverage frozen, the
input summaries grow to theirs.

#### Why "may", not "must" ####

A definite-assignment gate ("warn unless written on EVERY path") flagged correct programs whose
guarding correlations no analysis of this IR can see. The classic:

```
        JSR setup        ; writes c0 - except when it sets strips = 0
        LDA strips
        BEQ done         ; ...and every reader checks strips first
        LDA c0           ; so this read happens only when c0 WAS written
```

On the CFG there is a path reaching the read with no write, but the flag makes it unexecutable. A
must-write gate drowned the true inputs in false alarms like this; may-write stays quiet on them.

What "may" knowingly gives up: purely loop-carried state (read at a loop's top, written only later
inside it) is silenced by its own back edge, and a forgotten init on one path is silenced by an
init on another.

#### Coverage details ####

- An indexed or unknown-offset store covers its WHOLE variable: it proves no byte, but may have
  written any - the exact inverse of the allocator's kill rule.
- A `ZA_DISCARD` covers nothing: it declares the old value dead and supplies no new one.
- Unknown and external call arms contribute nothing - a warning must not demand annotations, and
  computed flow with variables in play is refused anyway.

## Colouring ##

With the interference graph built, `zp_color` (`src/zpalloc.c`) assigns addresses by
**first-fit-decreasing**:

- widths are processed widest first - a `ZA_AUTO 8` table is far more constrained than a lone byte, so it
  places while the pool is open;
- each variable takes the lowest reserved base whose whole span `[base, base+width)` is free of every
  interfering neighbour's span (`span_reserved` / `spans_overlap` - width-generic, so a table packs
  exactly like a pointer, just wider);
- an `unused` variable (touched by no instruction) is skipped entirely: warned once per declaration
  site, given no address, and its binding *removed* at the rewrite - as if never declared;
- a *used* variable with no home is a spill: `zeropage_full`, naming the variable.

The graphs are tiny (dozens of nodes), so the quadratic scan is irrelevant and first-fit is close enough
to optimal in practice.

## The output pass ##

With a spill-free colouring, the finalizer rewrites each variable's symbol from the typed placeholder to
its real address (an *unused* variable's binding is removed instead), and then `run_passes` runs one last
re-emission - the **output pass** (`parse_flags.output`) - whose sections ARE the result. There is no
operand patching: a settling pass's operand bytes hold only intra-variable offsets, meaningless as
output, and simply get thrown away with the rest of that pass's sections.

Why re-emission cannot move anything: a ZA_AUTO address is a typed value, and every context is
statically one of two kinds. The layout-affecting kind (conditions, counts, `org`) *refused* it
during assembly, so no branch flips and no size changes. The accepting kind (operands, immediates,
`EQU*` elements) emits the same number of bytes whatever the address - a base plus offset cannot
leave the zero page, and data widths are fixed per element. So the output pass reproduces the
converged layout exactly, with the true values in every byte.

The output pass is also where `-v` builds its listing text (`parse_flags.listing` rides on it) and
where `PRINT` speaks - which is how `PRINT var` shows the allocated address, and why address tables
(`EQUW var`), immediates (`LDA #var`) and derived symbols (`x = var + 1`) all come out real.

## The certainty contract ##

We only emit a program we can prove; everything else becomes a diagnostic pointing at the fix.

The refusals (all fatal):

| Code | Cause |
|------|-------|
| `za_auto_narrow_pointer` / `za_auto_out_of_bounds` | an access window past the declared width |
| `za_indexedby_out_of_range` | a declared index set reaching past the variable (the `ZA_INDEXEDBY` bounds check) |
| `za_auto_bad_width` | `ZA_AUTO n` outside 1-256 (at parse) |
| `za_auto_address` | a ZA_AUTO address in a context needing a number now: a condition, a count, an `org`, an annotation operand (the typed placeholder refused at the use site) |
| `za_auto_computed_flow` | an `unknown_succ` block with variables in play (the computed-flow refusal) |
| `za_auto_across_call` | live across a call whose footprint cannot be bounded (the across-call check) |
| `za_auto_recursion` | a fresh per-level value held across a recursive call (the recursion check) |
| `zeropage_full` | a spill: more simultaneous liveness than reserved bytes |

(`za_discard_needs_var` - an operand that is not a whole `ZA_AUTO` variable - and
`za_indexedby_needs_indexed` - an annotation with no indexed access before it - are recoverable
semantic errors at the statement, like the other operand mistakes.)

The warnings: `za_auto_unused`, `za_auto_unreachable`, `za_returnto_no_code` and
`za_auto_indexed_access` (default level - silence an indexed access by declaring its set with
`ZA_INDEXEDBY`) and `za_pool_store` (opt-in; the CLI's `--warn 2` shows it).

And the trust points - the deliberate holes in the proof, each an explicit contract with the user:

- **Annotations are believed.** A wrong `ZA_UNREACHABLE`, `ZA_CANCALL`, `ZA_CANJUMP`, `ZA_RETURN`
  or `ZA_RETURNTO` defeats the analysis; a *missing* one is caught wherever possible (the
  computed-flow and across-call checks) - except an unmarked RTS-dispatch, which is
  indistinguishable from a real return.
- **Markers are believed too.** A wrong `ZA_DISCARD` hands the variable's bytes away while the old
  value is still wanted; a missing one merely wastes bytes, never correctness. A wrong `ZA_WIPE` is
  both mistakes at once (every value handed away, every variable claimed initialised); a missing
  one costs a spurious warning or two.
- **A constant destination off the assembled stream is assumed external.** True for real OS calls, and
  the user's responsibility for a self-modified placeholder or a bare cross-bank number (name the label
  and it resolves properly).
- **The run-time value of an index register is the user's.** As is anything invisible in the stream: a
  self-modified operand, a *computed* pointer aimed into the pool. A store through a *fixed* address or
  base is visible, and the opt-in `za_pool_store` warning audits exactly those. A `ZA_INDEXEDBY`
  shrinks the index promise to "the register stays in the declared set" - the geometry is then checked,
  but the set itself is believed like any annotation.

## Known gaps ##

Deliberate limitations, all soundness-safe or documented trust points:

- **Annotation operands resolve in their own section.** Ordinary cross-section transfers resolve by
  label, but a `ZA_CANCALL`/`ZA_CANJUMP`/`ZA_RETURNTO` *operand* is still a bare number resolved in
  the annotating instruction's section, so it cannot yet name a target in a different bank.
  (`ZA_ENTRY`/`ZA_INTERRUPT` take no operand, so they are intrinsically in the right section.)
- **Annotation sites are bare addresses too.** Two banks sharing an address would cross-talk on
  their annotations - and since `ZA_UNREACHABLE` can sever a call's fall-through, that cross-talk
  could *remove* an edge (the unsound direction) rather than merely add one. Keep annotations and
  their sites in one bank until sites carry a section.
- **The reserved set is global.** One physical zero page, one pool.
- **Fall-through never crosses a section boundary.** A transfer's fall-through resolves in its own
  section. Runtime flow that walks off the end of one section into the code textually after it is
  an expressible shape now (a nested section with no `org` continues its parent's addresses), but
  it is an edge the CFG does not wire - and a missed edge can hide a callee's uses, the unsound
  direction. End a section's straight-line flow with a real transfer (`JMP`/`RTS`), or treat a
  deliberate fall-through across `SECTION`/`ENDSECTION` as a trust point for now.
- **Re-entrant interruption is outside the model.** A handler preempted by *itself* (or an unmarked
  RTS-dispatch inside a handler, invisible as ever) is a trust point, like recursion and `ZA_CANCALL`.

## Map of the code ##

| Piece | Where | What |
|-------|-------|------|
| IR + registries | `src/zeropage.{h,c}` | reserved set, `zp_var`/`zp_insn` (incl. the `zp_marker` size-0 records: ZA_DISCARD, ZA_WIPE, labels)/`zp_cflow`/`zp_label`/`zp_entry`, vreg resolution |
| recording | `src/opcodes.c` | `record_insn`: each instruction's touch, flow and target |
| basic blocks | `src/cfg.{h,c}` | `cfg_build`, target resolution, `cfg_call_targets`, the external rule |
| liveness | `src/liveness.{h,c}` | backward byte-level fixpoint, must-write, return edges, interference, classes; forward read-before-write walk for the ZA_ENTRY input warning |
| footprints | `src/footprint.{h,c}` | transitive Touch(R) per call site, recursion / unknown-call detection |
| colouring | `src/zpalloc.{h,c}` | first-fit-decreasing over the interference graph |
| the driver | `src/assemble.c` | `zeropage_finalize`: checks, roots + reachability, the call checks, interrupt pinning, colour, symbol rewrite; `run_passes` runs the output pass |
