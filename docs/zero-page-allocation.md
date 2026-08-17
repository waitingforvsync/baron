# Zero page allocation #

Zero page is the 6502's most precious real estate. Instructions that reach it are a byte shorter and a
cycle faster, and the indirect addressing modes (`(ptr),Y` and friends) live there and nowhere else. On a
machine like the Beeb, where the OS and BASIC have already staked a claim on most of it, hand-managing
which routine borrows which byte is exactly the sort of fiddly bookkeeping we would rather a tool did
for us.

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
its high byte `name+1`, so the usual pointer dance just works:

```
ZPAUTO2 ptr
    LDA #LO(data) : STA ptr
    LDA #HI(data) : STA ptr+1
    LDY #0
    LDA (ptr),Y
```

Wider variables are walked the same way - `table`, `table+1`, up to `table+count-1` - or with an index
(`table,X`), which is allowed but yours to bounds-check (see [The rules](#the-rules)).

## What you can rely on ##

The promises, briefly - each proved from your actual code, not guessed:

- **Lifetime, not declaration, decides sharing.** Six variables that take turns can live in two bytes;
  two that overlap need two, even if you only ever touch one at a time.
- **Braces do not fence anything.** Control flow is followed as it really runs - branch out of a block,
  `JMP` into another scope, put three routines in one brace pair or one routine across three. Scopes are
  about *naming*; the analysis follows the branches.
- **Calls are safe.** A variable held live across a `JSR` is kept clear of every byte the callee (and
  everything *it* calls) touches. A variable already dead at the call is free to reuse them. And a call
  *consumes its arguments*: a value stored into a callee's input variable is held live up to the `JSR`,
  so nothing can be allocated over it in between.
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

Declare a routine's inputs and outputs as variables in *its own* scope, and let callers reach them by the
dotted path - the natural calling convention for anything a register or two cannot carry:

```
ZPRESERVE &70..&72          ; three bytes for the lot

.scale                      ; scale.xin * 2 -> scale.res
{
    ZPAUTO1 xin
    ZPAUTO1 res
    LDA xin : ASL A : STA res
    RTS
}

.offset                     ; offset.xin + 7 -> offset.res
{
    ZPAUTO1 xin
    ZPAUTO1 res
    LDA xin : CLC : ADC #7 : STA res
    RTS
}

.start
    LDA #5 : STA scale.xin
    JSR scale
    LDA scale.res : STA offset.xin      ; one stage's result feeds the next
    JSR offset
    LDA offset.res                      ; A = (5 * 2) + 7
    RTS
```

Four variables, three bytes - the `-v` listing shows who got what:

```
.scale
{
xin = &70 [auto]
res = &71 [auto]
...
.offset
{
xin = &70 [auto]
res = &72 [auto]
```

Both `xin`s share one byte: each stage's argument is dead by the time the other stage's is stored, so the
byte simply passes between them. That is the overlap you want from a calling convention, and the safety
that comes with it:

- **An argument survives to its call.** A store into `scale.xin` is held live until the `JSR scale` that
  consumes it - nothing else can be allocated over it in between, however much code sits there, and an
  argument stored before *two* calls is kept clear of the first callee's workspace too.
- **A result survives to its read.** `scale.res` is live from the store inside `scale` until the caller
  reads it, so it is kept clear of everything its producer touches - which is also why a result never
  shares a byte with its own routine's locals.
- **Nothing is pinned longer than needed.** The argument's byte is free again the moment the callee has
  read it; the result's the moment the caller has. Feed results onward promptly (as `.start` does) and
  the bytes circulate.

And when two routines can never be active together - two arms of a dispatch, say - even their interface
variables overlap: Baron sees no path on which both are live, so the arms reuse each other's bytes
wholesale.

## Annotations ##

Some facts Baron cannot see from the instruction stream, and rather than guess it stops and asks. An
annotation is your promise - so do get it right; a wrong one is the one way to defeat the guarantee.

**`UNREACHABLE`** - after a branch you know is always taken, so the dead fall-through is pruned:

```
    CMP #10
    BCC in_range        ; carry is always clear here, honest
    UNREACHABLE
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

For both, list *every* destination - an omission is how you get it wrong.

## The rules ##

What the allocator will not accept, and what it trusts you with:

- **No unannotated computed flow.** An indirect `JMP` through your own vector, or a jump table, is
  refused while variables are live - annotate it (`CANJUMP` / `CANCALL`) or keep clear. RTS-dispatch has
  no annotation; keep those clear of live variables.
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
- **You cannot name a variable `a`** - `ASL a` would read as accumulator addressing. (`x` and `y` are
  fine; they only mean registers after a comma.)

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
