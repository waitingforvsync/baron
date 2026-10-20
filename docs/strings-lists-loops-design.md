# Strings, lists and loops - design discussion

A running record of the design discussion that started from tornewuff's issues
[#25](https://github.com/waitingforvsync/baron/issues/25) and
[#28](https://github.com/waitingforvsync/baron/issues/28). Settled points go under
[Decisions](#decisions), unsettled ones under [Open questions](#open-questions), and the
[Discussion log](#discussion-log) at the end records how we got here.

Started 2026-10-20.


## The goal

Three requests from tornewuff:

- **Joining strings** (#25). Today a list of strings joins as `CHR(FLATTEN(CODES(l)))`. Since `+`
  already joins two strings, `SUM` could join a list of them.
- **Replacing substrings** (#28), and runs of bytes in lists too, for section transforms. A `FIND`
  miss is always an error, so even writing a replace by hand is hard.
- **Loops and reassignment in FUNCTION bodies** (#28). A body binds each name once and loops by
  recursion. Reassigning locals inside a loop leaves the function just as pure, and is easier to
  write than the recursion it replaces.

And one from the user: strings sometimes act as single values and sometimes as lists, and some
functions map over a list while others take it whole. Neither rule is written down, so the
behaviour looks arbitrary. The aim is one stated rule for each, with the gaps filled.

The constraint: no existing program may change what it means. Everything proposed here is either an
error today or new.


## Decisions

1. **The two rules are adopted** (round 2): a string is one value, with a few sequence functions
   that look inside it ([section 2](#2-the-string-rule)); operators and element-wise functions map,
   and everything else takes its arguments whole ([section 3](#3-the-broadcasting-rule)). The guide
   explains both.
2. **Sections 4 and 5 are accepted as proposed:** `SUM` joins strings, `MIN`/`MAX`/`SORT` take
   strings, `REPLACE`, `FIND`'s fallback, the body `IF` error, and loops with reassignment in bodies.
3. **The name is `REPLACE`.** Baron is pre-release, so breaking a program that defines
   `FUNCTION replace` (or `FUNCTION where`, from decision 5) is acceptable; the release notes list it
   under "Breaking changes". User FUNCTIONs don't get to hide builtins.
4. **No `WHILE` or `BREAK`** for now.
5. **`WHERE(mask, a, b)` will be provided** - see [4.7](#47-where).
6. **Step 1 is built** (round 3): `SUM` joins strings, `MIN`/`MAX`/`SORT` take strings, a body `IF`
   refuses a list, and the reference states both rules.
7. **Steps 2 and 3 are built** (round 4): `REPLACE`, the `FIND` fallback, `WHERE`, reassignment and
   `FOR ... NEXT` in bodies. The whole design is in.


## 1. How strings behave today

Probed with the current build:

| Expression | Result | The string is |
|---|---|---|
| `{"a", "b"} + "!"` | `{"a!", "b!"}` | one value |
| `{"ab", "cd"} = "cd"` | `{FALSE, TRUE}` | one value |
| `CONCAT("ab", "cd")` | `{"ab", "cd"}` | one value |
| `FLATTEN({"ab", {"c"}})` | `{"ab", "c"}` | one value |
| `RANK("abc")`, `SHAPE("abc")` | `0`, `{}` | one value |
| `CODES({"ab", "c"})` | `{{97, 98}, {99}}` | one value |
| `FOR c = "abc"` | error: FOR needs a list or a range | one value |
| `LEN("abc")` | `3` | characters |
| `"abc"[1]`, `{"ab", "cd"}[.., 0]` | `"b"`, `{"a", "c"}` | characters |
| `REVERSE("abc")` | `"cba"` | characters |
| `FIND("hello", "ll")` | `2` | characters |
| `EQUB "AB"` | `41 42` | characters |

And the gaps:

| Expression | Result |
|---|---|
| `SUM({"ab", "cd"})` | error: Incompatible types |
| `MIN({"b", "a"})`, `MAX({"b", "a"})` | error: Incompatible types |
| `SORT({"b", "a"})` | error: Incompatible types |
| `FIND("hello", "x")` | error: Not found: 'x' - there's no way to ask without failing |

So the split isn't arbitrary. Everything that builds lists, maps over them or measures their shape
sees a string as one value. The handful of functions that work along a single sequence see its
characters. It has just never been said.


## 2. The string rule

> A string is a single value, like a number. A few functions work along a sequence: subscripts,
> `LEN`, `REVERSE`, `FIND`, `REPLACE` (section 4.3) and `LZ_PARSE`'s data. Given a string, they read
> its characters and give back a string; given a list, they read its elements and give back a list.

Python and NumPy draw the line in the same place: `len("abc")` is 3 but `numpy.ndim("abc")` is 0,
and `"ll" in "hello"` searches for a substring while `[1, 2] in xs` looks for an element.

The rule explains the rest of today's behaviour:

- **`+` joins two strings.** A string is one value, and adding two pieces of text means joining
  them. A list is many values, so `+` on lists adds the values inside; lists join with `CONCAT`.
- **`RANK("abc")` is 0, yet `LEN("abc")` is 3.** Rank describes list structure, and a string has
  none. `LEN` is a sequence function, so it looks inside.
- **`FOR` refuses a string.** To walk a string's characters, write `FOR c = CODES(s)`.
- **`CODES` maps, `CHR` doesn't.** `CODES` turns one string into a list of codes, so given a list
  of strings it gives one list per string. `CHR` takes its argument whole and makes one string of
  every code in it. They are inverses on a single string.
- **The data directives write a string's characters.** `EQUB "AB"` emits two bytes. That isn't the
  rule bending: emitting is what the directive is for.

There's no character type: one character is a one-character string. That's why `FIND` on a string
searches for a substring - a single character is just the shortest substring there is.


## 3. The broadcasting rule

> Operators and the element-wise functions act on single values, so given a list they act on every
> single value inside it, all the way down. Every other function takes its arguments whole.

Today's builtins sort as follows.

- **Element-wise:** every operator, and `ABS`, `LO`, `HI`, `INT`/`FLOOR`, `ROUND`, `TRUNC`, `CEIL`,
  `SQRT`, the trigonometry and logarithms, `NOT`, `RND`, `CODES`, `AS_STRING`, `AS_NUMBER` and
  `BITLEN`.
- **Whole:** `LEN`, `SHAPE`, `RANK`, `REPEATED`/`FULL`, `FLATTEN`, `CONCAT`, `ZIP`, `REVERSE`,
  `SORT`, `SUM`, `PRODUCT`, `MIN`, `MAX`, `DEFINED`, `CHR`, `FIND`, `IS_STRING`, `IS_NUMBER`,
  `ERROR`, `BITPACK` and `LZ_PARSE` - and every user `FUNCTION`.

One argument breaks the pattern: `FIND`'s needle maps, so `FIND(CODES(glyphs), CODES(s))` looks up
every code at once. That's the character-map idiom and it stays, but the reference should call it
out as the exception.

### 3.1 FUNCTIONs take their arguments whole

The guide says user FUNCTIONs "inherit all of it", because `sqr(0..15)` is a 16-element list. But a
FUNCTION receives the list as one argument; `sqr` maps only because `x*x` does. Give the body an
`IF` and the mapping stops, with a misleading error:

```
FUNCTION clamp(x)
    IF x > 255
        r = 255
    ELSE
        r = x
    ENDIF
= r

PRINT clamp({1, 300})           ; error: Undefined symbol: 'r'
```

A body `IF` with a list condition runs neither arm, so `r` is never bound, and the return gets the
blame. The statement `IF` and the body `ASSERT` both say "Operand is not a number" instead. See
[4.5](#45-a-body-if-refuses-a-list).

The guide now says what's really going on (round 2): a FUNCTION takes its arguments as they are, and
maps over a list only when everything in its body maps.

### 3.2 Choosing explicitly

tornewuff asked for a way to say when something should map. There are two directions:

- **Map a whole function over a list** - `LEN` of each string, say, or a FUNCTION with an `IF`
  applied to each element. That needs a loop: see
  [section 5](#5-loops-and-reassignment-in-function-bodies).
- **Stop an element-wise function mapping.** Not needed: those functions only make sense on single
  values.

`WHERE(mask, a, b)`, deferred earlier, is the element-wise `IF`, letting `clamp` map:
`FUNCTION clamp(x) = WHERE(x > 255, 255, x)`. Adopted in round 2; see [4.7](#47-where).


## 4. Filling the gaps

### 4.1 SUM joins strings

`SUM` is repeated `+`, and `+` joins two strings, so `SUM` joins a list of them:

```
line1 = SUM({line1_start, metadata.copyright_year, line1_middle,
             version_prefix, metadata.version_str, version_suffix})
```

That's #25's example, and the braces let it run over several lines.

- Nested lists flatten first, as they do now: `SUM({"a", {"b", "c"}})` is `"abc"`.
- With an axis, it joins along that axis: `SUM({{"a", "b"}, {"c", "d"}}, 1)` is `{"ab", "cd"}`.
- `SUM` starts from `""` instead of 0 when the first value it meets is a string. So `SUM({})` stays
  0, and `SUM({TRUE})` stays 1.
- Mixing strings and numbers is an error, just as `"a" + 1` is. `AS_STRING` converts.

`PRODUCT` stays numbers only.

### 4.2 MIN, MAX and SORT take strings

Comparisons already order strings (`"ab" < "b"` is `TRUE`), so these follow:

```
MIN({"pear", "apple"})          ; "apple"
SORT({"pear", "apple"})         ; {"apple", "pear"}
SORT(entries, 0)                ; entries sorted on the name each one starts with
```

The keys must all be the same kind: all numbers or all strings. A mixture is an error, as `"a" < 1`
is.

### 4.3 REPLACE

`REPLACE(x, old, new)` replaces every occurrence of `old` in `x` with `new`. It works left to right,
never overlapping, and never looks inside what it has just put in.

**In a string**, `old` and `new` are strings, and an occurrence is a substring:

```
REPLACE("1.2.3", ".", "-")      ; "1-2-3"
REPLACE("aaa", "aa", "b")       ; "ba"
```

**In a list** (or a range), an occurrence is a run of elements. `old` and `new` are read the way
`CONCAT` reads its arguments: a list gives its elements, and any other value is one element.

```
REPLACE(bytes, &0D, {&0D, &0A}) ; CR becomes CR LF
REPLACE(bytes, {0, 0}, {})      ; every pair of zeros deleted
REPLACE(names, "tmp", "t")      ; whole elements of a list of strings
REPLACE(rows, {{1, 2}}, 0)      ; wrap a list to mean one list-valued element
```

Elements compare as `FIND` compares them (a label by its address).

- No occurrence leaves `x` unchanged. That isn't an error: "replace every X" holds when there are
  none.
- An empty `old` is an error, since it would occur everywhere.
- A string `x` with a non-string `old` or `new` is an error, as is an `x` that is neither a string
  nor a list.

**`REPLACE` and `FIND` differ on lists.** `FIND`'s needle maps, so `FIND({1, 2, 3, 4}, {2, 3})` is
`{1, 2}`: two lookups. `REPLACE` rewrites runs, so `REPLACE({1, 2, 3, 4}, {2, 3}, 9)` is
`{1, 9, 4}`. On strings the two agree. Both reference entries should say so.

`REPLACE` takes `x` whole, like every sequence function. So `REPLACE({"a.b", "c.d"}, ".", "-")`
looks for an element equal to `"."`, finds none, and changes nothing. Replacing inside each string
of a list is a loop (section 5).

### 4.4 FIND with a fallback

`FIND(x, v, otherwise)` returns `otherwise` instead of failing with "Not found". The two-argument
form is unchanged.

```
IF FIND(name, ".", -1) >= 0                     ; does name contain a dot?
...
EQUS FIND(CODES(glyphs), CODES(s), 0)           ; characters missing from the font use glyph 0
```

With a mapping needle, each miss gets `otherwise`. `otherwise` itself is taken whole.

### 4.5 A body IF refuses a list

A body `IF` or `ELIF` whose condition isn't a number, a boolean or an error makes the call's result
an error: "Operand is not a number", reported at the condition. That matches the statement `IF` and
the body `ASSERT`. `clamp({1, 300})` then fails at its `IF`, not at an innocent return.

### 4.6 Not proposed

- **`CONCAT` of strings giving a string.** It would change what existing programs mean, and
  `CONCAT` always builds a list.
- **Changes to `CHR`.** Joining everything is consistent with the rule (it takes its argument
  whole), and `SUM` now covers the string join: `CHR(FLATTEN(CODES(l)))` becomes `SUM(l)`.
- **`SPLIT` and `JOIN` with a separator.** Easy to write once bodies can loop. Worth adding when
  someone asks.
- **`+` between a string and a number.** Stays an error; `AS_STRING` says which format.

### 4.7 WHERE

`WHERE(mask, a, b)` is the element-wise `IF`. For each value in `mask`, it gives the matching value
of `a` where the mask is true and of `b` where it's false. The three arguments broadcast together
under the usual rules, so any of them can be a single value.

```
FUNCTION clamp(x) = WHERE(x > 255, 255, x)

clamp({1, 300})                         ; {1, 255}
WHERE(bytes > 127, bytes - 256, bytes)  ; bytes as signed numbers
WHERE({TRUE, FALSE}, "on", "off")       ; {"on", "off"}
```

- A mask value is a boolean or a number, non-zero meaning true, as for `IF`. Anything else is an
  error.
- `WHERE` only looks at the values it picks. An error in a value it doesn't pick - a division by
  zero, or a name not yet defined - doesn't matter, just as nothing in an `IF` arm that isn't taken
  does. So `WHERE(x = 0, 0, 1/x)` is safe, whether `x` is a list or a single number. An error in the
  mask, or in a value it picks, is the result.
- It maps, so it belongs in the reference's element-wise table, and it's how a FUNCTION with a choice
  in it can still map over a list (3.1).


## 5. Loops and reassignment in FUNCTION bodies

### 5.1 Purity is about the function

A FUNCTION is pure: the same arguments always give the same result, nothing is emitted, and nothing
outside the call changes. Assigning a local twice inside one call touches none of that, because the
locals are gone when the call returns. tornewuff's point is that reassignment plus a loop has the
same power as tail recursion, in a shape that's easier to write.

Today a body binds each name once. The guide puts it as "`x = x + 1` is asking for a loop that does
not exist - recursion is the loop":

```
FUNCTION f(x)
    y = 1
    y = 2                       ; error: Duplicate symbol: 'y'
= y
```

### 5.2 The proposal

- **Reassignment.** Any local, parameters included, may be assigned again. An assignment only ever
  reaches the call's own names: assigning the name of a global makes a local that hides it, as it
  does now. A local can still only be read once something has assigned it.
- **`FOR name = seq ... NEXT`**, in the statement's own form. `seq` is a list or a bounded range,
  evaluated once before the first step. A string is refused, as the statement refuses it.
- **The loop variable and anything assigned in the loop are ordinary locals.** They keep their last
  values after `NEXT`.
- **The `=` return stays top-level only**, not inside a `FOR` any more than inside an `IF`.
- **No `WHILE` or `BREAK`.** A `FOR` over a known sequence always finishes, so there's no runaway to
  guard against. Its length is capped by the list limit (65536 elements), as the statement's is.
- **An empty sequence runs nothing.** A dead branch, and the definition scan, walk the loop once
  without running it, so its syntax is checked either way.
- **A sequence in error is the call's result**, as an `IF` condition in error is now. A forward
  reference still defers.

### 5.3 Examples

Map a whole function over a list, here to make a length-prefixed command table:

```
FUNCTION commands(names)
    out = {}
    FOR s = names
        out = CONCAT(out, LEN(s), CODES(s))
    NEXT
= out

EQUB commands({"LOAD", "SAVE", "RUN"})
```

Carry running state - each record's offset, from the lengths of the ones before it:

```
FUNCTION offsets(lengths)
    out = {}
    at = 0
    FOR n = lengths
        out = CONCAT(out, at)
        at = at + n
    NEXT
= out

EQUW records + offsets({5, 3, 8})       ; records, records+5, records+8
```

Filter:

```
FUNCTION evens(xs)
    out = {}
    FOR x = xs
        IF x MOD 2 = 0
            out = CONCAT(out, x)
        ENDIF
    NEXT
= out
```

Replace inside each string of a list, which `REPLACE` alone won't do (4.3):

```
FUNCTION replace_each(strings, old, new)
    out = {}
    FOR s = strings
        out = CONCAT(out, REPLACE(s, old, new))
    NEXT
= out
```

### 5.4 What it replaces

- **The comprehension.** Round 10 of the
  [section transform discussion](section-transform-design.md#discussion-log) reverted
  `{expr FOR x = seq}`: the braces read as wrapping a list, the binding should come first, and the
  change to the expression parser was too big. A body loop has none of those problems. The binding
  comes first, the syntax is the `FOR` everyone knows, and the expression parser doesn't change. The
  comprehension can be dropped for good.
- **Function values.** Map, filter and running state (`SCAN`, or the "comprehension with an
  accumulator" in 5.11 of that doc) all come from a loop. There's still no need for `MAP(f, xs)`.
- **An `EACH` marker.** Round 10 rejected it as less general than a comprehension; a loop is more
  general again.
- **Halving recursion for long walks.** A loop has no depth limit.

### 5.5 Memory

A call frees its working memory when it returns: the body works in the spare arena, and its result
is copied back. A loop needs the same, step by step. Otherwise `out = CONCAT(out, x)` keeps every
intermediate `out` until the call returns: 2000 steps growing a list would hold about 80 MB.

The plan for each step:

1. The step works in a fresh copy of the spare arena, so its temporaries die when the next step
   starts.
2. At the end of the step, the values the loop has assigned are copied to just past the step's own
   data. That's still in the spare arena, so nothing they point at is disturbed.
3. The call's arena is rewound to where it stood when the loop began, and the values are copied back
   into it.

```
call's arena:  [ frame, seq, ... | carried values ]      rewound to | every step
                                 ^ where the loop began
spare arena:   [ this step's temporaries | staged copy ]  emptied every step
```

Memory then stays at what the loop carries now, not at everything it has carried. The cost is two
copies per step of whatever the loop has assigned. A list grown with `CONCAT` is already copied once
per step, so the time to build n elements grows with n squared: fine for hundreds or a few thousand
elements. An in-place append could come later if transforms need it.

Measured once built (round 4), growing a list one element per step: 2000 steps 0.02 s and 3.7 MB,
8000 steps 0.25 s and 4.9 MB, 20000 steps 1.5 s and 7.3 MB - memory linear, time quadratic.

Nested loops follow: an inner loop treats the outer step's arena as the one it rewinds.

One catch for the implementation: a local first assigned inside the loop grows the frame. The frame
must grow in the call's arena below the rewind point, or be staged and copied back with the values,
or the rewind would free it.

### 5.6 Costs and risks

- **`for` and `next` become keywords in bodies.** A body local named exactly `for` or `next` would
  need renaming (`next_x` is fine - a longer name wins). Both are already keywords at statement
  level, so few programs will have one.
- **Growing a list is quadratic**, as above.
- **An error in a step is reported at its line in the body**, with the usual "Called from here"
  note; nothing says which step. An `ASSERT` naming the loop variable in its message is the way to
  find out.


## 6. Implementation notes

All in `src/expression.c` unless noted.

- **`SUM`:** `reduce` takes `""` as the identity when the argument's first leaf is a string. The
  plain-number fast path in `fold` is untouched; a run of strings should join into one buffer rather
  than through repeated `+`, which would copy the growing string every time.
- **`MIN` / `MAX`:** `op_min` and `op_max` order through `compare_values`.
- **`SORT`:** `sort_pair` holds its key as a value and compares through `compare_values`. A mixture
  of key kinds is a type mismatch, caught as the pairs are built.
- **`REPLACE`:** a new whole function, `fn_replace`, beside `fn_find`. The string search reuses
  `find_one`'s substring loop, and list elements compare as `find_one` compares them.
- **`FIND`:** `fn_find` takes 2 or 3 arguments, and `find_one` returns the fallback where it now
  returns `not_found`.
- **`WHERE`:** `fn_where`, entered in the table as a whole function so it receives its arguments
  raw and can ignore errors it doesn't pick, with its own three-way broadcast following
  `apply_binary`'s rules.
- **Body `IF`:** `interpret_if` turns a live condition that is neither a number nor an error into
  `error_type_operand_not_numeric`, as `interpret_assert` does.
- **Reassignment:** `interpret_assignment` overwrites the slot `frame_find` returns instead of
  reporting `duplicate_symbol`.
- **`FOR`:** `for` and `next` join `function_body_entries`, with a new `interpret_for` modelled on
  `interpret_if` (a `body_stop_next` beside `body_stop_endif`) and the step arenas of 5.5.
- **Docs:** the guide's "Broadcasting", "Strings and character codes" and "Functions" sections
  explain the two rules already (round 2); each feature adds its own text there as it lands. The
  reference still needs the rules in "Values" and "Broadcasting", the new rows in the function
  tables, and `FIND`'s exception.
- **Release notes:** under "Breaking changes":
  - `REPLACE` and `WHERE` are builtins, so a user FUNCTION of either name stops assembling.
  - `FOR` and `NEXT` are keywords in a body, so a local named `for` or `next` stops assembling (the
    compress examples' `next` became `refit`).
  - A body `IF` given a list used to run neither branch quietly; it's now an error. A body whose
    return didn't need either branch used to get away with it.
  - Not breaking, but new: a second assignment to a body local, once an error, now reassigns it.
- **Tests:** each row of the tables in section 1 becomes a pinned test, since together they're the
  rule's evidence. Every new behaviour gets its own.


## 7. Suggested order

1. **`SUM`, `MIN`/`MAX`/`SORT` and the body `IF` error**, with the two rules in the reference (the
   guide has them already). Small, and they answer #25.
2. **`REPLACE`, the `FIND` fallback and `WHERE`.** The first two answer #28's first request.
3. **Loops and reassignment.** The biggest piece, and #28's second request.


## Open questions

1. **`SPLIT` and `JOIN`**, when someone needs them.
2. **`SORT` of a string** (`SORT("pear")` as `"aepr"`)? Under the rule `SORT` is a list function, so
   no, unless a use turns up.
3. **`WHILE` and `BREAK`**, if a need ever turns up (decision 4 leaves them out for now). A `WHILE`
   would need a step limit, like the recursion limit.


## Discussion log

**Round 1 (2026-10-20).** Read tornewuff's comments on #25 and #28, and probed the current build for
how strings and lists behave (section 1). The split turns out to be consistent, just unwritten, and
every gap is an error today. Proposed the two rules, `SUM` joining strings, strings in
`MIN`/`MAX`/`SORT`, `REPLACE`, `FIND`'s fallback, the body `IF` error, and loops with reassignment
in bodies. A first sketch of the loop's memory (5.5) assumed a third arena; staging the carried
values past the step's own data manages with the two that exist.

**Round 2 (2026-10-20).** The user accepted the proposals and settled the open questions: the name
is `REPLACE` (pre-release, so the clash with a user `FUNCTION replace` goes in the release notes'
breaking changes, rather than letting user FUNCTIONs hide builtins); no `WHILE` or `BREAK` for now;
`WHERE` is to be provided (4.7, where an error in a value it doesn't pick is ignored, as in an `IF`
arm not taken). The guide gained the two rules: "What maps, and what doesn't" and "Functions take
their arguments whole" under Broadcasting, and "A string is one value", "Looking inside a string"
and "Character codes" under Strings. It documents only what exists, so the new functions join it as
they're built.

**Round 3 (2026-10-20).** Built step 1. `SUM` picks `""` as its starting value when the first value
it meets is a string, and `fold` joins a run of strings into one buffer. `op_min`/`op_max` order two
strings with `rc_str_compare`. `SORT` takes its key kind from the first key and sorts strings with a
second instantiation of the sort template. A live body `IF` or `ELIF` whose condition isn't a number
or boolean is `operand_not_numeric` at the condition. Tests pin the string rule (section 1's tables)
and each new behaviour; all five examples assemble byte-identical. The reference's String row and
Broadcasting section now state the two rules, and the guide joins strings with `SUM`.

**Round 4 (2026-10-20).** Built steps 2 and 3. `FIND` takes an optional fallback (`find_one` and a
`missed` helper; it matters only on a miss, so `ERROR("...")` there is a custom message). `REPLACE` and
`FIND` share `same_element` (a label by its address). `WHERE` is a whole-value builtin with its own
three-way broadcast (`where_apply`), so it sees errors raw and drops the ones it doesn't pick. In
bodies, `frame_assign` gives a name a value, new or not, and stamps the slot with the frame's assignment count;
`interpret_for` runs each step in a fresh copy of the spare arena and `carry_locals` stages the slots
assigned since the loop began, rewinds the call's arena and copies them back. A step's error is copied
out of the step's memory before it is returned. A missing `NEXT` is reported at the `FOR`, and stray
closers by name, at the body's top level too. Two general speed-ups came along: `value_make_copy`
skips elements with nothing to copy, and `CONCAT` appends a list in one go (3x on loop growth). The
compress examples used `next` as a local; renamed `refit`, all five examples byte-identical.
