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
standing in for the register file. If that phrase means nothing to you, good - the next section builds it
from scratch.

## Live ranges, interference, colouring ##

Three ideas, each small.

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

Three variables, two bytes, provably safe. That is the entire trick. Everything else in this document is
about computing the live ranges *honestly* - because a live range that comes out too short lets the
colourer overlap two values that both matter, which is a silent runtime corruption. The governing rule
throughout: when in doubt, a range gets **longer** (costing at worst a byte), never shorter.

## Why an assembler makes this interesting ##

A compiler allocates registers with the source structure in its hands. We have a raw instruction stream,
which changes the game in a few ways:

- **There is no structure to lean on.** Braces are *naming*, not lifetime: control happily branches out
  of a `{ }` block or into one, and one block can hold three routines. We must recover the real control
  flow from the branches themselves.
- **Layout settles over many passes.** A forward reference can change an instruction's size, shifting
  every later address - so we cannot allocate mid-assembly. The trick: `ZA_AUTO` binds its name to a
  **typed placeholder value** (`value_type_za_auto` in `src/value.h`) carrying the variable's identity
  and a byte offset, never a number. Every context that would let the layout depend on an address - a
  condition, a count, a section `org` - *refuses* the type with an eager error; the contexts that accept
  it (instruction operands, immediates, data elements) are width-stable, `LDA var` being a two-byte
  zero-page instruction from the very first pass. So the layout provably never depends on the
  allocation, the analysis runs **once, after convergence** (`zeropage_finalize` in `src/assemble.c`),
  and one final re-emission - the [output pass](#the-output-pass) - produces the true bytes.
- **Subroutines, computed jumps, paged banks and recursion** all threaten to hide a read from us - and a
  hidden read is a too-short range. Each gets its own machinery below.

## The pipeline ##

Everything runs inside `zeropage_finalize` (`src/assemble.c`), once, after the final pass:

```
  during assembly (every pass)          zeropage_finalize (once, post-convergence)
  ----------------------------          ------------------------------------------
  ZA_POOL -> reserved-byte set        1. resolve vregs   (operand identity -> variable id)
  ZA_AUTO    -> typed placeholder        2. bounds + indexed-access checks
  final pass -> record the IR           3. build the CFG   (basic blocks + edges)
                                        4. liveness        (ranges -> interference graph)
                                        5. call analyses   (footprints + the guards)
                                        6. colour          (first-fit-decreasing)
                                        7. rewrite symbols (or refuse)
                                        8. the OUTPUT pass re-emits with the real addresses
```

If any guard refuses, we record diagnostics, rewrite nothing, and the assemble fails. Only a fully
analysable, colourable program gets real addresses.

## Recording: what the analyses see ##

Only the final pass records anything - earlier passes exist to let the layout settle. The IR
(`src/zeropage.c`, filled by `record_insn` in `src/opcodes.c`) is four flat arrays:

- `zp_var` - one per declaration: name, owning scope, width (1-256), and its **def cursor** (the source
  position of the `ZA_AUTO` statement).
- `zp_insn` - one per *instruction*: pc, size, control-flow class (`zp_flow`), which variable it touches
  and how (`rw` read/write flags, constant offset, indexed/indirect flags), and its transfer target.
  A `BITZP`/`BITABS` skip marker is a real 1-byte record here too (`zp_flow_skip`, with `skip_bytes`
  carrying how much the BIT swallows) - plain `EQUB` data records nothing, which is exactly why the raw
  spelling of the trick cannot be modelled.
- `zp_cflow` - the annotations: `ZA_UNREACHABLE`, `ZA_CANCALL`, `ZA_CANJUMP`, `ZA_RETURN`, `ZA_RETURNTO`.
- `zp_label` - each label's identity mapped to its physical `(section, pc)`.

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
  One instruction plays both roles at once: a `JMP` through a ZA_AUTO vector names the variable as its
  target *and* reads its bytes, so it is recorded as a pointer read too - without that, liveness would
  let another variable take the vector's bytes between its last store and the jump.

## Easy checks first: bounds and indexed access ##

Before any graph exists, some things are decidable from single instructions (the first loop in
`zeropage_finalize`):

- **`var + n` must stay inside the variable.** During assembly the symbol is the placeholder 0, so an
  operand's evaluated value *is* its offset within the variable. A window past the declared width lands
  on a byte reserved for someone else: **refused**. `(var),Y` on a one-byte variable gets the pointed
  `za_auto_narrow_pointer` ("declare it ZA_AUTO2"); everything else gets `za_auto_out_of_bounds` - including
  the constant *base* of an indexed access (`table+4,X` on a 4-wide table is off the end before X has a
  say).
- **Indexed access itself is a warning, not a refusal** (`za_auto_indexed_access`, opt-in severity). The
  whole variable is reserved and liveness treats an indexed touch as using all of it, so an in-width
  index is sound - and the run-time index is the one thing no static check can see. The same trust as an
  annotation.

## The control-flow graph ##

To trace lifetimes we must know where control can go. The unit for this is the **basic block**: a
maximal straight-line run of instructions - one way in (the top), one way out (the bottom). Within a
block, execution is a simple sequence; all the interesting control flow lives on the **edges** between
blocks.

`cfg_build` (`src/cfg.c`) recovers blocks from the stream in three passes:

1. **Find the leaders** - every instruction where a block must start: the first instruction, every
   branch/jump/call target, the instruction after any branch, jump or return, every section boundary,
   and every annotation-declared target (`ZA_CANCALL`/`ZA_CANJUMP`/`ZA_RETURNTO`).
2. **Cut the stream** at the leaders - and after every branch, jump or return, so a terminator is
   always the last instruction of its block. The two rules coincide except across a data gap: inline
   data (`EQUS` after a `JSR`) records nothing, so the after-address leader from pass 1 can mark a pc
   no instruction sits on, and only the terminator cut keeps the `JMP`'s edges real. A call cuts too
   when an annotation reroutes its continuation (`ZA_RETURNTO`) or severs it (`ZA_UNREACHABLE` just
   past it), so pass 3 can express either as edge omission.
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

The details that make the CFG honest:

- **A block's identity is `(section, pc)`, not `pc` alone.** Two paged banks may both sit at `&8000`;
  the pair keeps their flow apart. Within one section, pc is strictly monotonic by construction
  (asserted in debug builds), so identity is unambiguous.
- **A `JSR` is an in-block instruction, not an edge.** Callees are reached through the [call
  analyses](#calls); an `RTS` ends a block with no successor. "Routines" are simply whatever is
  reachable from an entry without crossing a call or return.
- **Targets resolve by label** (`cfg_target_block`, via the `zp_label` markers) - which is how a
  transfer crosses sections and still finds the right bank. A bare number resolves within its own
  section only. An indirect `JMP`'s operand names its vector *cell*, never its destination
  (`zp_insn.target_via`), so the cell's address is never mistaken for an edge.
- **A constant destination matching nothing we assembled is a transfer out of the program**
  (`cfg_target_is_external`): `JSR &FFEE`, a tail `JMP &FFEE`, `JMP (&FFFC)` through an OS vector.
  External code cannot touch the pool - `ZA_POOL` names precisely the bytes nothing else uses - so an
  external exit is as clean as an `RTS`, and needs no annotation.
- **Flow we genuinely cannot place taints its block** (`unknown_succ`): an unannotated indirect `JMP`
  through our own vector, an indexed dispatch `JMP (table,X)`. A vector that is a ZA_AUTO *variable* is
  ours too (`zp_insn.target_is_zpvar`, resolved alongside the vregs), so `JMP (vec)` on a `ZA_AUTO2` is
  computed flow wanting `ZA_CANJUMP`, never mistaken for a constant OS cell. The taint later forces
  "everything live" out of that block - and Guard 1 refuses the program if any variables are in play at
  all.
- **A call/normal fall-through skips a data gap.** The fall-through pc is `pc + size`; when inline
  data displaced the next instruction there is no block there, and the edge goes to the next recorded
  same-section instruction instead - the adjacency the in-block walk already assumes, expressed as an
  edge. This is what makes `JSR pstring : EQUS "text", 0 : ...` sound with no annotation: the
  data-consuming callee resumes exactly there. Nothing found at all is the end of the stream, a clean
  dead end. (A *branch's* not-taken edge stays pure pc arithmetic - a not-taken branch into data is a
  broken program, not a continuation.)
- **A skip marker (`BITZP`/`BITABS`, `zp_flow_skip`) hops its swallowed bytes.** Pass 1 marks two
  leaders: the instruction at `pc+1` (the swallowed one - it starts its own block, entered only by an
  explicit branch, since the marker wires **no** edge to it) and the resume point
  `pc + 1 + skip_bytes` (pure arithmetic: a BIT always fetches its full operand). Pass 2 cuts after
  the marker like any terminator; pass 3 wires exactly one successor to the resume - falling back to
  the data-gap skip, constrained to `pc >= resume` so it can never land back on the swallowed block.
  A recorded instruction *straddling* the resume point (a 3-byte instruction under a `BITABS`) means
  the streams interleave mid-instruction - inexpressible, so finalize refuses it
  (`error_type_skip_spans_instruction`). Everything downstream needs no special cases: the swallowed
  store/read simply lives in a branch-only block, so must-write and liveness are correct by structure.
- **Annotations adjust the graph**: `ZA_UNREACHABLE` prunes a dead fall-through edge - a branch's
  not-taken arm, or the continuation of a never-returning `JSR`; `ZA_CANJUMP` wires a computed jump's or
  self-modified branch's declared targets as real successors **in place of** any literal edge (external
  arms contributing nothing - the programmer's word beats a placeholder operand, as it already did for
  calls); `ZA_RETURN` declares a jump/branch a return to the routine's own caller (no edge - the return
  machinery in liveness supplies the semantics); `ZA_RETURNTO` reroutes a call's continuation to its
  declared resumption points (an arm with no block is off the stream: no edge, and finalize warns). An
  `RTS` carrying a `ZA_CANJUMP` is the dispatch trick - push a target address, "return" into it - and is
  wired exactly like an annotated jump. An *unannotated* dispatch is indistinguishable from a real
  return, so it remains a trusted precondition, never a taint.

## Liveness: walking backward ##

Now the central question: at each point, which variables are live? The natural direction is
**backward** - liveness is about the *future* ("will this value be read again?"), and walking from the
end toward the start means that by the time we reach an instruction, we already know what the code after
it needs.

Walk one block backward with a set of live bytes in hand:

- at a **read**, the value clearly matters: *add* the read bytes to the set;
- at a **write**, the old value is gone: *remove* the written bytes;
- at a **call**, apply the call's transfer (next section).

Blocks chain through the CFG: a block's *live-out* is the union of its successors' *live-in* (any
successor might read it), and a tainted (`unknown_succ`) block's live-out is everything. Loops make the
sets provisional, so `liveness_analyze` (`src/liveness.c`) just iterates the whole thing until nothing
changes - a *fixpoint*. The sets only ever grow, so it terminates.

Two refinements:

- **The dataflow is per BYTE, not per variable.** A `ZA_AUTO2` pointer occupies two byte-ids; `STA ptr`
  rewrites only the low one. So a pointer whose MSB was written once and whose LSB is refreshed before
  each use stays live - nothing gets allocated over the MSB - while a `STA ptr : STA ptr+1` pair
  accumulates into a genuine full kill. (Treating any store as a whole-variable kill was a real bug: it
  severed a pointer's MSB from its dereferences and let another variable land on it.) `insn_window` maps
  each touch to its read/write byte windows; the public results are projected back to variable level,
  since whole variables are what we place.
- **Interference is collected during one more backward sweep**: at every write, the written variable
  gains an edge to the owner of every other live byte. That single rule generates the entire graph -
  "the output can reuse the dead input's byte" and all its friends fall out with no special cases.

Finally, each variable is classified `input` / `output` / `temp` / `unused`. Only `unused` matters in
production (the unused-variable warning and the colourer's skip); the `input`/`output` split, and the
synthetic rule that made a routine's entry inputs pairwise interfere, are anchored on an `entry_block`
parameter that the driver no longer supplies - there is no presumed program entry any more (see
[Roots, reachability and interrupts](#roots-reachability-and-interrupts)). The mechanism survives for
the analysis's own tests, and a value read at an entry before anything writes it simply has no defined
value and no protection.

One record in the stream is not an instruction at all: a `ZA_DISCARD` marker (`zp_insn.var_kill`, size 0,
sharing its pc with the instruction after it). Its window "writes" the variable's whole width while
reading nothing, so the backward walk treats it as a full kill - which is the entire point: an array
rebuilt only through indexed stores has no provable write anywhere, and without the marker its reads
keep the old value live clear back to the routine entry (and, through a `JMP` entry, around the caller's
loop). Three carve-outs keep the promise honest: the interference sweep clears the bytes *without
pinning* (nothing is stored, so another variable may own the bytes at that instant), the footprint walk
skips it entirely (a callee that merely discards touches nothing), and the must-write analysis counts it
in full (the caller's pre-call value is equally dead whichever path runs, so a discard-then-rebuild
callee kills at its call sites like a provable rewrite). The cfg tolerates the shared pc by cutting
blocks only where the address *changes* to a leader, marker first in its block.

## Calls ##

A `JSR` is where naive liveness falls apart, because the callee's code is not on the caller's CFG path.
Four rules close the gap - each one exists because a real program broke without it.

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
Any variable live *across* a call interferes with the whole footprint - that is Guard 2 in
`zeropage_finalize`. In this real test (`za_auto_interprocedural_allocation`):

```
    LDA #1 : STA keep
    JSR sub             ; keep is live across this call...
    LDA keep
    RTS
.sub { ZA_AUTO1 loc : STA loc : LDA loc : RTS }
```

`keep` gets `&70` and `loc` is forced to `&71` - without the rule both took `&70` and the call corrupted
`keep`. A variable already dead at the call is free to reuse the callee's bytes. `ZA_CANCALL` supplies the
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

Without this rule `t` landed on `wid`'s byte and overwrote the argument (a silent miscompile we shipped
briefly and caught by byte-comparing real output). Since a callee's own live-in includes whatever *its*
calls inject, the rule chains through nested calls for free.

**3. A call delivers its results.** The dual rule: a forward *must-write* analysis (also in
`liveness_analyze`) computes, per routine, the bytes written on **every** path to a returning exit. A
call *kills* the intersection over its arms (`call_kill_bytes`) - the caller's read after the call can
only see the callee's value, so the pre-call byte is dead and a result's range *starts at its call*.
This is what lets the usage guide's two-stage pipeline run four interface variables in one byte. The
proof is genuine: make the store conditional -

```
.offset { ZA_AUTO1 xin, res : LDA xin : CLC : ADC #7 : BEQ @+ : STA res : .@ RTS }
```

- and `res` is no longer must-written, so it is preserved across the whole journey on its own byte
(test `za_auto_conditional_result_is_preserved`). A "returning exit" (`block_returns`) is an RTS/RTI, a
transfer out of the program - the OS routine's own RTS returns to our caller, the tail-call idiom - *or*
a jump/branch wearing a `ZA_RETURN` (the inline-data trick's computed exit, declared to hand straight
back); but *not* an RTS wearing a `ZA_CANJUMP`, whose control continues at its declared targets.

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

**Recursion** gets one extra check: a value the cycle writes *afresh* at each level and reads back after
the recursive call would need a byte per level, which one static address cannot give - refused
(`za_auto_recursion`, keyed on the footprint's write-only `killed` set against the live set *before* the
must-write reduction, so the kill cannot hide the pattern). A counter merely `DEC`ed through the
recursion is a single running value and rides one byte happily.

## Roots, reachability and interrupts ##

Everything so far analyses the stream as it lies; nothing asks *where control can enter it*. That
question has two customers, both fed by the `ZA_ENTRY` / `ZA_INTERRUPT` markers (`zp_entry` records: a
section, the marked pc, an interrupt flag - `cfg_build` marks each a block leader so a mid-run entry
starts its own block, and a marker that resolves to no block is refused as marking no instruction).

**Reachability.** The root set is every declared marker's block; with no `ZA_ENTRY` anywhere the
defaults apply instead - each section's first recorded block (blocks come in stream order with
per-section pc monotonic, so first sighting of a section index is its earliest code; a `ZA_INTERRUPT`
alone keeps the defaults, since a handler says nothing about where the mainline starts). From the
roots, `reach_from` walks successor edges plus each call's resolved arms - the same edge set control
can actually take, `ZA_CANJUMP`/`ZA_CANCALL` arms included; unknown and external arms contribute nothing. A
block that touches a `ZA_AUTO` variable and is not reached warns (`za_auto_unreachable`,
default-visible): most likely an unmarked handler - the silently-unsound island shape the feature
exists to catch - or dead code. One warning per region: heads are unreachable blocks with no
unreachable predecessor, each head's closure is claimed once, and a headless cycle is mopped up
separately. Warnings never refuse; unreachable code is still fully analysed and coloured.

**Interrupt pinning (Guard 3).** A `ZA_INTERRUPT` handler preempts between *any* two instructions, so
no transaction discipline exists around it and two interference rules are injected (plain edge writes
into `lv.interfere`, like Guard 2's): the handler's live-in - state the mainline feeds it - is pinned
against *everything*, its own temps included (the mainline may rewrite a communication variable at any
moment relative to the handler, so no instant of "dead" is reusable); and every variable in the
handler's transitive footprint (`footprint_compute` at its entry) interferes with every variable
outside it. Handler-internal reuse is untouched - ordinary liveness governs among its own temps - and
two handlers separate each other for free (each is outside the other's footprint). A footprint the
walk cannot bound (an unannotated computed call in the extent) is refused with the usual
`za_auto_across_call`, at the marker. Edge injection is order-independent of Guard 2: `lv.interfere` is
write-only until the colourer reads it.

`ZA_ENTRY` deliberately carries no interface semantics. An external API's inputs and outputs belong at
fixed addresses outside the pool (an auto-allocated address moves between builds); an auto-allocated
entry is protected exactly as far as a strict poke-`CALL`-peek transaction requires, which ordinary
liveness already provides - so a sync entry is *only* a root.

**The input warning.** A sync root that reads a variable *nothing in the program could yet have
written* is expecting its caller to have set the value - impossible at an auto-allocated address - so
each `ZA_ENTRY` block is checked and every such variable warned (`za_entry_input`, default-visible;
handler blocks are exempt, their live-in being Guard 3's supported comm-var pattern). The test is NOT
`live_in` at the root: the backward fixpoint's return edges are context-insensitive, so a helper
called both from the entry's pre-init stretch and from inside the main loop smears the loop call
site's live-after through the shared `RTS` into the entry's unrelated call site, and every
loop-carried variable then looks live-in at the root along a path that cannot execute. Instead
`liveness_read_before_write` (`src/liveness.c`) runs a forward **may-write** flow from the root
(union meet over predecessors, seeded empty - a byte is covered once ANY route to this point contains
a write of it) and harvests each read the covered set misses; a call applies its callees' summaries
in place (their input bytes count only where the caller's set does not already cover them, their
may-writes - the union over their arms - extend it), which is per-call-site precise where the return
edges are not. Two phases: the may-write summaries grow to their fixpoint first, then, coverage
frozen, the input summaries grow to theirs. The gate is deliberately *may*, not the allocator's
must-write: a definite-assignment gate flagged correct programs whose guarding correlations no
analysis of this IR can see - the flag-guarded init (writes skipped exactly when a "nothing visible"
flag is set, reads guarded by the same flag) and the value-correlated dispatch (written under `X = 0`,
read only in the handler dispatched when `X = 0`) - and drowned the true inputs. What "may" gives up,
knowingly: purely loop-carried state (read at a loop's top, written only later inside it) is silenced
by its own back edge, and a forgotten init on one path is silenced by an init on another. Coverage
details: an indexed / unknown-offset store counts for its whole variable here (it proves no byte, but
may have written any - the inverse of the allocator's kill rule), while a `ZA_DISCARD` counts for
nothing (it declares the old value dead; it supplies no new one). Unknown and external arms
contribute nothing - a warning must not demand annotations, and Guard 1 refuses computed flow anyway.

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

Why re-emission cannot move anything: a ZA_AUTO address is a typed value, and every context is statically
one of two kinds. The layout-affecting kind (conditions, counts, `org`) *refused* it during assembly, so
no branch flips and no size changes; the accepting kind (operands, immediates, `EQU*` elements) is
width-stable - a base+offset cannot leave the zero page, and data widths are fixed per element. So the
output pass reproduces the converged layout exactly, with the true values in every byte. It is also
where `-v` builds its listing text (`parse_flags.listing` rides on it) and where `PRINT` speaks - which
is how `PRINT var` shows the allocated address, and why address tables (`EQUW var`), immediates
(`LDA #var`) and derived symbols (`x = var + 1`) all come out real.

## The certainty contract ##

We only emit a program we can prove; everything else becomes a diagnostic pointing at the fix.

The refusals (all fatal):

| Code | Cause |
|------|-------|
| `za_auto_narrow_pointer` / `za_auto_out_of_bounds` | an access window past the declared width |
| `za_auto_bad_width` | `ZA_AUTO n` outside 1-256 (at parse) |
| `za_auto_address` | a ZA_AUTO address in a context needing a number now: a condition, a count, an `org`, an annotation operand (the typed placeholder refused at the use site) |
| `za_auto_computed_flow` | an `unknown_succ` block with variables in play (Guard 1) |
| `za_auto_across_call` | live across a call whose footprint cannot be bounded (Guard 2) |
| `za_auto_recursion` | a fresh per-level value held across a recursive call (Guard 2) |
| `zeropage_full` | a spill: more simultaneous liveness than reserved bytes |

(`za_discard_needs_var` - an operand that is not a whole `ZA_AUTO` variable - is a recoverable semantic
error at the statement, like the other operand mistakes.)

The warnings: `za_auto_unused`, `za_auto_unreachable` and `za_returnto_no_code` (default level) and
`za_auto_indexed_access` (opt-in).

And the trust points - the deliberate holes in the proof, each an explicit contract with the user:

- **Annotations are believed.** A wrong `ZA_UNREACHABLE`, `ZA_CANCALL`, `ZA_CANJUMP`, `ZA_RETURN` or
  `ZA_RETURNTO` defeats the analysis; a
  *missing* one is caught wherever possible (Guards 1 and 2) - except an unmarked RTS-dispatch, which is
  indistinguishable from a real return. A wrong `ZA_DISCARD` is the same class: it hands the variable's
  bytes away while the old value is still wanted; a missing one merely wastes bytes, never correctness.
- **A constant destination off the assembled stream is assumed external.** True for real OS calls, and
  the user's responsibility for a self-modified placeholder or a bare cross-bank number (name the label
  and it resolves properly).
- **The run-time value of an index register is the user's.** As is anything invisible in the stream: a
  self-modified operand, a stray pointer aimed into the pool.

## Known gaps ##

Deliberate limitations, all soundness-safe or documented trust points:

- **Annotation operands resolve in their own section.** Ordinary cross-section transfers resolve by
  label; a `ZA_CANCALL`/`ZA_CANJUMP`/`ZA_RETURNTO` *operand* is still a bare number resolved in the
  annotating instruction's section, so it cannot yet name a target in a different bank.
  (`ZA_ENTRY`/`ZA_INTERRUPT` take no operand, so they are intrinsically in the right section.) In the
  same vein a cflow *site* is a bare pc: two banks sharing an address would cross-talk on annotations,
  and with `ZA_UNREACHABLE` now able to sever a call's fall-through that cross-talk could remove an
  edge (the unsound direction) rather than merely add one - keep annotations and their sites in one
  bank until sites carry a section.
- **The reserved set is global.** One physical zero page, one pool.
- **Fall-through never crosses a section boundary.** A transfer's fall-through resolves in its own
  section, so runtime flow that walks off the end of one section into the code textually after it -
  now an expressible shape, since a nested section with no `org` transparently continues its parent's
  addresses - is an edge the CFG does not wire, and a missed edge can hide a callee's uses (the
  unsound direction). End a section's straight-line flow with a real transfer (`JMP`/`RTS`), or treat
  a deliberate fall-through across `SECTION`/`ENDSECTION` as a trust point for now.
- **Re-entrant interruption is outside the model.** A handler preempted by *itself* (or an unmarked
  RTS-dispatch inside a handler, invisible as ever) is a trust point, like recursion and `ZA_CANCALL`.

## Map of the code ##

| Piece | Where | What |
|-------|-------|------|
| IR + registries | `src/zeropage.{h,c}` | reserved set, `zp_var`/`zp_insn` (incl. `var_kill` ZA_DISCARD markers)/`zp_cflow`/`zp_label`/`zp_entry`, vreg resolution |
| recording | `src/opcodes.c` | `record_insn`: each instruction's touch, flow and target |
| basic blocks | `src/cfg.{h,c}` | `cfg_build`, target resolution, `cfg_call_targets`, the external rule |
| liveness | `src/liveness.{h,c}` | backward byte-level fixpoint, must-write, return edges, interference, classes; forward read-before-write walk for the ZA_ENTRY input warning |
| footprints | `src/footprint.{h,c}` | transitive Touch(R) per call site, recursion / unknown-call detection |
| colouring | `src/zpalloc.{h,c}` | first-fit-decreasing over the interference graph |
| the driver | `src/assemble.c` | `zeropage_finalize`: checks, roots + reachability, guards, interrupt pinning, colour, symbol rewrite; `run_passes` runs the output pass |
