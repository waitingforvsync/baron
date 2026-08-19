# Zero page allocation #

Zero page is a valuable but limited resource on the 6502. It's typically used as if it were an extended
register file as it's faster to access, and is the only way to access values through a pointer. In a
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

As far as the rest of your program is concerned, `m1`, `m2` and `result` are ordinary zero-page
symbols - `LDA m1` assembles to a two-byte zero-page load, just as if you had written `m1 = &70`
yourself. The difference is that you didn't: Baron picked the addresses for you, from the pool you
reserved. Better still, if two variables are never needed at the same moment, Baron happily gives them
the *same* byte. The `-v` listing shows you each choice as the assignment it became: `m1 = &70 [auto]`.

## ZPRESERVE: the pool ##

`ZPRESERVE` tells Baron which bytes it's allowed to use - a comma-separated list of addresses and
ranges:

```
ZPRESERVE &70..&8F                 ; a run of 32 bytes
ZPRESERVE &70..&8F, &A8..&AF       ; two runs, wherever the OS leaves you gaps
ZPRESERVE &70, &71, &72            ; individual bytes, if you must
```

There is one zero page, so there is one pool for the whole program, and it must appear before any
variable that wants to use it. Only reserve bytes that are genuinely yours - Baron takes your word for
it - but within that, reserve generously: an unused byte costs nothing.

## ZPAUTO: the variables ##

```
ZPAUTO1 counter, temp, flags     ; one-byte variables
ZPAUTO2 src, dst                 ; two-byte pairs - pointers
ZPAUTO 16, sines                 ; a 16-byte table
ZPAUTO WIDTH*2, buf              ; the count is any constant expression, 1 to 256
```

A variable is a normal scoped symbol: declare it inside a named block and it can be reached from
outside as `block.var`, exactly like a label - which is how a routine publishes its inputs and outputs
(see [Subroutine inputs and outputs](#subroutine-inputs-and-outputs)). A `ZPAUTO2`'s low byte is `name`
and its high byte `name+1`, so things like this just work:

```
ZPAUTO2 ptr
    LDA #LO(data) : STA ptr
    LDA #HI(data) : STA ptr+1
    LDY #0
    LDA (ptr),Y
```

Wider variables work the same way - `table`, `table+1`, up to `table+count-1` - or with an index
(`table,X`), which is allowed but yours to bounds-check (see [The rules](#the-rules)).

## The address of a variable ##

While your program is being assembled, a variable doesn't have an address yet - Baron only picks the
addresses at the very end, once it has seen everything. Until then, the address is a placeholder. That's
fine almost everywhere: as long as the code being produced doesn't *change* depending on the address,
you can use the placeholder freely and Baron fills in the real number at the end:

```
    LDA #v : STA ptr        ; the address as an immediate - seeding a pointer
    LDA #0 : STA ptr+1

.handlers
    EQUW v, w               ; address tables of variables just work

vhi = v + 1                 ; aliases carry the identity: LDA vhi is a use of v

    PRINT "v lives at ", v  ; PRINT happens after allocation, so this is the real address
```

What you can't do is make the *shape* of the program depend on an address: a condition (`IF v <> w`), a
count (`SKIP v`, `ZPAUTO v, q`), a loop bound (`FOR n = v..8`), a section's `org`. Those all need a real
number while Baron is still assembling - before any addresses exist - so Baron refuses them at the
line: `Cannot use a ZPAUTO address here: 'v'`. The only arithmetic a placeholder address supports is
adding or subtracting a whole number (that is how `ptr+1` works); anything else - multiply, compare, a
range - is a type error.

## What you can rely on ##

Baron doesn't guess at any of this: it reads your instructions and traces what actually happens to each
variable. These are the promises it makes:

- **Sharing is decided by *when* a variable matters, not where you declared it.** A variable matters
  from the moment you store something into it until the last time you read that value back. Three
  variables used strictly one after another can all live in a single byte; two whose uses overlap need
  two bytes, even if you only ever touch one at a time.
- **Braces don't fence anything.** Baron follows your code the way the CPU runs it - branch out of a
  block, `JMP` into another scope, put three routines in one brace pair or one routine across three.
  Scopes are about *naming*; the analysis follows the branches.
- **Calls are safe.** If you're partway through using a variable when you `JSR` somewhere, Baron keeps
  that variable's byte away from everything the called routine touches - and everything *it* calls, all
  the way down. A variable you've finished with is free to be reused there. A value you store for the
  callee to read is kept safe right up to the `JSR`, and a result the callee writes is kept safe until
  you read it. See [Subroutine inputs and outputs](#subroutine-inputs-and-outputs).
- **OS calls are free.** `JSR &FFEE` - or `JSR oswrch` with `oswrch = &FFEE` - goes to code outside
  your program, which can't touch your pool, so nothing special is needed. Same for a tail `JMP &FFEE`
  or a jump through a constant OS vector like `JMP (&FFFC)`.
- **Pointers are tracked byte by byte.** A pointer whose high byte is written once and whose low byte
  is refreshed before each use keeps its value - nothing gets allocated over the high byte. Rewriting
  *both* bytes tells Baron you're done with the old value, and the pair is free again.
- **Sections are free.** Use as many as you like, at any addresses - including two at the same address
  (paged banks), which happily share zero-page bytes since they are never resident together. Just name
  your cross-section entry points: a `JSR label` finds the right bank; a bare `JSR &8003` is assumed to
  leave the program.
- **Correctness.** When in doubt, Baron plays it safe rather than emitting broken code, and if it meets
  something it can't reason about, it stops and asks you to annotate it rather than guessing.

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
landing on `&70`. The value simply flows through it: argument in, doubled in place, relayed, seven
added, result out. That's the tightest packing possible, and it comes from three guarantees:

- **An argument survives to its call.** A store into `scale.input` is kept safe until the `JSR scale`
  that reads it - nothing can be allocated over it in between, however much code sits there. Store an
  argument before *two* calls, and it's kept clear of everything the first callee touches too.
- **A result survives to its read.** From its store inside the routine to the caller's read, a result
  is protected the whole way - including from its own routine's *later* code: a temp trashed on the way
  to the `RTS` will not land on it.
- **A guaranteed result needs no protecting beforehand.** Baron can prove that `offset` writes `result`
  on every path before returning - so whatever the caller reads afterwards, it can only ever be the
  value the routine just wrote. Whatever was in that byte *before* the call simply doesn't matter, so
  the byte stays up for grabs right up to the `JSR`. That's what lets `offset.result` take the exact
  same byte `scale.result` has just finished with, instead of being kept empty through every earlier
  call "just in case".

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

Now the caller's `LDA offset.result` may see whatever the byte held *before* the call, so `result` must
genuinely be preserved across the subroutine - and Baron allocates accordingly: everything else still
shares `&70`, while this `offset.result` alone is kept clear of the whole journey on `&71` (the pool
needs two bytes).

Nothing was declared; the routine's own instructions are the specification.

### Wasteful code ###

When using the allocator, it is worth glancing at the verbose listing now and then to see what was
really produced. In the example above, the relay line assembles to a load and a store of the *same*
byte:

```
  0015  A5 70           LDA scale.res
  0017  85 70           STA offset.xin
```

The store is wasted - harmless, but six cycles of nothing. You might be tempted to guard the relay with
`IF scale.res <> offset.xin` - but **an `IF` on a ZPAUTO variable is an error, and the assembly fails**:

```
relay.6502:8:7: error: Incompatible types
```

A conditional can't depend on an allocated address, because the addresses are only chosen once the
whole assembly is finished - at `IF` time there is nothing to compare. Baron refuses rather than
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

They still share a single location, and the relay instructions are gone entirely. The scoped symbol
paths buy decoupling; the shared variable buys fusion. Choose your weapon!

## Annotations ##

Some facts Baron cannot see from the instruction stream, and rather than guess it stops and asks. An
annotation is your promise to the allocator.

**`UNREACHABLE`** - after a branch you know is always taken, telling Baron not to bother with the code
path that never happens:

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

Do annotate a self-modified call even when its placeholder operand looks like an innocent constant -
the constant reads as a harmless OS call, which is not what the patched `JSR` does at runtime. A
destination may be external (`CANCALL handler_a, &FFEE`): that arm contributes nothing.

**`CANJUMP`** - after a computed or indirect `JMP` through a vector or table *you* assembled, naming
every landing site:

```
    JMP (vector)                ; one of the mode handlers - Baron cannot see which
    CANJUMP mode_draw, mode_erase, mode_flip
```

External arms work here too: `JMP (myvec) : CANJUMP &FFEE` says the vector may hold an OS address, and
that arm is as clean an exit as an `RTS`. (A jump through a *constant* OS vector needs no annotation at
all.)

`CANJUMP` also marks the **RTS-dispatch trick** - pushing a target address onto the stack and jumping
to it with an `RTS`. An unmarked `RTS` looks exactly like a real return, and Baron trusts it as one;
the annotation tells it this one is really a jump, and the analysis follows control to the declared
targets like any other dispatch:

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
  refused while any variable still matters there - annotate it (`CANJUMP` / `CANCALL`) or keep clear.
  An RTS-dispatch is the one computed transfer Baron *cannot* refuse: it looks identical to a real
  return, so an unmarked one is silently trusted - annotating it with `CANJUMP` is on you.
- **A pointer dereference needs a `ZPAUTO2`.** `(var),Y` reads two bytes; dereferencing a one-byte
  variable that way is refused - declare it `ZPAUTO2` or wider.
- **`var+n` must stay inside the variable.** `var+1` on a `ZPAUTO1`, or `table+16` on a `ZPAUTO 16`,
  is refused - that byte belongs to someone else. The constant base of an indexed access is checked the
  same way (`table+16,X` is off the end before X gets a say).
- **The run-time index is yours.** `table,X` is fine while X stays inside the declared width; Baron
  can't check that at assembly time, so it trusts you (there's an opt-in warning, if you want to audit
  every site). It likewise can't see a self-modified operand or a stray pointer aimed into the pool -
  keeping those out is your side of the bargain.
- **No fresh per-level value across recursion.** If a recursive routine writes a variable afresh at
  each level of the recursion and needs it back afterwards, one static byte can't hold a value per
  level, and Baron refuses. A counter merely `DEC`ed/`INC`ed through the recursion is one running value
  and works fine - seed it outside the cycle:

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

A byte becomes reusable the moment a variable is finished with, so the trick is to be finished with
things as early as possible:

- Read a variable for the last time as early as you can - the moment after its final read, its byte is
  up for grabs.
- Don't hold values across calls you don't have to; a variable that must survive a `JSR` has to dodge
  every byte the callee touches. Finish with it first, or recompute it after.
- Prefer many short-lived temporaries to a few long-lived ones - ten temps that take turns can share
  two or three bytes.
- Fully re-initialise a pointer when you are done with the old value; rewriting both bytes is what
  releases the pair.
- Reach for `UNREACHABLE` on always-taken branches; the pruned path frees whatever was only needed
  there.

If you run out of bytes, Baron names the variable it could not place - usually the fix is a bigger
`ZPRESERVE`, or noticing that something is being kept around longer than it really needs to be.

## Errors and warnings ##

Every error here is a refusal - Baron will not emit code it cannot vouch for. Fix it, annotate it, or
fall back to a hand-placed address.

| Message | What happened |
|---------|---------------|
| `ZPRESERVE address outside the zero page` | The pool can only contain bytes &00-&FF. |
| `ZPAUTO needs a prior ZPRESERVE` | A variable with no pool reserved. Add a `ZPRESERVE` first. |
| `ZPAUTO count must be 1 to 256` | A generic `ZPAUTO n` with a count outside 1-256. |
| `Cannot use a ZPAUTO address here: '...'` | A variable's address used somewhere that needs a real number during assembly - a condition, a count, an `org`, an annotation target. See [The address of a variable](#the-address-of-a-variable). |
| `ZPAUTO variable cannot be named 'A'` | The accumulator clash. Rename it. |
| `No free zero-page byte for ZPAUTO variable: '...'` | More variables needed at once than the pool has bytes. Reserve more, or shorten a lifetime. |
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
