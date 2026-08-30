# Zero page allocation #

Zero page is a valuable but limited resource on the 6502. It's typically used as if it were an extended
register file as it's faster to access, and is the only way to access values through a pointer. In a
complicated program, hand-managing which zero page locations a routine uses as inputs, outputs or scratch
space is exactly the sort of fiddly bookkeeping we would rather a tool did for us.

So Baron does it. You hand over a pool of zero-page bytes, declare named variables, and Baron works out
where each one lives - packing several onto the same byte when their lifetimes never overlap, and
refusing outright rather than ever emitting code it cannot prove safe. It is entirely opt-in: no
`ZA_POOL` in your source, no allocator, no cost.

This is the usage guide. If you want to know *how* Baron pulls it off, the machinery lives in
[the internals document](zero-page-allocation-internals.md).

## Contents ##
- [A first taste](#a-first-taste)
- [ZA_POOL: the pool](#za_pool-the-pool)
- [ZA_AUTO: the variables](#za_auto-the-variables)
- [The address of a variable](#the-address-of-a-variable)
- [What you can rely on](#what-you-can-rely-on)
- [Subroutine inputs and outputs](#subroutine-inputs-and-outputs)
- [Inline data after a JSR](#inline-data-after-a-jsr)
- [Annotations](#annotations)
- [The rules](#the-rules)
- [Tips for tight packing](#tips-for-tight-packing)
- [Errors and warnings](#errors-and-warnings)

## A first taste ##

```
ZA_POOL &70..&8F        ; hand Baron these 32 bytes to play with

.multiply
{
    ZA_AUTO1 m1, m2, result

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

## ZA_POOL: the pool ##

`ZA_POOL` tells Baron which bytes it's allowed to use - a comma-separated list of addresses and
ranges:

```
ZA_POOL &70..&8F                   ; a run of 32 bytes
ZA_POOL &70..&8F, &A8..&AF         ; two runs, wherever the OS leaves you gaps
ZA_POOL &70, &71, &72              ; individual bytes, if you must
```

There is one zero page, so there is one pool for the whole program, and it must appear before any
variable that wants to use it. Only reserve bytes that are genuinely yours - Baron takes your word for
it - but within that, reserve generously: an unused byte costs nothing.

## ZA_AUTO: the variables ##

```
ZA_AUTO1 counter, temp, flags    ; one-byte variables
ZA_AUTO2 src, dst                ; two-byte pairs - pointers
ZA_AUTO 16, sines                ; a 16-byte table
ZA_AUTO WIDTH*2, buf             ; the count is any constant expression, 1 to 256
```

A variable is a normal scoped symbol: declare it inside a named block and it can be reached from
outside as `block.var`, exactly like a label - which is how a routine publishes its inputs and outputs
(see [Subroutine inputs and outputs](#subroutine-inputs-and-outputs)). A `ZA_AUTO2`'s low byte is `name`
and its high byte `name+1`, so things like this just work:

```
ZA_AUTO2 ptr
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

    PRINT "v lives at &", ~v   ; PRINT happens after allocation, so this is the real address
```

What you can't do is make the *shape* of the program depend on an address: a condition (`IF v <> w`), a
count (`SKIP v`, `ZA_AUTO v, q`), a loop bound (`FOR n = v..8`), a section's `org`. Those all need a real
number while Baron is still assembling - before any addresses exist - so Baron refuses them at the
line: `Cannot use a ZA_AUTO address here: 'v'`. The only arithmetic a placeholder address supports is
adding or subtracting a whole number (that is how `ptr+1` works); the only other operator that will
touch one is `~`, which formats it for `PRINT`. Anything else - multiply, compare, a range - is a type
error.

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
ZA_POOL &70..&8F            ; In reality only uses ONE byte for the lot

.scale                      ; scale.input * 2 -> scale.result
{
    ZA_AUTO1 input
    ZA_AUTO1 result
    LDA input : ASL A : STA result
    RTS
}

.offset                     ; offset.input + 7 -> offset.result
{
    ZA_AUTO1 input
    ZA_AUTO1 result
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
    ZA_AUTO1 input
    ZA_AUTO1 result
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
`IF scale.res <> offset.xin` - but **an `IF` on a ZA_AUTO variable is an error, and the assembly fails**:

```
relay.6502:8:7: error: Incompatible types
```

A conditional can't depend on an allocated address, because the addresses are only chosen once the
whole assembly is finished - at `IF` time there is nothing to compare. Baron refuses rather than
guessing, and the same applies to every construct that would let the program's *shape* depend on an
address: `IF var` alone (`Cannot use a ZA_AUTO address here: 'v'`), `SKIP var`, `FOR n = var..8`,
`org = var` and friends - see [The address of a variable](#the-address-of-a-variable).

If you want the copy gone, say so by *naming*: when two stages should hand a value over in place, give
that value one name they share -

```
ZA_AUTO1 pipe               ; scale's result IS offset's input

.scale  { ZA_AUTO1 xin : LDA xin : ASL A : STA pipe : RTS }
.offset { ZA_AUTO1 res : LDA pipe : CLC : ADC #7 : STA res : RTS }

    JSR scale
    JSR offset              ; no relay - the value is already where offset looks
```

They still share a single location, and the relay instructions are gone entirely. The scoped symbol
paths buy decoupling; the shared variable buys fusion. Choose your weapon!

## Inline data after a JSR ##

A classic 6502 idiom passes data to a subroutine inline, right after the call:

```
    JSR pstring
    EQUS "HELLO", 13, 0
    LDA #0                  ; execution resumes here
```

`pstring` pulls the return address off the stack, walks the text to its terminator, and resumes the
caller past it - either by pushing the adjusted address back and `RTS`ing, or by jumping straight
there. Baron understands the shape: data emits no instructions, so the allocator's view of the caller
simply steps from the `JSR` to the next instruction - exactly where a data-consuming callee resumes.
A callee that returns with a munged `RTS` therefore needs **no annotation at all**: variables live
across the call survive the excursion, and the callee's outputs flow back, just as for an ordinary
call.

A callee that *jumps* back instead ends in computed flow, which the allocator refuses until you name
it - and the name for "this jump goes back to whoever called me" is `ZA_RETURN`:

```
.pstring
{
    ZA_AUTO2 ptr
    PLA : STA ptr : PLA : STA ptr+1
    ...                     ; walk the text; advance ptr past the terminator
    JMP (ptr)
    ZA_RETURN               ; this jump IS our return
}
```

When the callee resumes the caller somewhere *other* than just past the data - a skip table, an error
path - `ZA_RETURNTO` on the caller side names the real resumption point(s). And if a `JSR` genuinely
never comes back at all, say so with `ZA_UNREACHABLE` right after it, and nothing downstream is held
live on its account. All three are described with the other [annotations](#annotations) below.

## Annotations ##

Some facts Baron cannot see from the instruction stream, and rather than guess it stops and asks. An
annotation is your promise to the allocator.

**`ZA_UNREACHABLE`** - after a branch you know is always taken, telling Baron not to bother with the code
path that never happens:

```
    ADC #10
    BCC in_range        ; carry is always clear here, honest
    ZA_UNREACHABLE      ; This is actually some pretty neat documentation, zp allocator or not
```

It emits nothing and changes nothing; it just lets Baron pack tighter. It works after a `JSR` too:
sited right after a call that never returns, it severs the continuation, so nothing is held live for
code that never runs on that path.

**`ZA_CANCALL`** - after a `JSR` whose real target is computed (self-modified, or dispatched through a
table), naming every routine it can reach:

```
    JSR dispatch                ; address patched at runtime
    ZA_CANCALL handler_a, handler_b, handler_c
```

Do annotate a self-modified call even when its placeholder operand looks like an innocent constant -
the constant reads as a harmless OS call, which is not what the patched `JSR` does at runtime. A
destination may be external (`ZA_CANCALL handler_a, &FFEE`): that arm contributes nothing. Targets
may also arrive as a list-valued symbol - `subs = {handler_a, handler_b}` then `ZA_CANCALL subs` -
flattened exactly like `EQUB` data (nested lists and ranges included).

**`ZA_CANJUMP`** - after a computed or indirect `JMP` through a vector or table *you* assembled, naming
every landing site:

```
    JMP (vector)                ; one of the mode handlers - Baron cannot see which
    ZA_CANJUMP mode_draw, mode_erase, mode_flip
```

External arms work here too: `JMP (myvec) : ZA_CANJUMP &FFEE` says the vector may hold an OS address, and
that arm is as clean an exit as an `RTS`. (A jump through a *constant* OS vector needs no annotation at
all.) The declared targets *replace* whatever the literal operand said - a self-modified `JMP`'s
placeholder wires no edge of its own - and list-valued symbols flatten here just as for `ZA_CANCALL`,
so a jump table's target list can live in one word. A self-modified *branch* is annotated the same
way: `BNE placeholder : ZA_CANJUMP real_a, real_b` replaces the taken edge with the declared arms
while the ordinary fall-through stays.

`ZA_CANJUMP` also marks the **RTS-dispatch trick** - pushing a target address onto the stack and jumping
to it with an `RTS`. An unmarked `RTS` looks exactly like a real return, and Baron trusts it as one;
the annotation tells it this one is really a jump, and the analysis follows control to the declared
targets like any other dispatch:

```
    LDA #HI(handler-1) : PHA
    LDA #LO(handler-1) : PHA
    RTS                         ; not a return: it "returns" into handler
    ZA_CANJUMP handler
```

The `PHP : RTI` flavour (address pushed unadjusted) is annotated the same way.

For all of these, list *every* destination - if you omit something by mistake, you may see your zp
getting clobbered unexpectedly.

**`ZA_RETURN`** - after a jump (or branch) whose destination is simply *whoever called this routine*:
the [inline-data trick](#inline-data-after-a-jsr)'s exit, where a routine pops its return address,
consumes the data following the call, and jumps past it:

```
    JMP (ptr)                   ; ptr points just past the inline data
    ZA_RETURN                   ; this jump IS our return
```

No target list - the destination is "back to our caller", and Baron routes liveness home to every
call site exactly as it does for an `RTS`. It also tames a self-modified direct `JMP` whose
placeholder operand happens to name real code (the placeholder edge is ignored, as with `ZA_CANJUMP`).
On a branch it declares a *conditional* return - the not-taken path carries on as usual. And it
composes with `ZA_CANJUMP` on the same jump, for a dispatch that may instead hand straight back.

**`ZA_RETURNTO`** - the caller-side counterpart, for when a data-consuming callee resumes you
somewhere other than just past the data:

```
    JSR emit : ZA_RETURNTO done
    EQUS "payload", 0           ; consumed by emit
    NOP                         ; never executed - emit resumes at .done
.done
```

It takes a target list like `ZA_CANCALL` (list-valued symbols and ranges flatten the same way) and
REPLACES the call's normal continuation. You do not need it for the common shape - a callee that
resumes just past the data resumes at the next instruction, which is what Baron assumes anyway. A
declared target that is not the start of an assembled instruction draws a warning: it is almost
always a mistyped label, or an address inside the data itself.

**`ZA_DISCARD`** - tells Baron a variable's current value is finished with: everything read later comes
from writes after this point. You need it when a variable is rebuilt through indexed stores, because
`STA arr,X` never proves *which* byte it wrote:

```
    LDX #0
.fill
    LDA (src),Y : STA arr,X     ; rebuilds all of arr - but which byte? Baron can't tell
    INX : CPX #4 : BNE fill
    LDA arr+0 : ORA arr+1       ; so these reads look like they might want stale bytes
```

With no provable write anywhere, Baron assumes the old value might still be wanted, keeps `arr` alive
back through the routine's entry and around whatever loop reaches it, and walls its bytes off from
everything. One line fixes it - place it where the old value stops mattering, before the rebuild:

```
    ZA_DISCARD arr              ; the old value is dead; the fill loop makes a new one
```

It emits nothing, takes a comma list, and wants whole variables (`ZA_DISCARD arr+1` is refused). TRUSTED
like the others: if something *does* read the old value past a `ZA_DISCARD`, that byte may already belong
to someone else. Inside a subroutine it also tells callers their copy dies at the `JSR`, just as a real
full rewrite would. One quirk: at a loop's top it re-asserts every time around - Baron cannot tell "on
entry" from "each iteration" at the same address. (In the spritescale demo, two `ZA_DISCARD` lines freed
17 bytes of zero page at zero runtime cost - the two plotters' slot arrays fold onto each other.)

**`ZA_ENTRY`** - put this at the top of any routine that the *outside world* calls. A routine that only
BASIC ever `CALL`s is referenced by nothing in the program, so to Baron it looks like dead code:

```
.blit
    ZA_ENTRY                    ; nothing here calls this - BASIC does
    LDA xpos
    ...
    RTS
```

The marker takes no operand; it just remembers the address where it stands, so either side of the
label works. Normally Baron assumes execution enters at the top of each section. Write one `ZA_ENTRY`
and it stops guessing: your markers are now the complete list of ways in, so mark every routine the
outside calls - the main one included. Anything touching a `ZA_AUTO` variable that none of them can
reach gets a warning: dead code, or a routine you forgot to mark.

One thing `ZA_ENTRY` does *not* do: keep BASIC's pokes and peeks safe. If BASIC does
`?&70=X% : CALL blit`, then `&70` had better not be auto-allocated - the allocator would cheerfully
move it next build. Variables you share with the outside get a fixed home:

```
xpos = &70                      ; BASIC pokes here, so it must stay put - not a ZA_AUTO
```

Baron watches for the mistake: a `ZA_ENTRY` routine that reads a `ZA_AUTO` variable before writing it
is expecting *someone* to have set the value - and the only someone is the outside caller, who cannot
know an auto-allocated address. That draws a warning naming the variable
(`ZA_AUTO input to a ZA_ENTRY routine`). The fix is one of two: give the variable a fixed home as
above, or initialise it in the routine before the first read.

**`ZA_INTERRUPT`** - the same marker, for interrupt handlers. These need more than a "way in", because
an interrupt fires between any two instructions. Say the handler counts frames:

```
ZA_AUTO1 vsync

.irq
    ZA_INTERRUPT
    LDA #&40 : STA &FE4D
    INC vsync
    LDA &FC
    RTI
```

Without the marker, `vsync` looks dead nearly everywhere - only the handler and one wait loop touch
it - so some innocent temporary gets packed onto its byte, and the next interrupt increments your
temporary. With it, Baron keeps the handler's world apart: `vsync` gets a byte all to itself, and
nothing the handler touches can land on a byte the main program might be using when it fires.
Temporaries *inside* the handler still share with each other as usual, and two marked handlers are
kept apart from each other too (an NMI can land mid-IRQ).

Two honest limits: a *multi-byte* variable shared with a handler can still be caught half-updated
(keep shared state to single bytes, or bring your own interlock), and an RTS-dispatch inside a handler
is as invisible here as anywhere - `ZA_CANJUMP` it.

## The rules ##

What the allocator will not accept, and what it trusts you with:

- **No unannotated computed flow.** An indirect `JMP` through your own vector, or a jump table, is
  refused while any variable still matters there - annotate it (`ZA_CANJUMP` / `ZA_CANCALL`, or
  `ZA_RETURN` when the destination is simply your caller) or keep clear.
  An RTS-dispatch is the one computed transfer Baron *cannot* refuse: it looks identical to a real
  return, so an unmarked one is silently trusted - annotating it with `ZA_CANJUMP` is on you.
- **Inline data after a `JSR` is understood.** Data emits no instructions, so the caller's flow steps
  from the call straight to the next instruction - where a data-consuming callee resumes. A callee
  returning by computed jump wants `ZA_RETURN`; a caller resumed elsewhere wants `ZA_RETURNTO`; a
  call that never returns wants `ZA_UNREACHABLE` after it.
- **A pointer dereference needs a `ZA_AUTO2`.** `(var),Y` reads two bytes; dereferencing a one-byte
  variable that way is refused - declare it `ZA_AUTO2` or wider.
- **`var+n` must stay inside the variable.** `var+1` on a `ZA_AUTO1`, or `table+16` on a `ZA_AUTO 16`,
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
- **`STA arr,X` proves nothing.** An indexed store cannot say which byte it wrote, so an array rebuilt
  only that way looks permanently live. `ZA_DISCARD` it where the old value dies.
- **Mark your ways in.** One `ZA_ENTRY` anywhere and Baron stops assuming sections are entered at the
  top - your markers become the complete list. Interrupt handlers always need their `ZA_INTERRUPT`.
  The unreachable warning follows calls, branches and declared `ZA_CANJUMP`/`ZA_CANCALL` targets, so a
  routine reached only through a computed call may need the `ZA_CANCALL` before it goes quiet.
- **Variables shared with the outside get fixed addresses.** `?&70=X% : CALL blit` needs `&70` to stay
  put, and an auto-allocated address moves whenever the code changes.
- **A write nobody reads is thrown away.** If nothing in the program consumes a value, its byte is up
  for reuse - and "read back by BASIC afterwards" counts as nobody. Another reason shared variables
  live at fixed addresses.

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
- Reach for `ZA_UNREACHABLE` on always-taken branches; the pruned path frees whatever was only needed
  there.

If you run out of bytes, Baron names the variable it could not place - usually the fix is a bigger
`ZA_POOL`, or noticing that something is being kept around longer than it really needs to be.

## Errors and warnings ##

Every error here is a refusal - Baron will not emit code it cannot vouch for. Fix it, annotate it, or
fall back to a hand-placed address.

| Message | What happened |
|---------|---------------|
| `ZA_POOL address outside the zero page` | The pool can only contain bytes &00-&FF. |
| `ZA_AUTO needs a prior ZA_POOL` | A variable with no pool reserved. Add a `ZA_POOL` first. |
| `ZA_AUTO count must be 1 to 256` | A generic `ZA_AUTO n` with a count outside 1-256. |
| `Cannot use a ZA_AUTO address here: '...'` | A variable's address used somewhere that needs a real number during assembly - a condition, a count, an `org`, an annotation target. See [The address of a variable](#the-address-of-a-variable). |
| `ZA_AUTO variable cannot be named 'A'` | The accumulator clash. Rename it. |
| `No free zero-page byte for ZA_AUTO variable: '...'` | More variables needed at once than the pool has bytes. Reserve more, or shorten a lifetime. |
| `ZA_AUTO variable live across an unanalysable JSR (annotate with ZA_CANCALL)` | A variable held across a call whose destination Baron cannot follow. |
| `Computed jump reaches unknown code (annotate with ZA_CANJUMP)` | A jump table or indirect `JMP` the analysis cannot follow. |
| `ZA_AUTO variable freshly written and held live across recursion` | A per-level value in a call cycle - one byte cannot hold a value per level. |
| `ZA_AUTO1 dereferenced as a pointer (declare it ZA_AUTO2)` | `(var),Y` on a one-byte variable. |
| `Access past the end of ZA_AUTO variable` | A `var+n` offset outside the declared width. Widen it or fix the offset. |
| `ZA_DISCARD needs a whole ZA_AUTO variable: '...'` | The operand was a number, a fixed address, or a `var+n` slice. Name a `ZA_AUTO` variable, whole. |
| `ZA_ENTRY/ZA_INTERRUPT does not mark an instruction` | The marker sits on data, or after the last instruction of its section. Move it to the top of its routine. |

And five warnings:

| Message | Level | What happened |
|---------|-------|---------------|
| `ZA_RETURNTO target does not begin an assembled instruction` | default | The declared resumption point matches no code Baron assembled - usually a mistyped label, or an address inside the inline data. That arm gets no edge; if it genuinely names an OS address, the warning is yours to wave through. |
| `Unused ZA_AUTO variable: '...'` | default | No instruction touches it, so it gets no address and **no definition** - referencing it is an error, exactly as if the declaration were not there. Use it or remove it. |
| `Unchecked indexed access into ZA_AUTO variable: '...'` | opt-in | An indexed access (`var,X`, `var,Y`, `(var,X)`) - allowed, but the run-time index is yours to keep in range. |
| `ZA_AUTO used in code unreachable from any entry (missing ZA_ENTRY/ZA_INTERRUPT, or dead code)` | default | Nothing can reach this code from any entry. Usually a handler or a BASIC-called routine missing its marker; sometimes dead code; occasionally a routine behind a computed call that wants a `ZA_CANCALL`. |
| `ZA_AUTO input to a ZA_ENTRY routine: '...' (external callers cannot know its address)` | default | A `ZA_ENTRY` routine reads the variable before writing it on some path, so it expects its caller to have set the value - and an outside caller cannot know an auto-allocated address. Give the interface a fixed home (see `ZA_ENTRY` above), or initialise the variable before the first read. |
