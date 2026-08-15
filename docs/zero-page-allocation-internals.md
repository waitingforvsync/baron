# Zero-page allocation: internals #

This is the technical companion to [`zero-page-allocation.md`](zero-page-allocation.md) (the user-facing
guide). It describes how the allocator actually works: the pipeline, the data structures, the analyses and the
guards. If you just want to *use* `ZPRESERVE` / `ZPAUTO`, read the other document; this one is for anyone
changing the allocator.

## The problem, in one line ##

The user declares named zero-page variables and Baron must assign each a real zero-page byte such that no two
variables that are ever simultaneously live share a byte - and prove it, refusing rather than emitting code it
cannot show is correct. This is **register allocation by graph colouring**, with the 6502 zero page standing in
for the register file: variables are the values, reserved bytes are the registers, and "simultaneously live"
is the interference relation.

The twist that makes it more than a textbook allocator is that Baron is an *assembler*, not a compiler: it
sees a raw instruction stream (no source-level scopes for lifetimes), it runs multiple passes to convergence
before it can allocate at all, and it must cope with subroutine calls, computed jumps, paged banks that share
addresses, and recursion - each of which is a place where a naive analysis would silently under-approximate a
live range and hand back a wrong byte.

## High-level pipeline ##

Everything happens in one post-convergence function, `zeropage_finalize` (`src/assemble.c`), which runs once
after the final assembly pass. The stages, in order:

```
  assembly passes (settling)                 zeropage_finalize (once, post-convergence)
  --------------------------                 -------------------------------------------
  ZPRESERVE  -> reserved-byte set            0. resolve vregs   (def cursor -> vreg id)
  ZPAUTO     -> placeholder symbol           1. Guard 0         (indexed-access warning + var+n bounds)
  final pass -> record IR:                   2. build CFG       (per (section, pc), label-resolved edges)
                zp_var  (declarations)       3. liveness        (backward fixpoint -> interference graph)
                zp_insn (var touches)        4. footprint       (interprocedural Touch(R) per call)
                zp_cflow(annotations)           + Guard 1 (computed flow) + Guard 2 (calls / recursion)
                zp_label(label markers)      5. colour          (first-fit-decreasing over the interference graph)
                                             6. patch operands + rewrite symbols  (or refuse)
```

If any guard refuses, the function records diagnostics, patches nothing, and `run_passes` fails the assemble.
Only a fully analysable, colourable program has its operand bytes and symbols rewritten to real addresses.
(Guard 0's indexed-access *warning* is the exception - it records a diagnostic but does not refuse; the
allocation runs to completion.)

## Stage 0 - recording during assembly ##

The allocator needs a picture of the finished program, but the program's size is still settling during the
early passes (a forward reference can change an instruction's length, which shifts every later address). So the
allocator does nothing during settling except keep the layout *convergent*:

- **`ZPRESERVE a..b`** fills the 256-bit `reserved` set (`zeropage.reserved`) - which bytes the allocator may
  draw from - and enables the feature. It re-runs every pass; the set is cleared at the top of each pass and
  refilled, so after the final pass it holds the settled reservation.
- **`ZPAUTO1 v` / `ZPAUTO2 v` / `ZPAUTO n, v`** binds `v` to a *placeholder* symbol at `zeropage_var_placeholder`
  (0), typed as a zero-page access. The placeholder's only job is to size `LDA v` as a 2-byte zero-page
  instruction. Because it is a fixed constant, the binding never "changes" pass-to-pass, so it does not by
  itself prevent convergence.

Only on the **final pass**, once addresses have settled, does the manager capture the IR the analyses walk
(`src/zeropage.c`):

- `zp_var` - one per declared variable: name, owning scope, width (1, 2, or a `ZPAUTO n` table up to 256), and its **def cursor** (the source
  position of its `ZPAUTO` statement), which is its identity across passes and instantiations.
- `zp_insn` - one per instruction that *touches* a variable (plus enough control-flow metadata on every
  instruction to build the CFG): its `pc`, `size`, control-flow class (`zp_flow`), how it touches the variable
  (`rw`: `vref_read` / `vref_write` / both for an RMW like `INC`), the operand's `(section, offset)` for later
  patching, and the control-transfer target (see below).
- `zp_cflow` - the trusted annotations (`UNREACHABLE` / `CANCALL` / `CANJUMP`).
- `zp_label` - a **label marker** tying a label's identity `(scope, def)` to its physical `(section, pc)`.
  This is what lets a control transfer that named a label resolve to the right block even when two paged banks
  share an address.

A subtlety: a variable can be *used* textually before its `ZPAUTO` declaration, and the registry fills in
source order. So instructions record the operand's `(var_scope, var_def)` pair, and vreg ids are resolved
*after* the pass, once the whole registry exists.

## Stage 0.5 - vreg resolution ##

`zeropage_resolve_vregs` walks the recorded instructions and, for each, looks up `(var_scope, var_def)` in the
variable registry to fill in `vreg` (or leaves `RC_INDEX_NONE` if the operand named something that is not a
ZPAUTO). The `(scope, def)` **pair** is the identity: a def cursor alone collides across the instantiations of
a macro or `FOR` body (they share one def), but each instantiation runs in its own child scope, which tells
them apart. After this, `vreg` indexes the global id space `[0, num_vars)` that every later stage uses.

## Stage 1 - Guard 0: indexed access and bounds ##

The allocator reserves the *whole* variable `[base, base + width)` and liveness marks all of it live on any
access, so an index that stays inside the width only ever reads the variable's own bytes. An indexed or
indexed-indirect access (`var,X`, `var,Y`, `(var,X)`) is therefore sound *given* a run-time index below the
width - which the compiler cannot check. So it is not refused but **warned** at `severity_optional`
(`error_type_zpauto_indexed_access`, silent until the warning level is raised - the same trust as
`CANCALL` / `UNREACHABLE`), recorded per instruction via the `var_indexed` flag. The allocation proceeds
normally: the variable colours and its full span is reserved.

A **bounds check** lives here too, and it *is* a refusal - it catches the compile-time-known part. Every access
spans a window `[offset, offset + access)` of its variable: `offset` is the constant in `var + offset` (recorded
on the instruction - during assembly the variable's symbol is still the placeholder `0`, so the operand's
evaluated value *is* the offset), and `access` is 1 for a direct byte or 2 for an indirect pointer dereference
(`(var),Y` / `(var)`, which reads the pointer's low+high bytes). If that window runs past the variable's declared
`width`, the stray byte is one the allocator never reserved for it, so it is **refused**. This applies to the
constant *base* of an indexed access too: `var+4,X` on a 4-wide variable is refused because the base is already
off the end, before any index is added. A 1-byte `ZPAUTO1` used as a pointer is the special case that gets the
pointed `error_type_zpauto_narrow_pointer` ("declare it `ZPAUTO2`"); every other overrun - a `var + n` past the
end of a table, a pointer straddling the top of a wider variable - gets `error_type_zpauto_out_of_bounds`. An
unknown (forward-referenced) offset is simply skipped. These checks are decided from the instruction stream
alone, so they run before the CFG is built and a *refusal* bails immediately (an indexed-access warning alone
does not - the allocation carries on).

## Stage 2 - the control-flow graph ##

`cfg_build` (`src/cfg.c`) turns the instruction stream into basic blocks. It is **intraprocedural**: a `JSR`
is an in-block instruction (the callee is reached through the call graph, not a CFG edge), and an `RTS` ends a
block with no successor. So "routines" fall out naturally as the sets of blocks reachable from an entry
without crossing a call or return.

- **Block identity is `(section, pc)`, not `pc` alone.** Two sections (paged banks) may sit at the same
  address, so `pc` cannot name a block. Fall-through and in-section branches stay within one section; only a
  transfer that *named a label* crosses between sections, and the label's marker picks the target section.
- **Leaders** (block entry points) are the first instruction, every branch/jump/call target, every
  fall-through after a branch/jump/return, and every section boundary. The stream is then cut into blocks at
  the leaders. Within a section run, `pc` is strictly monotonic by construction (a section's org is fixed at
  open and its cursor only advances), which `cfg_build` asserts in debug builds - so a within-section address
  collision is impossible and there is nothing to refuse there.
- **Targets** resolve through `cfg_target_block`: a named-label target goes through the markers to its own
  section's block (crossing sections); a bare numeric target resolves within the instruction's own section
  only; a computed target resolves to nothing. An indirect `JMP`'s operand names its *vector cell*, never its
  destination, so it too resolves to nothing directly - `zp_insn.target_via` records the distinction
  (direct / vector / indexed table) so the CFG never mistakes the cell's own address for an edge.
- **External transfers.** A transfer whose *constant* destination matches nothing we assembled is a transfer
  *out of the program*: a `JSR &FFEE` into the OS, a tail `JMP &FFEE`, or a `JMP (&FFFC)` through a vector
  cell that is not one of our own labels (a literal address or a named constant). External code cannot touch
  a ZPAUTO variable - `ZPRESERVE` names precisely the bytes nothing outside the program uses - so an external
  jump/branch arm is as clean an exit as an `RTS` (no taint) and an external call contributes an empty
  footprint. `cfg_target_is_external` is the shared test, consulted only after a target fails to place as a
  block.
- **`unknown_succ`** marks a block whose control may *also* leave to an address we cannot model: an indirect
  `JMP` through a vector *we* assembled (its run-time contents may point back into our own code) with no
  `CANJUMP`, an indexed dispatch (`JMP (table,X)`), or an unresolved target. It is not a plain return - a
  return has no successor and is fully known - and it is not an external exit. `unknown_succ` is the
  conservative taint that later forces "everything live" out of that block.
- Annotations apply here: `UNREACHABLE` prunes a branch's dead fall-through edge; `CANJUMP` wires a computed
  jump's declared targets as real successors (a variable-count successor slice makes a jump table
  representable, not just the one/two edges of a jump/branch). Declared `CANJUMP` / `CANCALL` targets are
  marked as block *leaders* in pass 1, so an in-program declared target always gets its own block even when
  it sits mid-run - which is what lets a declared target with *no* block reliably mean "off the assembled
  stream": an **external arm** of the dispatch (`CANJUMP handler_a, &FFEE`), a clean exit contributing no
  edge and no taint, the annotation-side twin of the external-transfer rule above.

## Stage 3 - liveness and the interference graph ##

`liveness_analyze` (`src/liveness.c`) is a standard backward dataflow fixpoint, plus interference construction:

1. **Per-block use/def.** `use[b]` = variables read before any write in the block; `def[b]` = variables
   written anywhere in it.
2. **Backward fixpoint.** `live_out[b] = union of successors' live_in` (plus *all* variables if `b` is
   `unknown_succ` - the taint), and `live_in[b] = use[b] union (live_out[b] - def[b])`. Iterated round-robin in
   reverse block order until nothing changes. The CFGs are tiny, so there is no worklist. This is the hot loop,
   and the set algebra (`copy` / `union` / equality) is richc's `rc_bitset` word-at-a-time ops.
3. **Interference graph.** Sweep each block backward from its `live_out`: at a def of `a`, `a` interferes with
   everything else live at that point; then advance the live set to *before* the instruction (remove the def,
   add the use). Two vregs interfere iff their live ranges overlap at some program point. All the "output reuses
   input's byte", "the temp cannot overlap the pointer" legality falls out of these overlaps - no special
   cases. `interfere` is a symmetric adjacency structure, one `rc_bitset` row per vreg.
4. **Entry inputs.** Every variable live-in at the routine entry is a synthetic input; they pairwise interfere
   (read-only inputs coexist at entry even if no instruction ever has them simultaneously live). See
   *Deferred*, below, for the single-entry limitation.
5. **Classification.** Each vreg is `input` (live-in at entry), `output` (written, never read inside), `temp`
   (written and read inside), or `unused`. This feeds the colouring's conflict test and diagnostics.

## Stage 4 - interprocedural footprint (calls) ##

Liveness is intraprocedural, so on its own it would miss that a `JSR` clobbers the callee's zero-page
variables. The footprint pass (`src/footprint.c`) closes that gap. A routine's **footprint** `Touch(R)` is
every vreg it touches, transitively through every routine it calls. `footprint_of_call` computes the union
over a single call site's callee(s) - which is exactly what a variable held live across that call collides
with, because the call clobbers all of it.

The walk is a DFS over the call graph seeded at the call target, with an `on_stack` set of routine entries
being computed; revisiting one flags **recursion**. It gathers two sets: `touched` (any vreg touched) and
`killed` (vregs given a *write-only* def - `vref_write` without `vref_read` - anywhere in the walked subtree).
A call whose constant target lies off the assembled stream is *external* (see stage 2) and contributes an
empty footprint; an untrackable target (a computed call, or a callee that itself leaves via a computed jump)
sets `unknown_call`. A `CANCALL` annotation overrides the literal target with the declared set, so a
self-modified or dispatched call can still be bounded (a declared target with no block reads as an external
entry in the dispatch set, contributing nothing). Note the flip side of the external rule: a self-modified
`JSR` whose placeholder operand is a constant now *looks* external, so `CANCALL` is what makes it sound - the
analysis can no longer catch the unannotated case.

Two guards consume this, sweeping each block backward with the live set (so at a call, the live set is exactly
what is live *across* it - a `JSR` touches no variable of its own):

- **Guard 1 - computed flow.** A block flagged `unknown_succ` with any variable in play is refused
  (`error_type_zpauto_computed_flow`): control leaves for code we cannot model, so we cannot prove the variable
  survives. Annotate with `CANJUMP` / `CANCALL` or keep clear.
- **Guard 2 - calls.** At a call with variables live across it, compute the callee footprint:
  - `unknown_call` -> `error_type_zpauto_across_call` (the footprint cannot be bounded; annotate or restructure).
  - recursion carrying a *freshly written* value across itself -> `error_type_zpauto_recursion` (see below).
  - otherwise, add interference edges: every variable live across the call interferes with every vreg in the
    callee's `touched` set. This is what forces a live-across variable off the callee's bytes.

### The recursion rule (why it is not a blanket refusal) ###

Recursion alone is *not* fatal. What cannot live in one static byte is a value the recursion writes *afresh*
at each level and then needs back after the child returns - each level would want its own byte, which one
static address cannot give. A value merely read, or read-modified in place (a shared `DEC`/`INC` counter), is a
single running quantity that rides one byte perfectly.

So Guard 2 refuses recursion **only when a variable live across the recursive call is also in the callee
footprint's `killed` set** (`rc_bitset_intersects(&live, &fp.killed)`) - i.e. it receives a write-only def
somewhere in the cycle. This keys purely off the recorded `rw` flags, independent of which routine declares the
variable, so it accepts a shared counter (only ever `DEC`'d) and still catches a per-level temp (`STA` then
read across the self-call), for both direct and mutual recursion (the footprint walk spans the whole cycle, so
a fresh write anywhere in it is seen). The rule is sound-conservative: it can refuse a little more than the
strict minimum (a fresh write in a non-cyclic callee of a recursive call), never less.

## Stage 5 - colouring ##

`zp_color` (`src/zpalloc.c`) is **first-fit-decreasing** over the widths present, widest first: it sweeps every
width from the widest declared down to 1, placing the more constrained wide variables (a `ZPAUTO 8` table, a
2-byte pointer) before the narrow ones. Each variable is placed at the lowest reserved base whose byte span
`[base, base + width)` is fully reserved and overlaps no already-placed *conflicting* variable's span. Two
variables conflict if they interfere, or if either is `unused` (an unused variable has no liveness to reason
about, so we never prove any sharing safe and keep it on its own byte). A multi-byte variable takes consecutive
reserved bytes. A variable that finds no base **spills** - `any_spilled` is set and the finalizer reports
`error_type_zeropage_full` at that variable's declaration.

First-fit-decreasing is left-edge-optimal for the equal-width common case and good enough for a mix of widths;
the graphs are tiny, so the O(vars^2 x bytes) scan is irrelevant in practice. The span logic (`span_reserved` /
`spans_overlap`) is width-generic, so an arbitrary-width `ZPAUTO <n>` table packs exactly like a pointer, just
wider.

## Stage 6 - patch and rewrite ##

With a spill-free colouring, the finalizer:

1. **Patches operands.** Each recorded instruction was emitted against placeholder base 0, so its operand byte
   held only the *intra-variable* offset (0 for `var`, 1 for `var+1`). `sections_patch_add_u8` folds in the
   assigned base, in the instruction's own section buffer at its recorded offset.
2. **Rewrites symbols.** Each variable's symbol is moved from the placeholder to its real zero-page address, so
   references resolve to the true byte in the result.

Because every reference was already sized as a zero-page access during settling, assigning a real byte and
patching cannot change any instruction's size - the layout stays exactly as it converged. This is why the whole
thing can run *after* convergence without perturbing it.

## Key data structures ##

| Type | File | Role |
|------|------|------|
| `zp_var` | zeropage.h | a declared variable: name, scope, width, def cursor (identity) |
| `zp_insn` | zeropage.h | one var-touching instruction: pc, size, flow, `rw`, vreg, `var_offset`, target (+ `target_via`), `(section, offset)` |
| `zp_cflow` | zeropage.h | a trusted annotation: `UNREACHABLE` / `CANCALL` / `CANJUMP` at a site |
| `zp_label` | zeropage.h | label identity `(scope, def)` -> physical `(section, pc)` |
| `basic_block` | cfg.h | `(section, pc)` identity, insn slice, successor slice, `unknown_succ` |
| `cfg` | cfg.h | blocks + shared successor pool + retained label markers |
| `liveness` | liveness.h | per-block live-in/out, interference rows, per-vreg class |
| `footprint` | footprint.h | `touched` + `killed` sets, `unknown_call`, `recursive` |
| `zp_coloring` | zpalloc.h | per-vreg assigned base (or `RC_INDEX_NONE`), `any_spilled` |

## The certainty contract ##

The governing principle is that the allocator only patches a program it can *prove* correct; anything it cannot
becomes a clear diagnostic pointing at the fix. The refusals, all fatal to the assemble:

| Guard | Code | Cause |
|-------|------|-------|
| 0b | `zpauto_narrow_pointer` | a 1-byte ZPAUTO1 dereferenced as a pointer (`(var),Y` / `(var)`) - needs ZPAUTO2 |
| 0b | `zpauto_out_of_bounds` | a constant `var + n` (or pointer, or indexed base) reaches past the declared width |
| parse | `zpauto_bad_width` | `ZPAUTO <count>` with a count outside 1..256 |
| 1 | `zpauto_computed_flow` | a jump through a vector/table in our own memory reaches unmodelled code with variables live |
| 2 | `zpauto_across_call` | a variable live across a call whose footprint cannot be bounded (the callee reaches computed flow) |
| 2 | `zpauto_recursion` | a freshly-written per-level value held live across a recursive call |
| colour | `zeropage_full` | more simultaneously-live variables than reserved bytes (a spill) |

One diagnostic is a **warning**, not a refusal - the deliberate relaxation for run-time indexing:

| Guard | Code | Severity | Cause |
|-------|------|----------|-------|
| 0 | `zpauto_indexed_access` | `severity_optional` | a ZPAUTO reached by indexed / indexed-indirect addressing; the whole variable is reserved, so an in-width index is sound, but the run-time index is the user's responsibility |

The `CANCALL` / `CANJUMP` / `UNREACHABLE` annotations are the escape hatch: they are *trusted* assertions that
sit exactly where the analysis would otherwise refuse, turning "cannot prove it" into the programmer's explicit
"I promise it is these". A wrong annotation is the one way to defeat the contract - and the indexed-access
warning extends that same trust to the one thing no static check can ever see, the value of an index register.
The external-target policy is a third, *implicit* trust: a constant destination off the assembled stream is
assumed to be an OS/ROM entry that leaves the reserved bytes alone - true for real OS calls (which is the whole
point: `JSR &FFEE` and `JMP (&FFFC)` need no markup), and the programmer's responsibility for a self-modified
placeholder operand or a bare cross-bank number, both of which now read as external rather than refusing.

## Sections / paged banks ##

Because block identity is `(section, pc)` and control transfers resolve targets *by label*, coexisting sections
at the same address are fully distinct to the allocator. Two banks can both define a `.draw` routine at `&8000`
and use ZPAUTO independently; a `JSR bank5.entry` from one into the other resolves through the label marker to
the right bank's block, so the footprint walk crosses banks correctly. A cross-section transfer to a bare
number (`JSR &8003`) cannot say which bank it means; under the external-target policy it is assumed to leave
the program for external code (an empty footprint) - name the target instead when the number really means one
of our own banks, or the analysis will under-approximate.

## Deferred / known limitations ##

- **Single-entry liveness classification.** `liveness_analyze` seeds its entry-input interference and
  `input` classification from a single `entry_block` (0). A bank reached only by a cross-section call is its own
  entry region and should seed the same logic from its own entry. This is a *precision* refinement, not a
  soundness gap: a variable live across a cross-section call already interferes with the callee footprint via
  Guard 2, so the colouring stays correct; and ZPAUTO variables are region-local by design (cross-bank
  persistent state uses `ZPRESERVE`). The effect is a slightly coarser classification in non-entry banks.
- **Cross-section annotation targets.** Ordinary cross-section transfers resolve by label and work today. But a
  `CANCALL` / `CANJUMP` *operand* is evaluated to a bare 16-bit number and resolved in the annotating
  instruction's own section (`n.section`), so it cannot yet name a target in a *different* bank. Fixing it means
  recording the target label's `(scope, def)` on the `zp_cflow` and resolving via the markers, exactly as
  ordinary targets already do. Exotic (computed/dispatched flow that also crosses banks), hence low priority.
- **The reserved set is global.** There is one physical zero page, so one `reserved` map for the whole program.
  Per-section reservation is not a thing (and, given region-local variables, not obviously wanted).

## Where the code lives ##

```
src/zeropage.{h,c}   the manager: reserved set, var/insn/cflow/label registries, vreg resolution
src/cfg.{h,c}        basic-block CFG over the instruction stream, label-resolved targets
src/liveness.{h,c}   backward liveness fixpoint + interference graph + classification
src/footprint.{h,c}  interprocedural Touch(R) per call site, recursion + unknown-call detection
src/zpalloc.{h,c}    first-fit-decreasing graph colouring over the interference graph
src/assemble.c       zeropage_finalize: the driver that runs the stages and the guards, then patches
src/opcodes.c        records each instruction's var touch + flow + target into the zp_insn IR
```
