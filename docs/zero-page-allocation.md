# Zero page allocation #

Zero page is a valuable but limited resource on the 6502. Instructions that reach it are a byte shorter and a
cycle faster, and the indirect addressing modes (`(ptr),Y` and friends) live there and nowhere else. In a
complicated program, hand-managing which zero page locations a routine uses as inputs, outputs or scratch
space is exactly the sort of fiddly bookkeeping we would rather a tool did for us.

So Baron does it. You hand over a pool of zero-page bytes, declare named variables, and Baron works out
where each one lives - packing several onto the same byte when their lifetimes never overlap, and
refusing outright rather than ever emitting code it cannot prove safe. It is entirely opt-in: no
`ZPRESERVE` in your source, no allocator, no cost.

This is the usage guide. If you want to know *how* Baron pulls it off, the machinery lives in
[the internals document](zero-page-allocation-internals.md).

## Contents ##
- [A first taste](#a-first-taste)
- [ZPRESERVE: the pool](#zpreserve-the-pool)
- [ZPAUTO: the variables](#zpauto-the-variables)
- [The address of a variable](#the-address-of-a-variable)
- [What you can rely on](#what-you-can-rely-on)
- [Subroutine inputs and outputs](#subroutine-inputs-and-outputs)
- [Annotations](#annotations)
- [The rules](#the-rules)
- [Tips for tight packing](#tips-for-tight-packing)
- [Errors and warnings](#errors-and-warnings)

## A first taste ##

```
ZPRESERVE &70..&8F      ; hand Baron these 32 bytes to play with

.multiply
{
    ZPAUTO1 m1, m2, result

    STA m1
    STX m2
    LDA #0 : STA result
    ; ... and so on
    RTS
}
```

`m1`, `m2` and `result` are ordinary zero-page symbols as far as the rest of your program is concerned -
`LDA m1` assembles to a two-byte zero-page load - but their addresses are chosen for you, drawn from the
pool, and shared between variables whose lifetimes never clash. The `-v` listing shows you each choice as
the assignment it became: `m1 = &70 [auto]`.

## ZPRESERVE: the pool ##

`ZPRESERVE` declares which bytes the allocator may use - a comma-separated list of addresses and ranges:

```
ZPRESERVE &70..&8F                 ; a run of 32 bytes
ZPRESERVE &70..&8F, &A8..&AF       ; two runs, wherever the OS leaves you gaps
ZPRESERVE &70, &71, &72            ; individual bytes, if you must
```

The reservation is global - one zero page, one pool for the whole program - and must appear before any
variable that wants to use it. Reserve only bytes that are genuinely yours (the allocator trusts the pool
completely), but reserve generously within that: unused bytes cost nothing.

## ZPAUTO: the variables ##

```
ZPAUTO1 counter, temp, flags     ; one-byte variables
ZPAUTO2 src, dst                 ; two-byte pairs - pointers
ZPAUTO 16, sines                 ; a 16-byte table
ZPAUTO WIDTH*2, buf              ; the count is any constant expression, 1 to 256
```

A variable is a normal scoped symbol: declared inside a named block, it is reachable from outside as
`block.var`, exactly like a label - which is how a routine publishes its inputs and outputs (see
[Subroutine inputs and outputs](#subroutine-inputs-and-outputs)). A `ZPAUTO2`'s low byte is `name` and
its high byte `name+1`, so things like this just work:

```
ZPAUTO2 ptr
    LDA #LO(data) : STA ptr
    LDA #HI(data) : STA ptr+1
    LDY #0
    LDA (ptr),Y
```

Wider variables are walked the same way - `table`, `table+1`, up to `table+count-1` - or with an index
(`table,X`), which is allowed but yours to bounds-check (see [The rules](#the-rules)).

## The address of a variable ##

A variable's *address* is chosen at the very end, so during assembly it is a special typed value rather
than a number. You can still use it anywhere the layout does not depend on it, and Baron fills in the
real address at the end:

```
    LDA #v : STA ptr        ; the address as an immediate - seeding a pointer
    LDA #0 : STA ptr+1

.handlers
    EQUW v, w               ; address tables of variables just work

vhi = v + 1                 ; aliases carry the identity: LDA vhi is a use of v

    PRINT "v lives at ", v  ; PRINT is invoked after allocation, so this is the real address
```

You can even dispatch through a variable used as a vector - `JMP (vec)` on a `ZPAUTO2` - which Baron
treats as computed flow through a cell it owns: annotate it with `CANJUMP`, exactly like any jump table
(see [Annotations](#annotations)).

What you cannot do is make the program's *shape* depend on an address: a condition (`IF v <> w`), a
count (`SKIP v`, `ZPAUTO v, q`), a loop bound (`FOR n = v..8`), a section's `org`. Those need a number
before allocation exists, and Baron refuses them at the line - `Cannot use a ZPAUTO address here: 'v'`.
The only arithmetic an address supports is adding or subtracting an integer (that is how `ptr+1` works);
anything else - multiply, compare, a range - is a type error.

## What you can rely on ##

The promises, briefly - each proved from your actual code, not guessed:

- **Lifetime, not declaration, decides sharing.** Three variables that are used in strict succession
  can live in a single byte; two that overlap need two, even if you only ever touch one at a time.
- **Braces do not fence anything.** Control flow is followed as it really runs - branch out of a block,
  `JMP` into another scope, put three routines in one brace pair or one routine across three. Scopes are
  about *naming*; the analysis follows the branches.
- **Calls are safe.** A variable held live across a `JSR` is kept clear of every byte the callee (and
  everything *it* calls) touches. A variable already dead at the call is free to reuse them. A call
  *consumes its arguments* - a value stored into a callee's input variable is held live up to the `JSR` -
  and *delivers its results*: one the callee provably writes on every path is "reborn" at the call, its
  pre-call byte free for reuse. See [Subroutine inputs and outputs](#subroutine-inputs-and-outputs).
- **OS calls are free.** `JSR &FFEE` - or `JSR oswrch` with `oswrch = &FFEE` - targets code outside the
  program, which cannot touch your pool, so nothing special is needed. Same for a tail `JMP &FFEE` or a
  jump through a constant OS vector like `JMP (&FFFC)`.
- **Pointer lifetimes are byte-accurate.** A pointer whose MSB is written once and whose LSB is refreshed
  before each use stays live - nothing is allocated over the MSB. Rewriting *both* bytes ends the old
  value and frees the pair.
- **Sections are free.** Use as many as you like, at any addresses - including two at the same address
  (paged banks), which happily share zero-page bytes since they are never resident together. Just name
  your cross-section entry points: a `JSR label` finds the right bank; a bare `JSR &8003` is assumed to
  leave the program.

## Subroutine inputs and outputs ##

You can declare a routine's inputs and outputs as variables in *its own* scope, and let callers reach
them by their fully scoped (dot separated) path:

```
ZPRESERVE &70..&8F          ; In reality only uses ONE byte for the lot

.scale                      ; scale.input * 2 -> scale.result
{
    ZPAUTO1 input
    ZPAUTO1 result
    LDA input : ASL A : STA result
    RTS
}

.offset                     ; offset.input + 7 -> offset.result
{
    ZPAUTO1 input
    ZPAUTO1 result
    LDA input : CLC : ADC #7 : STA result
    RTS
}

.start
    LDA #5 : STA scale.input
    JSR scale
    LDA scale.result : STA offset.input    ; one stage's result feeds the next
    JSR offset
    LDA offset.result                      ; A = (5 * 2) + 7
    RTS
```

Four variables, and the whole relay runs in a **single byte** - the `-v` listing shows every one of them
landing on `&70`. The value simply flows through it: argument in, doubled in place, relayed, seven added,
result out. That is the optimal packing, and it's due to three guarantees:

- **An argument survives to its call.** A store into `scale.input` is held live until the `JSR scale` that
  consumes it - nothing can be allocated over it in between, however much code sits there, and an
  argument stored before *two* calls is kept clear of the first callee's workspace too.
- **A result survives to its read.** From its store inside the routine to the caller's read, a result is
  protected the whole way - including from its own routine's *later* code: a temp trashed on the way to
  the `RTS` will not land on it.
- **A delivered result is "born" at its call.** Baron proves `offset` writes `result` on every path before
  returning, so the caller's read can only ever see the callee's value - the byte's pre-call contents are
  dead, and the call is where the result's life begins. That is what lets `offset.result` take the exact
  same byte `scale.result` just vacated, instead of being held clear of every earlier call "just in case".

We can show the final point by contriving a contrary scenario. Let's change `offset` so the store is
*conditional*, and watch the allocation change with it:

```
.offset                     ; offset.input + 7 -> offset.result, left alone when the sum is zero
{
    ZPAUTO1 input
    ZPAUTO1 result
    LDA input : CLC : ADC #7 : BEQ @+
    STA result : .@
    RTS
}
```

Now the caller's `LDA offset.result` may see the byte as it was *before* the call, so `result` must
genuinely be preserved across the subroutine - and Baron allocates accordingly: everything else still
shares `&70`, while this `offset.result` alone is kept clear of the whole journey on `&71` (the pool
needs two bytes).

Nothing was declared; the routine's own instructions are the specification.

### Wasteful code ###

When using the allocator, it is worth glancing at the verbose listing now and then to see what was really
produced. In the example above, the relay line assembles to a load and a store of the *same* byte:

```
  0015  A5 70           LDA scale.res
  0017  85 70           STA offset.xin
```

The store is wasted - harmless, but six cycles of nothing. You might be tempted to guard the relay with
`IF scale.res <> offset.xin` - but **an `IF` on a ZPAUTO variable is an error, and the assembly fails**:

```
relay.6502:8:7: error: Incompatible types
```

A conditional cannot depend on an allocated address, because addresses are assigned only *after* the
whole assembly has converged - at `IF` time there is nothing to compare. Baron refuses rather than
guessing, and the same applies to every construct that would let the program's *shape* depend on an
address: `IF var` alone (`Cannot use a ZPAUTO address here: 'v'`), `SKIP var`, `FOR n = var..8`,
`org = var` and friends - see [The address of a variable](#the-address-of-a-variable).

If you want the copy gone, say so by *naming*: when two stages should hand a value over in place, give
that value one name they share -

```
ZPAUTO1 pipe                ; scale's result IS offset's input

.scale  { ZPAUTO1 xin : LDA xin : ASL A : STA pipe : RTS }
.offset { ZPAUTO1 res : LDA pipe : CLC : ADC #7 : STA res : RTS }

    JSR scale
    JSR offset              ; no relay - the value is already where offset looks
```

They still share a single location, and the relay instructions are gone entirely. The scoped symbol paths
buy decoupling; the shared variable buys fusion. Choose your weapon!

## Annotations ##

Some facts Baron cannot see from the instruction stream, and rather than guess it stops and asks. An
annotation is your promise to the allocator.

**`UNREACHABLE`** - after a branch you know is always taken, so the dead fall-through is pruned:

```
    ADC #10
    BCC in_range        ; carry is always clear here, honest
    UNREACHABLE         ; This is actually some pretty neat documentation, zp allocator or not
```

It emits nothing and changes nothing; it just lets Baron pack tighter.

**`CANCALL`** - after a `JSR` whose real target is computed (self-modified, or dispatched through a
table), naming every routine it can reach:

```
    JSR dispatch                ; address patched at runtime
    CANCALL handler_a, handler_b, handler_c
```

Do annotate a self-modified call even when its placeholder operand looks like an innocent constant - the
constant reads as a harmless OS call, which is not what the patched `JSR` does at runtime. A destination
may be external (`CANCALL handler_a, &FFEE`): that arm contributes nothing.

**`CANJUMP`** - after a computed or indirect `JMP` through a vector or table *you* assembled, naming
every landing site:

```
    JMP (vector)                ; one of the mode handlers - Baron cannot see which
    CANJUMP mode_draw, mode_erase, mode_flip
```

External arms work here too: `JMP (myvec) : CANJUMP &FFEE` says the vector may hold an OS address, and
that arm is as clean an exit as an `RTS`. (A jump through a *constant* OS vector needs no annotation at
all.)

`CANJUMP` also marks the **RTS-dispatch trick** - pushing a target address and jumping to it with an
`RTS`. An unmarked `RTS` looks exactly like a real return, and Baron trusts it as one; the annotation
tells it this one is a jump, and the analysis follows control to the declared targets like any other
dispatch:

```
    LDA #HI(handler-1) : PHA
    LDA #LO(handler-1) : PHA
    RTS                         ; not a return: it "returns" into handler
    CANJUMP handler
```

The `PHP : RTI` flavour (address pushed unadjusted) is annotated the same way.

For all of these, list *every* destination - if you omit something by mistake, you may see your zp
getting clobbered unexpectedly.

## The rules ##

What the allocator will not accept, and what it trusts you with:

- **No unannotated computed flow.** An indirect `JMP` through your own vector, or a jump table, is
  refused while variables are live - annotate it (`CANJUMP` / `CANCALL`) or keep clear. An RTS-dispatch
  is the one computed transfer Baron *cannot* refuse: it is indistinguishable from a real return, so an
  unmarked one is silently trusted - annotating it with `CANJUMP` is on you.
- **A pointer dereference needs a `ZPAUTO2`.** `(var),Y` reads two bytes; dereferencing a one-byte
  variable that way is refused - declare it `ZPAUTO2` or wider.
- **`var+n` must stay inside the variable.** `var+1` on a `ZPAUTO1`, or `table+16` on a `ZPAUTO 16`, is
  refused - that byte belongs to someone else. The constant base of an indexed access is checked the same
  way (`table+16,X` is off the end before X gets a say).
- **The run-time index is yours.** `table,X` is fine while X stays inside the declared width; Baron
  cannot check that, so it is trusted to you (an opt-in warning, if you want to audit every site). It
  likewise cannot see a self-modified operand or a stray pointer aimed into the pool - those are
  preconditions, not diagnostics.
- **No fresh per-level value across recursion.** A variable written afresh at each level of a recursive
  call and needed back afterwards cannot live in one static byte, and is refused. A counter merely
  `DEC`ed/`INC`ed through the recursion is one running value and works fine - seed it outside the cycle:

  ```
      LDA #5 : STA n          ; seeded outside the recursion
      JSR down
  .down
      DEC n : BEQ @+
      JSR down                ; n rides through happily on one byte
  .@  RTS
  ```

- **Cross-section transfers go through labels**, so Baron knows which bank you mean.

## Tips for tight packing ##

Reuse is driven by lifetimes, so keep lifetimes short:

- Read a variable for the last time as early as you can - the moment after its final read, its byte is
  up for grabs.
- Don't hold values across calls you don't have to; a variable live across a `JSR` must dodge the
  callee's entire footprint. Finish with it first, or recompute it after.
- Prefer many short-lived temporaries to a few long-lived ones - ten temps that take turns can share two
  or three bytes.
- Fully re-initialise a pointer when you are done with the old value; rewriting both bytes is what
  releases the pair.
- Reach for `UNREACHABLE` on always-taken branches; the pruned path frees whatever only lived there.

If you run out of bytes, Baron names the variable it could not place - usually the fix is a bigger
`ZPRESERVE`, or a shorter lifetime on something that overlaps more than it needs to.

## Errors and warnings ##

Every error here is a refusal - Baron will not emit code it cannot vouch for. Fix it, annotate it, or
fall back to a hand-placed address.

| Message | What happened |
|---------|---------------|
| `ZPRESERVE address outside the zero page` | The pool can only contain bytes &00-&FF. |
| `ZPAUTO needs a prior ZPRESERVE` | A variable with no pool reserved. Add a `ZPRESERVE` first. |
| `ZPAUTO count must be 1 to 256` | A generic `ZPAUTO n` with a count outside 1-256. |
| `Cannot use a ZPAUTO address here: '...'` | A variable's address used where a number is needed before allocation exists - a condition, a count, an `org`, an annotation target. See [The address of a variable](#the-address-of-a-variable). |
| `ZPAUTO variable cannot be named 'A'` | The accumulator clash. Rename it. |
| `No free zero-page byte for ZPAUTO variable: '...'` | A spill: more variables live at once than reserved bytes. |
| `ZPAUTO variable live across an unanalysable JSR (annotate with CANCALL)` | A variable held across a call whose destination Baron cannot follow. |
| `Computed jump reaches unknown code (annotate with CANJUMP)` | A jump table or indirect `JMP` the analysis cannot follow. |
| `ZPAUTO variable freshly written and held live across recursion` | A per-level value in a call cycle - one byte cannot hold a value per level. |
| `ZPAUTO1 dereferenced as a pointer (declare it ZPAUTO2)` | `(var),Y` on a one-byte variable. |
| `Access past the end of ZPAUTO variable` | A `var+n` offset outside the declared width. Widen it or fix the offset. |

And two warnings:

| Message | Level | What happened |
|---------|-------|---------------|
| `Unused ZPAUTO variable: '...'` | default | No instruction touches it, so it gets no address and **no definition** - referencing it is an error, exactly as if the declaration were not there. Use it or remove it. |
| `Unchecked indexed access into ZPAUTO variable: '...'` | opt-in | An indexed access (`var,X`, `var,Y`, `(var,X)`) - allowed, but the run-time index is yours to keep in range. |
