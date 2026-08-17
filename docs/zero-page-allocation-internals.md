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
- [Colouring](#colouring)
- [Patch and rewrite](#patch-and-rewrite)
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

Here is a real program (it is test `zpauto_allocates_and_reuses` in `assemble.c`), with each variable's
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
  every later address - so we cannot allocate mid-assembly. The trick (`src/zeropage.c`): `ZPAUTO` binds
  its name to a *placeholder* address 0, typed zero-page, so `LDA var` is a two-byte zero-page
  instruction from the very first pass. The layout never depends on the allocation, which means the whole
  analysis can run **once, after convergence** (`zeropage_finalize` in `src/assemble.c`) and patch
  addresses into place without moving a single byte.
- **Subroutines, computed jumps, paged banks and recursion** all threaten to hide a read from us - and a
  hidden read is a too-short range. Each gets its own machinery below.

## The pipeline ##

Everything runs inside `zeropage_finalize` (`src/assemble.c`), once, after the final pass:

```
  during assembly (every pass)          zeropage_finalize (once, post-convergence)
  ----------------------------          ------------------------------------------
  ZPRESERVE -> reserved-byte set        1. resolve vregs   (operand identity -> variable id)
  ZPAUTO    -> placeholder symbol       2. bounds + indexed-access checks
  final pass -> record the IR           3. build the CFG   (basic blocks + edges)
                                        4. liveness        (ranges -> interference graph)
                                        5. call analyses   (footprints + the guards)
                                        6. colour          (first-fit-decreasing)
                                        7. patch operands + rewrite symbols  (or refuse)
```

If any guard refuses, we record diagnostics, patch nothing, and the assemble fails. Only a fully
analysable, colourable program gets real addresses.

## Recording: what the analyses see ##

Only the final pass records anything - earlier passes exist to let the layout settle. The IR
(`src/zeropage.c`, filled by `record_insn` in `src/opcodes.c`) is four flat arrays:

- `zp_var` - one per declaration: name, owning scope, width (1-256), and its **def cursor** (the source
  position of the `ZPAUTO` statement).
- `zp_insn` - one per *instruction*: pc, size, control-flow class (`zp_flow`), which variable it touches
  and how (`rw` read/write flags, constant offset, indexed/indirect flags), where its operand byte lives
  for later patching, and its transfer target.
- `zp_cflow` - the annotations: `UNREACHABLE`, `CANCALL`, `CANJUMP`.
- `zp_label` - each label's identity mapped to its physical `(section, pc)`.

Two details worth knowing:

- **A variable's identity is the pair (scope, def cursor)** - not the def cursor alone. A macro or `FOR`
  body shares one `ZPAUTO` across every instantiation, but each instantiation runs in its own child
  scope, so the pair tells the copies apart (each iteration's `loc` is a genuinely distinct variable).
  Operands record the pair; `zeropage_resolve_vregs` maps pairs to dense ids once the registry is
  complete, which is also why a use may precede its declaration.
- **Dotted operands attribute like local ones.** `STA sub.wid` resolves through the scope tree
  (`scopes_resolve_symbol_def`, `src/scopes.c`) to the exact variable instance, so cross-scope interface
  variables are patched and analysed exactly like everything else.

## Easy checks first: bounds and indexed access ##

Before any graph exists, some things are decidable from single instructions (the first loop in
`zeropage_finalize`):

- **`var + n` must stay inside the variable.** During assembly the symbol is the placeholder 0, so an
  operand's evaluated value *is* its offset within the variable. A window past the declared width lands
  on a byte reserved for someone else: **refused**. `(var),Y` on a one-byte variable gets the pointed
  `zpauto_narrow_pointer` ("declare it ZPAUTO2"); everything else gets `zpauto_out_of_bounds` - including
  the constant *base* of an indexed access (`table+4,X` on a 4-wide table is off the end before X has a
  say).
- **Indexed access itself is a warning, not a refusal** (`zpauto_indexed_access`, opt-in severity). The
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
   and every `CANCALL`/`CANJUMP`-declared target.
2. **Cut the stream** at the leaders.
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
  External code cannot touch the pool - `ZPRESERVE` names precisely the bytes nothing else uses - so an
  external exit is as clean as an `RTS`, and needs no annotation.
- **Flow we genuinely cannot place taints its block** (`unknown_succ`): an unannotated indirect `JMP`
  through our own vector, an indexed dispatch `JMP (table,X)`. The taint later forces "everything live"
  out of that block - and Guard 1 refuses the program if any variables are in play at all.
- **Annotations adjust the graph**: `UNREACHABLE` prunes a branch's fall-through edge; `CANJUMP` wires a
  computed jump's declared targets as real successors (external arms contributing nothing). An `RTS`
  carrying a `CANJUMP` is the dispatch trick - push a target address, "return" into it - and is wired
  exactly like an annotated jump. An *unannotated* dispatch is indistinguishable from a real return, so
  it remains a trusted precondition, never a taint.

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

- **The dataflow is per BYTE, not per variable.** A `ZPAUTO2` pointer occupies two byte-ids; `STA ptr`
  rewrites only the low one. So a pointer whose MSB was written once and whose LSB is refreshed before
  each use stays live - nothing gets allocated over the MSB - while a `STA ptr : STA ptr+1` pair
  accumulates into a genuine full kill. (Treating any store as a whole-variable kill was a real bug: it
  severed a pointer's MSB from its dereferences and let another variable land on it.) `insn_window` maps
  each touch to its read/write byte windows; the public results are projected back to variable level,
  since whole variables are what we place.
- **Interference is collected during one more backward sweep**: at every write, the written variable
  gains an edge to the owner of every other live byte. That single rule generates the entire graph -
  "the output can reuse the dead input's byte" and all its friends fall out with no special cases.

Finally, variables live-in at the program entry are its *inputs*; they pairwise interfere (two read-only
inputs coexist at entry even if no instruction sees both at once), and each variable is classified
`input` / `output` / `temp` / `unused` for diagnostics and the unused-variable handling.

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
`zeropage_finalize`. In this real test (`zpauto_interprocedural_allocation`):

```
    LDA #1 : STA keep
    JSR sub             ; keep is live across this call...
    LDA keep
    RTS
.sub { ZPAUTO1 loc : STA loc : LDA loc : RTS }
```

`keep` gets `&70` and `loc` is forced to `&71` - without the rule both took `&70` and the call corrupted
`keep`. A variable already dead at the call is free to reuse the callee's bytes. `CANCALL` supplies the
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
.offset { ZPAUTO1 xin, res : LDA xin : CLC : ADC #7 : BEQ @+ : STA res : .@ RTS }
```

- and `res` is no longer must-written, so it is preserved across the whole journey on its own byte
(test `zpauto_conditional_result_is_preserved`). A "returning exit" (`block_returns`) is an RTS/RTI *or*
a transfer out of the program - the OS routine's own RTS returns to our caller, the tail-call idiom -
but *not* an RTS wearing a `CANJUMP`, whose control continues at its declared targets.

**4. Returns see the caller.** Rule 3 creates a hazard: an escaping result's only reads are in the
caller, so inside its producer it would look dead the moment it is stored - and the producer's *later*
code could take its byte:

```
.sub { ZPAUTO1 res, t : LDA #1 : STA res : LDA #2 : STA t : LDA t : RTS }
                                  ^ res "dead" here?     ^ then t may land on it!
```

So every returning block's live-out also unions the live-*after* set of each call site that reaches its
routine (the `ret_from` map and per-call `after` snapshots, maintained inside the fixpoint). `res` stays
live from its store to the `RTS`, and `t` gets its own byte (test
`zpauto_result_survives_producer_tail`).

**Recursion** gets one extra check: a value the cycle writes *afresh* at each level and reads back after
the recursive call would need a byte per level, which one static address cannot give - refused
(`zpauto_recursion`, keyed on the footprint's write-only `killed` set against the live set *before* the
must-write reduction, so the kill cannot hide the pattern). A counter merely `DEC`ed through the
recursion is a single running value and rides one byte happily.

## Colouring ##

With the interference graph built, `zp_color` (`src/zpalloc.c`) assigns addresses by
**first-fit-decreasing**:

- widths are processed widest first - a `ZPAUTO 8` table is far more constrained than a lone byte, so it
  places while the pool is open;
- each variable takes the lowest reserved base whose whole span `[base, base+width)` is free of every
  interfering neighbour's span (`span_reserved` / `spans_overlap` - width-generic, so a table packs
  exactly like a pointer, just wider);
- an `unused` variable (touched by no instruction) is skipped entirely: warned once per declaration
  site, given no address, and its binding *removed* at the rewrite - as if never declared;
- a *used* variable with no home is a spill: `zeropage_full`, naming the variable.

The graphs are tiny (dozens of nodes), so the quadratic scan is irrelevant and first-fit is close enough
to optimal in practice.

## Patch and rewrite ##

With a spill-free colouring, the finalizer walks the recorded instructions and folds each assignment in:

- **operands**: every reference was emitted against placeholder base 0, so the operand byte currently
  holds only the intra-variable offset (0 for `var`, 1 for `var+1`); `sections_patch_add_u8` adds the
  assigned base in place;
- **symbols**: each variable's binding moves from the placeholder to its real address, so the symbol
  table (and the `-v` listing's `var = &70 [auto]` lines) tell the truth.

No instruction changes size - everything was zero-page-shaped from pass one - so the converged layout is
untouched. Under `-v`, the listing pass then re-emits from the rewritten symbols and reproduces the same
bytes the patches made. (Beware when testing: because the listing re-emits, it shows the *intended*
operand even if a patch were missed - byte-compare the saved output, as `zpauto_dotted_interface_variables`
does.)

## The certainty contract ##

We only patch a program we can prove; everything else becomes a diagnostic pointing at the fix.

The refusals (all fatal):

| Code | Cause |
|------|-------|
| `zpauto_narrow_pointer` / `zpauto_out_of_bounds` | an access window past the declared width |
| `zpauto_bad_width` | `ZPAUTO n` outside 1-256 (at parse) |
| `zpauto_computed_flow` | an `unknown_succ` block with variables in play (Guard 1) |
| `zpauto_across_call` | live across a call whose footprint cannot be bounded (Guard 2) |
| `zpauto_recursion` | a fresh per-level value held across a recursive call (Guard 2) |
| `zeropage_full` | a spill: more simultaneous liveness than reserved bytes |

The warnings: `zpauto_unused` (default level) and `zpauto_indexed_access` (opt-in).

And the trust points - the deliberate holes in the proof, each an explicit contract with the user:

- **Annotations are believed.** A wrong `UNREACHABLE`, `CANCALL` or `CANJUMP` defeats the analysis; a
  *missing* one is caught wherever possible (Guards 1 and 2) - except an unmarked RTS-dispatch, which is
  indistinguishable from a real return.
- **A constant destination off the assembled stream is assumed external.** True for real OS calls, and
  the user's responsibility for a self-modified placeholder or a bare cross-bank number (name the label
  and it resolves properly).
- **The run-time value of an index register is the user's.** As is anything invisible in the stream: a
  self-modified operand, a stray pointer aimed into the pool.

## Known gaps ##

Three deliberate limitations, all soundness-safe:

- **Single-entry classification.** Entry-input interference and the `input` class seed from block 0; a
  bank entered only by cross-section calls gets a slightly coarser classification. Precision only -
  Guard 2's footprint edges keep the colouring correct regardless.
- **Annotation operands resolve in their own section.** Ordinary cross-section transfers resolve by
  label; a `CANCALL`/`CANJUMP` *operand* is still a bare number resolved in the annotating
  instruction's section, so it cannot yet name a target in a different bank.
- **The reserved set is global.** One physical zero page, one pool.

## Map of the code ##

| Piece | Where | What |
|-------|-------|------|
| IR + registries | `src/zeropage.{h,c}` | reserved set, `zp_var`/`zp_insn`/`zp_cflow`/`zp_label`, vreg resolution |
| recording | `src/opcodes.c` | `record_insn`: each instruction's touch, flow and target |
| basic blocks | `src/cfg.{h,c}` | `cfg_build`, target resolution, `cfg_call_targets`, the external rule |
| liveness | `src/liveness.{h,c}` | backward byte-level fixpoint, must-write, return edges, interference, classes |
| footprints | `src/footprint.{h,c}` | transitive Touch(R) per call site, recursion / unknown-call detection |
| colouring | `src/zpalloc.{h,c}` | first-fit-decreasing over the interference graph |
| the driver | `src/assemble.c` | `zeropage_finalize`: checks, guards, colour, patch, rewrite |
