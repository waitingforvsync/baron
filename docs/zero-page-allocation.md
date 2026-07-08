# Zero page allocation #

Zero page is the 6502's most precious real estate. Instructions that reach it are a byte shorter and a cycle
faster, and a handful of addressing modes (`(ptr),Y` and friends) live there and nowhere else. On a machine
like the Beeb, where the OS and BASIC have already staked a claim on most of it, the bytes you get to keep are
few and fought over - and hand-managing which routine borrows which byte is exactly the sort of fiddly
bookkeeping we would rather a tool did for us.

So Baron does it. You reserve a pool of zero-page bytes, declare named variables, and Baron works out where
each one lives - packing several variables onto the same byte when their lifetimes never overlap, the way a
compiler allocates registers. You never write an address; you write a name, and Baron proves an assignment that
cannot clobber anything and patches it in.

This is opt-in and costs nothing until you ask for it. No `ZPRESERVE`, no allocator, no overhead.

## Contents ##
- [A first taste](#a-first-taste)
- [Reserving the pool: ZPRESERVE](#reserving-the-pool-zpreserve)
- [Declaring variables: ZPAUTO1 and ZPAUTO2](#declaring-variables-zpauto1-and-zpauto2)
- [How it works](#how-it-works)
- [Scopes, blocks and control flow](#scopes-blocks-and-control-flow)
- [Across subroutine calls](#across-subroutine-calls)
- [Annotations](#annotations)
- [Limitations](#limitations)
- [What doesn't work (yet)](#what-doesnt-work-yet)
- [Getting the most out of it](#getting-the-most-out-of-it)
- [The errors, and what they mean](#the-errors-and-what-they-mean)

## A first taste ##

```
ZPRESERVE &70..&8F      ; hand Baron these 32 bytes to play with

.multiply
{
    ZPAUTO1 a, b, result

    STA a
    STX b
    LDA #0 : STA result
    ; ... and so on
    RTS
}
```

`a`, `b` and `result` are ordinary zero-page symbols as far as the rest of your program is concerned - `LDA a`
assembles to a two-byte zero-page load - but their actual addresses are chosen for you, drawn from the reserved
pool, and reused between variables whose lifetimes do not clash. If `a` is dead by the time `result` is first
written, they can share a byte, and Baron will let them.

## Reserving the pool: ZPRESERVE ##

`ZPRESERVE` declares which zero-page bytes the allocator may draw from. It takes a comma-separated list of
addresses and ranges:

```
ZPRESERVE &70..&8F                 ; a single run of 32 bytes
ZPRESERVE &70..&8F, &A8..&AF       ; two runs, wherever the OS leaves you gaps
ZPRESERVE &70, &71, &72            ; individual bytes, if you must
```

Ranges use Baron's usual inclusive `..`, so `&70..&8F` is `&70` up to and including `&8F`.

The presence of a `ZPRESERVE` is what switches the whole feature on. With none in your source, the allocator
never runs and the IR is never built - there is genuinely no cost. The reservation is **global**: there is one
physical zero page shared by all your code, so there is one pool for the whole program. (Per-overlay reuse of
the same bytes is a refinement for another day.)

A `ZPRESERVE` must appear before any variable that wants to use it. It is fine to reserve more than you think
you need; you only pay for bytes the allocator actually hands out.

## Declaring variables: ZPAUTO1 and ZPAUTO2 ##

Two keywords, because the width lives in the name:

- `ZPAUTO1` declares one-byte variables.
- `ZPAUTO2` declares two-byte variables - pointers, or any 16-bit quantity.

Both take a comma-separated list of names:

```
ZPAUTO1 counter, temp, flags
ZPAUTO2 src, dst              ; two pointers
```

A variable is a normal scoped symbol. Declared inside a named block it is reachable from outside by its dotted
path, exactly like a label:

```
.sprite
{
    ZPAUTO2 addr
    ; ...
}

; elsewhere:
    LDA sprite.addr
    LDA sprite.addr+1
```

For a `ZPAUTO2`, the low byte is the name and the high byte is `name+1` - and both count as touching the same
variable, so Baron tracks them as one. Its intended use is as a pointer:

```
ZPAUTO2 ptr
    LDA #LO(data) : STA ptr
    LDA #HI(data) : STA ptr+1
    LDY #0
    LDA (ptr),Y             ; the whole pair read as a pointer - a use of ptr
```

## How it works ##

The trick that makes this fit an assembler cleanly is that **a variable is always a zero-page address**, no
matter which byte it eventually lands on. So every `LDA var` is a two-byte zero-page instruction from the very
first pass, whatever address we choose in the end. The layout of your program never depends on the allocation,
which means Baron can let the addresses settle as usual with the variables standing in as fixed-size
placeholders, and then run allocation as a final step once everything else has converged - patching the chosen
address into each operand byte in place. Nothing moves; nothing changes size.

Underneath, once the layout has settled, Baron:

1. Recovers a control-flow graph from the emitted instructions - basic blocks, branches, the lot.
2. Runs a backward liveness analysis to find, for every point in the code, which variables are still needed.
3. Builds an interference graph: two variables interfere if they are ever live at the same moment.
4. Colours that graph - packing variables that never interfere onto the same byte - and writes the addresses
   back.

The upshot is that lifetime, not declaration, decides sharing. Six variables that take turns can live in two
bytes; two that overlap need two bytes even if you only ever touch one at a time.

## Scopes, blocks and control flow ##

It is worth being clear about this, because it is where an assembler's instincts can mislead you: **a scoped
block is not the unit the allocator reasons about.** A `{ ... }` block, or a `.name { ... }` named one, is about
*naming* - where a variable is declared, and how it is spelled from outside (`routine.var`). It is not a
boundary the analysis respects.

The unit the allocator reasons about is the **basic block**, and it recovers those - and the edges between them
- from the instructions you actually emitted: the branch targets, the jumps, the fall-throughs. So control flow
is followed as it really runs, and it is free to cross brace boundaries. You can branch out of a block to a
shared exit in another scope, or `JMP` to a label that happens to live inside a different one, and the liveness
analysis follows right along:

```
ZPRESERVE &70..&7F

.work
{
    ZPAUTO1 n
    STA n
    BEQ done            ; leave the block entirely...
    LDA n : STA n
}

.done
    LDA work.n          ; ... n is still live here, across the brace and the branch
    RTS
```

Here `n` is live from its store in `.work`, across a branch that leaves the block, all the way to a read in
`.done`. The braces do not fence the analysis; the `BEQ` does. Naming still is by scope, mind - that is why the
read at `.done` spells it `work.n` - but the *control flow* is recovered from the real branch.

Two consequences fall out for free. One braced block can hold several routines (several `JSR` targets), and one
routine can span several braces. So "one routine per block" is not a rule you have to follow - it is simply what
the analysis reconstructs when that is what your code does. The only thing it cannot follow is flow it cannot
see statically (see below).

## Across subroutine calls ##

A `JSR` is where naive allocation comes unstuck: the callee is free to trample any zero-page it likes, so
anything the caller is holding across the call had better not share a byte with anything the callee touches.

Baron handles this properly. It computes each routine's **footprint** - every variable it touches, plus
everything its callees touch, all the way down - and a caller variable that is live across a call is made to
interfere with the callee's entire footprint. So it is placed out of harm's way. A variable that is already
dead by the time of the call is free to reuse the callee's bytes, because the call cannot corrupt something
nobody is going to read again.

```
ZPRESERVE &70..&8F
    ZPAUTO1 keep

    STA keep
    JSR work        ; work has its own locals; keep is live across this
    LDA keep        ; ... and needed here, so it must survive the call
    RTS

.work
{
    ZPAUTO1 scratch
    STA scratch : LDA scratch
    RTS
}
```

Here `keep` is guaranteed a different byte from `work.scratch`, because the footprint rule puts them in each
other's way in the interference graph. A variable that is dead across the same call would be free to reuse
`work.scratch`'s byte instead - the graph is what decides, so you get the tightest packing that is provably
safe.

## Annotations ##

Baron will only allocate a byte when it can prove the assignment is correct. Some things it cannot see from the
instruction stream alone - an always-taken branch, or a call whose real destination is computed at runtime -
and rather than guess and risk silently broken code, it stops and asks. Annotations are how you supply the
missing fact. They are a promise from you to Baron, so do get them right; a wrong annotation is one of the very
few ways to defeat the correctness guarantee.

### UNREACHABLE ###

Place `UNREACHABLE` immediately after a branch you know is always taken. It tells the allocator that control
cannot fall through to that point, so the never-taken path is pruned from the analysis:

```
    CMP #10
    BCC in_range        ; here we KNOW carry is always clear
    UNREACHABLE         ; ... so the fall-through never happens
    ; (dead code the analysis would otherwise trust)
.in_range
```

This is purely a precision win: with the dead edge gone, variables used only down that impossible path no
longer look live across the branch, so they free up bytes for reuse. It never changes what your program does -
it emits nothing - it just lets Baron pack more tightly.

### CANCALL ###

Place `CANCALL` immediately after a `JSR` whose real target the analysis cannot follow - a self-modified call
address, or a dispatch through a table - and list the routines it might actually reach:

```
    JSR dispatch                ; address is patched at runtime
    CANCALL handler_a, handler_b, handler_c
```

Now Baron bounds the call's footprint by the union of those routines, exactly as if you had called them
directly, so a variable held live across the call is protected from all of them. Without the annotation, such a
call has an unknown footprint and Baron refuses to allocate anything live across it (see `across_call` below).
List every routine the call can reach - an omission is the one way to get it wrong.

### CANJUMP ###

Reserved for a future release. It will do for an indirect `JMP` (a jump table) what `CANCALL` does for a
computed call. For now, a computed jump with variables in play is refused, not annotated - you will need to
restructure or keep those bytes out of the reserved pool.

## Limitations ##

The analysis is sound, which means it is conservative: when it cannot be certain, it refuses rather than
risking a miscompile. These are the shapes it needs, and the ones it will not accept without help.

- **Direct addressing only.** A variable is reached as `var`, `var+1` (the high byte of a `ZPAUTO2`), or as a
  pointer through `(var),Y`. Reaching one by a wandering index (`var,X` with an unbounded X), self-modifying its
  operand, or pointing into it from elsewhere in zero page is outside the envelope - use a hand-placed symbol
  for those.
- **Statically recoverable flow is the only real requirement.** Every branch and jump target must be a label
  the assembler can resolve - a known address, not a computed one. Within that you have a free hand: a block may
  have several entry points (each its own `JSR` target), several `RTS` exits, early-outs, and branches or jumps
  that cross scope boundaries. A routine is whatever the CFG reconstructs, not what the braces suggest, so none
  of that is a restriction - see [Scopes, blocks and control flow](#scopes-blocks-and-control-flow). It is only
  flow the assembler cannot *see* that is off-limits (next item).
- **No unannotated computed flow.** An indirect `JMP`, a jump table or an RTS-dispatch whose target the analysis
  cannot follow is refused while variables are live. Annotate it (`CANCALL`, and in time `CANJUMP`) or keep
  clear. This - not the shape of your entries and exits - is the genuine boundary.
- **No recursion.** A value held live across a recursive call cannot live in one static byte - each level would
  need its own. Baron detects the cycle and refuses.
- **You cannot name a variable `a`.** It collides with accumulator addressing: `ASL a` would read as `ASL A`
  and quietly lose the variable. Names `x` and `y` are fine - they only mean registers after a comma, which a
  plain operand never is - so `STA x` resolves to your variable, not the X register.

Most of these fail loudly. If Baron cannot prove an allocation correct, it says so and points at the spot, so
you can annotate it or reach for a manual address. That is the whole contract: it would rather stop than hand
you code that assembles cleanly and runs wrong.

## What doesn't work (yet) ##

Some concrete shapes, and what Baron does with each. The first four it catches and refuses; the last is a
soundness precondition it does not yet police, so it is on you.

**Recursion is refused.** A value held across a call to yourself cannot live in one static byte:

```
.countdown
{
    ZPAUTO1 n
    STA n
    JSR countdown       ; n is live across a call into the same routine
    LDA n
    RTS
}
```
> A ZPAUTO variable is live across a recursive call ...

**A computed jump is refused** while variables are in play, because Baron cannot see where it lands (this is
what `CANJUMP` will one day annotate):

```
    ZPAUTO1 state
    STA state
    JMP (vector)        ; an indirect jump - to where?
```
> A computed or indirect jump reaches unknown code ...

**A variable named `a` is refused** at the point of declaration - it clashes with accumulator addressing:

```
    ZPAUTO1 a           ; ASL a would read as ASL A and lose the variable
```
> A ZPAUTO variable cannot be named 'A' ...

**Over-subscribing the pool is refused** as a spill - more variables live at once than you reserved bytes:

```
ZPRESERVE &70                       ; one byte
    ZPAUTO1 p, q
    STA p : STA q : LDA p : LDA q   ; p and q are both live - two into one won't go
```
> No free zero-page byte left ...

**Indexed access is NOT yet caught** - and this is the one to watch. `var,X` with a wandering X reads whatever
byte is at `var + X`, which the allocator may well have handed to another variable:

```
    ZPAUTO1 table
    LDX #4
    LDA table,X         ; reads table+4 - possibly somebody else's byte
```

Baron will assemble this today without complaint, but it is outside the envelope: the allocator assumes a
variable is reached only directly. Until the envelope is enforced, keep indexed reads, `(var,X)`-style access,
and self-modified operands on hand-placed symbols rather than auto-variables.

## Getting the most out of it ##

Reuse is driven by lifetimes, so the way to pack tightly is to keep lifetimes short:

- **Read a variable for the last time as early as you can.** The moment after its final read it is dead, and its
  byte is up for grabs. A value you stash "just in case" and read much later pins its byte for the whole span
  in between.
- **Don't hold values across calls you don't have to.** A variable live across a `JSR` has to dodge the callee's
  entire footprint. If you can finish with it before the call, or recompute it after, it costs far less.
- **Prefer many short-lived temporaries to a few long-lived ones.** Ten temps that take turns can share two or
  three bytes; two that overlap the whole routine need two bytes to themselves.
- **Reserve enough, but reserve the truth.** Only reserve bytes that are genuinely yours - the allocator trusts
  the pool completely. Reserving generously within that is free; unused bytes cost nothing.
- **Reach for UNREACHABLE on hot paths.** If a branch really is always taken, saying so lets Baron drop the dead
  path and reclaim whatever lived on it.

If you run out of bytes, Baron tells you which variable it could not place - usually the fix is a bigger
`ZPRESERVE`, or a shorter lifetime on something that is overlapping more than it needs to.

## The errors, and what they mean ##

| Message                                     | What happened                                                                 |
|---------------------------------------------|-------------------------------------------------------------------------------|
| ZPAUTO needs a ZPRESERVE block before it    | You declared a variable with no pool reserved. Add a `ZPRESERVE` first.        |
| No free zero-page byte left ...             | A spill: more variables are live at once than you reserved bytes for.          |
| ... live across a JSR whose callee footprint cannot be determined | A computed or off-stream call with a variable live across it. Annotate it with `CANCALL`. |
| ... live across a recursive call            | A cycle in the call graph. One static byte cannot hold a per-recursion value.  |
| A computed or indirect jump reaches unknown code | A jump table or indirect `JMP` the analysis cannot follow. Restructure, or keep clear until `CANJUMP` lands. |
| A ZPAUTO variable cannot be named 'A'       | The accumulator clash. Rename it.                                              |

Every one of these is a refusal, not a warning - Baron will not emit code it cannot vouch for. Fix it, annotate
it, or fall back to a hand-placed address, and you are on solid ground again.
