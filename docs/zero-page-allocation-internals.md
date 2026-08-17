# Zero-page allocation: internals #

The technical companion to [the usage guide](zero-page-allocation.md), for anyone changing the allocator.

The problem: the user declares named zero-page variables and we must give each a real byte such that no two
variables ever simultaneously live share one - and *prove* it, refusing rather than emitting code we cannot
vouch for. This is register allocation by graph colouring, with the zero page standing in for the register
file. The twist is that we are an assembler, not a compiler: we see a raw instruction stream, we only have a
settled layout after multi-pass convergence, and we must cope with subroutine calls, computed jumps, paged
banks sharing addresses, and recursion - each a place where a naive analysis would silently under-approximate
a live range and hand back a wrong byte.

## The pipeline ##

Everything runs in `zeropage_finalize` (`src/assemble.c`), once, after the final assembly pass:

```
  assembly passes (settling)              zeropage_finalize (once, post-convergence)
  --------------------------              ------------------------------------------
  ZPRESERVE -> reserved-byte set          1. resolve vregs  (operand identity -> vreg id)
  ZPAUTO    -> placeholder symbol         2. bounds + indexed-access checks
  final pass -> record the IR:            3. build the CFG  (blocks keyed (section, pc))
    zp_var   declarations                 4. liveness       (backward byte-level fixpoint -> interference)
    zp_insn  instructions                 5. footprints     (what each call clobbers) + the guards
    zp_cflow annotations                  6. colour         (first-fit-decreasing)
    zp_label label markers                7. patch operands + rewrite symbols  (or refuse)
```

If any guard refuses, we record diagnostics, patch nothing, and the assemble fails. Only a fully analysable,
colourable program gets real addresses.

## Recording ##

During the settling passes the allocator's one job is not to disturb convergence. `ZPRESERVE` fills a 256-bit
`reserved` set (re-filled each pass; its presence enables the whole feature). `ZPAUTO` binds its name to a
*placeholder* symbol at address 0, typed as a zero-page access - so every `LDA var` is a two-byte zero-page
instruction from the first pass, whatever byte we later choose. The layout never depends on the allocation;
that is what lets the whole analysis run *after* convergence without perturbing it.

On the final pass, with addresses settled, the manager (`src/zeropage.c`) captures the IR:

- `zp_var` - one per declaration: name, owning scope, width (1-256), and its **def cursor** (the source
  position of the `ZPAUTO`), its identity across passes.
- `zp_insn` - one per instruction (`src/opcodes.c` records them): pc, size, control-flow class, its
  variable touch if any (`rw` read/write flags, constant offset, indexed/indirect flags), the operand's
  `(section, offset)` for later patching, and the transfer target.
- `zp_cflow` - the annotations (`UNREACHABLE` / `CANCALL` / `CANJUMP`).
- `zp_label` - a marker tying a label's identity `(scope, def)` to its physical `(section, pc)` - what lets
  a transfer that *named a label* find the right block even when two banks share an address.

A variable can be used textually before its declaration, so an operand records the binding's
`(scope, def cursor)` pair and `zeropage_resolve_vregs` maps pairs to vreg ids once the registry is complete.
The *pair* is the identity: a macro / `FOR` body's instantiations share one def cursor but run in distinct
child scopes. Operand attribution (`scopes_resolve_symbol_def`) follows the same rules as a value lookup -
a bare name shadows up the parent chain, a dotted path (`routine.var`) descends the child scopes - so a
cross-scope reference to a routine's interface variable attributes, and is patched and analysed, exactly
like a local one.

## Bounds and indexed access ##

Decided from the instruction stream alone, before any CFG exists.

Every access spans a window `[offset, offset + access)` of its variable - `offset` is the constant in
`var + offset` (the operand's evaluated value, since the symbol is still the placeholder 0), `access` is 1
for a direct byte or 2 for an indirect pointer deref (`(var),Y` / `(var)`). A window past the declared width
lands on a byte we never reserved for this variable, so it is **refused**: `zpauto_narrow_pointer` for the
pointer-deref-of-a-1-byte special case, `zpauto_out_of_bounds` for everything else, including the constant
*base* of an indexed access (`var+4,X` on a 4-wide table is off the end before X gets a say).

An indexed access itself (`var,X`, `var,Y`, `(var,X)`) is a **warning**, not a refusal
(`zpauto_indexed_access`, `severity_optional`): the whole variable is reserved and liveness treats any
indexed touch as using all of it, so an in-width index is sound - and the run-time index is the one thing no
static check can see, so it is trusted to the user, the same trust as an annotation.

## The CFG ##

`cfg_build` (`src/cfg.c`) cuts the stream into basic blocks. It is intraprocedural: a `JSR` is an in-block
instruction (callees are reached through the call graph, not CFG edges) and an `RTS` ends a block with no
successor - "routines" fall out as whatever is reachable from an entry without crossing a call or return.

- **Block identity is `(section, pc)`.** Paged banks may share an address, so `pc` alone cannot name a
  block. Within a section, pc is strictly monotonic by construction (org fixed at open, cursor only
  advances) - asserted in debug builds, so within-section collisions cannot happen.
- **Leaders** are the first instruction, every transfer target, every fall-through after a branch / jump /
  return, every section boundary - plus every *declared* `CANCALL` / `CANJUMP` target, so an in-program
  declared target always gets a block even mid-run.
- **Targets** resolve through the label markers when the operand named a label (this is how a transfer
  crosses sections); a bare number resolves within its own section only; a computed target resolves to
  nothing. An indirect `JMP`'s operand names its vector *cell*, not its destination
  (`zp_insn.target_via`), so the cell's address is never mistaken for an edge.
- **External transfers.** A *constant* destination matching nothing we assembled is a transfer out of the
  program - `JSR &FFEE`, a tail `JMP &FFEE`, `JMP (&FFFC)` through a vector cell that is not ours.
  External code cannot touch the pool (`ZPRESERVE` names precisely the bytes nothing else uses), so an
  external exit is as clean as an `RTS` and an external call has an empty footprint. `cfg_target_is_external`
  is the shared test; a declared annotation target with no block reads the same way (an external arm,
  `CANJUMP mode_a, &FFEE`).
- **`unknown_succ`** taints a block whose control may leave for code we cannot model: an unannotated
  indirect `JMP` through our own vector, an indexed dispatch, an unresolved target. The taint later forces
  "everything live" out of that block.
- `UNREACHABLE` prunes a branch's fall-through edge; `CANJUMP` wires the declared targets as real
  successors (a variable-length successor slice, so jump tables are representable).

## Liveness ##

`liveness_analyze` (`src/liveness.c`) is a standard backward dataflow fixpoint plus interference
construction - but it runs over a **byte** id space, not the vreg space: variable `v` owns bytes
`[base[v], base[v] + width[v])` and every dataflow set is a byte set. That is what makes partial writes come
out right: a 6502 store writes one byte, so `STA ptr` alone redefines the LSB while the MSB's old value
flows through and keeps the pointer live - and a `STA ptr : STA ptr+1` pair accumulates, across blocks too,
into a genuine full kill. (Treating any store as a whole-variable kill was the bug that let another variable
be allocated over a pointer's once-written MSB.)

1. Each instruction's touch maps to read/write windows (`insn_window`): a read consumes the addressed byte
   (two for a pointer deref, the whole variable for indexed / unknown offsets); a write provably redefines
   only a direct store's one byte.
2. The round-robin fixpoint: `live_out = union of successors' live_in` (plus *all* bytes under the
   `unknown_succ` taint), then `live_in` comes from walking the block's instructions backward - a write
   removes its provable bytes, a read adds its bytes, and **a call adds its callees' live-in**. That last
   rule is the interprocedural half of the argument story: a call *consumes its inputs*, so a value the
   caller stored into a callee's variable stays live from the store to the `JSR` - nothing can be coloured
   over it in between - and the effect chains transitively through nested calls, since a callee's own
   live-in includes whatever *its* calls inject. Call targets resolve once through `cfg_call_targets`
   (the shared policy: CANCALL overrides, label resolution, external arms contributing nothing; a computed
   unannotated call injects nothing - annotation territory, as ever). The set algebra is richc's
   `rc_bitset`.
3. Interference: sweep each block backward (calls injecting their callees' live-in here too); at any write
   the variable is pinned to its bytes and interferes with the owner of every other live byte. Two vregs
   interfere iff some pair of their bytes' ranges overlap. All the "output reuses the dead input's byte"
   legality falls out of these overlaps - no special cases.
4. Variables live-in at the entry block are the routine's inputs and pairwise interfere (read-only inputs
   coexist at entry even if no instruction sees them simultaneously live).
5. Each vreg is classified `input` / `output` / `temp` / `unused` for the diagnostics and the unused-variable
   handling.

The public results (interference rows, live-in/out, classes) are projected back to variable level - a
variable is live iff any of its bytes is - because the allocator places whole variables.

## Footprints and the guards ##

Liveness is intraprocedural, so alone it would miss that a `JSR` clobbers the callee's variables.
`footprint_of_call` (`src/footprint.c`) computes what a call site reaches: a DFS over the call graph
gathering `touched` (every vreg the callee subtree touches) and `killed` (vregs given a write-only def
anywhere in it); revisiting an on-stack entry flags `recursive`; an untrackable target - a computed call, or
a callee that leaves via computed flow - sets `unknown_call`. A `CANCALL` overrides the literal target with
the declared set (which is what makes a self-modified call sound: its constant placeholder otherwise reads
as external). External targets contribute nothing.

`zeropage_finalize` then sweeps each block backward with a variable-level live set (its kill test,
`zp_insn_write_kills`, ends a range only on a full single-store rewrite - coarser than stage 4's
byte-accurate dataflow, but a too-live set only ever *adds* edges, so it stays sound):

- **Computed flow**: any `unknown_succ` block, with variables in play, is refused
  (`zpauto_computed_flow`) - annotate or keep clear.
- **Calls**: at a call with variables live across it (the live set at a `JSR` *is* the live-across set - a
  call touches no variable of its own): `unknown_call` refuses (`zpauto_across_call`); recursion refuses
  only if a live-across variable is in the cycle's `killed` set (`zpauto_recursion` - a *freshly written*
  per-level value cannot ride one static byte, while a merely-read or `DEC`/`INC`-accumulated one can; the
  test is `live intersects killed`, keyed purely off the rw flags, so it handles direct and mutual recursion
  alike); otherwise every live-across variable gains an interference edge to every vreg in `touched`, which
  is what forces it off the callee's bytes. The sweep then injects the callees' live-in (mirroring the
  dataflow's call-input rule) - *after* the checks, deliberately, so an argument is never "live across" the
  call that consumes it (no spurious edges against its own callee's workspace), yet counts as live across
  any *earlier* call it must survive.

## Colouring, patching, rewriting ##

`zp_color` (`src/zpalloc.c`) is first-fit-decreasing by width: widest variables first, each placed at the
lowest base whose whole span is reserved and overlaps no interfering variable's span. The span logic is
width-generic, so a `ZPAUTO 16` table packs exactly like a pointer, just wider. An `unused` variable is
skipped - no address, not a spill - warned once per declaration site (`zpauto_unused`), and its binding
*removed* at the rewrite step, so it ends up exactly as if never declared. A *used* variable with no home is
a spill: `zeropage_full`, naming it.

With a spill-free colouring, the finalizer patches each recorded operand (`sections_patch_add_u8` folds the
assigned base into the byte, which held only the intra-variable offset) and rewrites each symbol from the
placeholder to its real address. No instruction changes size - every reference was zero-page-sized all
along - so the converged layout is untouched. Under `-v`, the listing pass then re-emits from the rewritten
symbols, reproducing the same bytes the patches produced.

## Data structures ##

| Type | File | Role |
|------|------|------|
| `zp_var` | zeropage.h | a declaration: name, scope, width, def cursor (identity) |
| `zp_insn` | zeropage.h | one instruction: pc, size, flow, touch (`rw`, offset, flags), patch site, target |
| `zp_cflow` | zeropage.h | one annotation at a site |
| `zp_label` | zeropage.h | label identity `(scope, def)` -> physical `(section, pc)` |
| `basic_block` / `cfg` | cfg.h | `(section, pc)`-keyed blocks, successor slices, `unknown_succ` |
| `liveness` | liveness.h | live-in/out, interference rows, per-vreg class (byte-accurate inside) |
| `footprint` | footprint.h | `touched` + `killed`, `unknown_call`, `recursive` |
| `zp_coloring` | zpalloc.h | per-vreg base (or `RC_INDEX_NONE`), `any_spilled` |

## The certainty contract ##

We only patch a program we can prove; everything else becomes a diagnostic pointing at the fix. The
refusals (all fatal): the bounds pair (`zpauto_narrow_pointer` / `zpauto_out_of_bounds`), `zpauto_bad_width`
at parse, `zpauto_computed_flow`, `zpauto_across_call`, `zpauto_recursion`, and `zeropage_full`. The two
warnings: `zpauto_indexed_access` (opt-in) and `zpauto_unused` (default).

The annotations are *trusted* assertions sitting exactly where the analysis would otherwise refuse; a wrong
one is the one way to defeat the contract. The external-target policy is a second, implicit trust: a
constant destination off the assembled stream is assumed to leave the program - true for real OS calls
(the point: `JSR &FFEE` needs no markup), the user's responsibility for a self-modified placeholder or a
bare cross-bank number (name the label and it resolves properly, banks included).

## Known gaps ##

Three deliberate limitations:

- **Single-entry classification.** Entry-input interference and the `input` class seed from block 0. A bank
  reached only by cross-section calls should seed from its own entry - a precision refinement only, since
  Guard 2's footprint edges keep the colouring correct regardless.
- **Annotation operands resolve in their own section.** Ordinary cross-section transfers resolve by label;
  a `CANCALL` / `CANJUMP` *operand* is still a bare number resolved in the annotating instruction's section,
  so it cannot yet name a target in a different bank.
- **The reserved set is global.** One physical zero page, one pool.

## Where the code lives ##

```
src/zeropage.{h,c}   the manager: reserved set, var/insn/cflow/label registries, vreg resolution
src/cfg.{h,c}        basic blocks over the instruction stream, label-resolved targets
src/liveness.{h,c}   backward byte-level fixpoint + interference + classification
src/footprint.{h,c}  per-call-site Touch(R), recursion + unknown-call detection
src/zpalloc.{h,c}    first-fit-decreasing colouring
src/assemble.c       zeropage_finalize: drives the stages and guards, then patches
src/opcodes.c        records each instruction's touch / flow / target into the IR
```
