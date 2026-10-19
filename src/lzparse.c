#include "lzparse.h"

#include "richc/array/i64.h"
#include "richc/array/u32.h"
#include "richc/macros.h"


// The search, in brief (docs/section-transform-design.md, section 7 has it at length). Positions run
// left to right. Each keeps two prices: lit_end, the cheapest way to produce that many bytes ending with
// a literal run (matches must come next), and match_end, ending with a match run (literals next; the
// start counts as this). A literal run is one move, its count included, since every literal's cost is
// fixed. A match run is built match by match, so its count travels with it as state: at each position,
// a short "staircase" of partial runs, (matches so far, cost so far), header still unpaid.
//
// Repeats make the offset in hand - the last match's - state as well. Offsets have no order to prune by
// (which one is worth holding depends on the bytes still to come), so it is kept per offset: a queue of
// the closed match runs that left it in hand, and one of the literal runs after them. Only a run's last
// match decides what is in hand, so the matches before it can still take the nearest offset.

static const int64_t lz_impossible = INT64_C(1) << 40;   // above any real total; sums of a few stay in range


// The cost of writing value i from table t, or -1 when it cannot be written.
static int64_t table_cost(rc_view_i32 t, uint32_t i)
{
    if (i >= t.num) {
        return -1;
    }
    int32_t c = rc_view_i32_get(t, i);
    return c < 0 ? -1 : c;
}

// The largest value table t has an entry for.
static uint32_t table_top(rc_view_i32 t)
{
    return t.num == 0 ? 0 : t.num - 1;
}

static rc_span_u32 u32_filled(uint32_t num, uint32_t v, rc_arena *arena)
{
    rc_span_u32 s = rc_array_u32_resize(&(rc_array_u32) {0}, num, arena);
    for (uint32_t i = 0; i < num; i++) {
        rc_span_u32_set(s, i, v);
    }
    return s;
}

static rc_span_i64 i64_filled(uint32_t num, int64_t v, rc_arena *arena)
{
    rc_span_i64 s = rc_array_i64_resize(&(rc_array_i64) {0}, num, arena);
    for (uint32_t i = 0; i < num; i++) {
        rc_span_i64_set(s, i, v);
    }
    return s;
}

// Prefix sums of literal costs, so a literal run [i, j) costs sums[j] - sums[i] plus its header. A byte
// that cannot be a literal costs "impossible", which no real total reaches.
static rc_span_i64 literal_sums(rc_view_bytes data, rc_view_i32 literal, rc_arena *arena)
{
    rc_span_i64 sums = rc_array_i64_resize(&(rc_array_i64) {0}, data.num + 1, arena);
    rc_span_i64_set(sums, 0, 0);
    for (uint32_t i = 0; i < data.num; i++) {
        int64_t c = table_cost(literal, rc_view_bytes_get(data, i));
        rc_span_i64_set(sums, i + 1, rc_span_i64_get(sums, i) + (c < 0 ? lz_impossible : c));
    }
    return sums;
}


// One step of a parse, remembered so the winner can be walked back from the end: a match (a partial
// match run on a staircase, k matches costing `cost` with the header unpaid, or the last match of a
// closed run), or - for repeats - a literal run. `prev` is, for a match, the match before it in its run,
// the literal run its run opened after, or RC_INDEX_NONE when its run opened after the cheapest literal
// run to `from`; for a literal run, the last match of the run before it.
typedef struct lz_entry {
    int64_t  cost;
    uint32_t k;
    uint32_t prev;
    uint32_t from;     // where the match, or the literal run, starts
    uint32_t length;   // the match's length, or the literal run's count
    uint32_t offset;   // 0 for a repeat
    uint32_t next;     // the next entry on the same staircase (or on the free list)
    uint32_t refs;     // how many things point here: its staircase, later entries, the queues
    bool     literal;
} lz_entry;

#define RC_ARRAY_TYPE lz_entry
#define RC_ARRAY_NAME lz_entry
#include "richc/template/array.h"

// Every entry lives in one pool, counted by what points at it, and goes back on the free list once
// nothing does: an entry pushed off a staircase before anything extended it, or a record no queue or
// later step remembers.
typedef struct lz_pool {
    rc_array_lz_entry entries;
    rc_span_u32       stairs;   // per position: its staircase's first entry, or RC_INDEX_NONE
    uint32_t          free;
} lz_pool;

static uint32_t pool_add(lz_pool *pool, lz_entry e, rc_arena *arena)
{
    e.refs = 0;
    if (e.prev != RC_INDEX_NONE) {
        rc_array_lz_entry_at(&pool->entries, e.prev)->refs++;
    }
    if (pool->free != RC_INDEX_NONE) {
        uint32_t i = pool->free;
        pool->free = rc_array_lz_entry_get(&pool->entries, i).next;
        rc_array_lz_entry_set(&pool->entries, i, e);
        return i;
    }
    return rc_array_lz_entry_push(&pool->entries, e, arena);
}

static void pool_hold(lz_pool *pool, uint32_t i)
{
    rc_array_lz_entry_at(&pool->entries, i)->refs++;
}

// Let go of entry i, and of anything it pointed at that nothing else wants.
static void pool_release(lz_pool *pool, uint32_t i)
{
    while (i != RC_INDEX_NONE) {
        lz_entry *e = rc_array_lz_entry_at(&pool->entries, i);
        if (--e->refs != 0) {
            return;
        }
        uint32_t prev = e->prev;
        e->next    = pool->free;
        pool->free = i;
        i          = prev;
    }
}

// Offer a partial run to position q's staircase. It is refused if one already there has no more
// matches and costs no more (the header never gets cheaper as the count grows, so that one can never
// do worse); any it beats the same way are dropped.
static void stairs_offer(lz_pool *pool, uint32_t q, lz_entry e, rc_arena *arena)
{
    for (uint32_t i = rc_span_u32_get(pool->stairs, q); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&pool->entries, i).next) {
        lz_entry o = rc_array_lz_entry_get(&pool->entries, i);
        if (o.k <= e.k && o.cost <= e.cost) {
            return;
        }
    }

    uint32_t *link = rc_span_u32_at(pool->stairs, q);
    while (*link != RC_INDEX_NONE) {
        lz_entry o = rc_array_lz_entry_get(&pool->entries, *link);
        if (o.k >= e.k && o.cost >= e.cost) {
            uint32_t gone = *link;
            *link = o.next;
            pool_release(pool, gone);
        }
        else {
            link = &rc_array_lz_entry_at(&pool->entries, *link)->next;
        }
    }

    e.next = rc_span_u32_get(pool->stairs, q);
    uint32_t i = pool_add(pool, e, arena);
    pool_hold(pool, i);
    rc_span_u32_set(pool->stairs, q, i);
}


// A band of literal-run counts [lo, hi] sharing one header cost, with a sliding-window minimum over
// the candidate run starts: for a run ending at j, a start i in [j - hi, j - lo].
typedef struct lz_band {
    uint32_t    lo;
    uint32_t    hi;
    int64_t     cost;
    rc_span_u32 deque;   // run starts, best first
    uint32_t    first;
    uint32_t    last;    // one past the back
} lz_band;

#define RC_ARRAY_TYPE lz_band
#define RC_ARRAY_NAME lz_band
#include "richc/template/array.h"

enum { lz_max_bands = 64 };   // beyond this a table is not banded enough to be worth it: try every start

// The literal-run table's bands of equal cost, impossible counts left out. An empty result with more
// than lz_max_bands bands means "scan instead".
static rc_array_lz_band literal_bands(rc_view_i32 table, uint32_t n, rc_arena *arena)
{
    rc_array_lz_band bands = {0};
    uint32_t top = table_top(table) < n ? table_top(table) : n;
    for (uint32_t c = 1; c <= top; c++) {
        int64_t cost = table_cost(table, c);
        if (cost < 0) {
            continue;
        }
        if (bands.num != 0) {
            lz_band *b = rc_array_lz_band_at(&bands, bands.num - 1);
            if (b->hi == c - 1 && b->cost == cost) {
                b->hi = c;
                continue;
            }
        }
        if (bands.num == lz_max_bands) {
            return (rc_array_lz_band) {0};
        }
        rc_array_lz_band_push(&bands, (lz_band) {.lo = c, .hi = c, .cost = cost}, arena);
    }
    for (uint32_t b = 0; b < bands.num; b++) {
        rc_array_lz_band_at(&bands, b)->deque = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena);
    }
    return bands;
}


// A queued record (a closed match run, or a literal run after one) and its price, in a per-offset
// list kept newest first. An older one costing no less is dropped as soon as a newer one arrives: it
// would make a longer literal run (or repeat) for no saving, and runs out of reach sooner.
typedef struct lz_queued {
    int64_t  key;
    uint32_t pos;
    uint32_t entry;
    uint32_t next;    // the next older one (or the free list)
} lz_queued;

#define RC_ARRAY_TYPE lz_queued
#define RC_ARRAY_NAME lz_queued
#include "richc/template/array.h"

// A closed match run ending at the current position, the last match's offset `held` left in hand,
// waiting to see whether it is worth queueing.
typedef struct lz_ending {
    int64_t  cost;
    uint32_t held;
    uint32_t from;     // its last match
    uint32_t offset;
    uint32_t prev;
} lz_ending;

#define RC_ARRAY_TYPE lz_ending
#define RC_ARRAY_NAME lz_ending
#include "richc/template/array.h"

// A repeat that opens a match run at the current position, header unpaid.
typedef struct lz_repeat_start {
    int64_t  cost;
    uint32_t from;
    uint32_t after;   // the literal run it follows
} lz_repeat_start;

// The cost of a match's offset, from the table for its length.
static int64_t offset_cost(rc_view_lz_table offsets, uint32_t length, uint32_t off)
{
    if (offsets.num == 0) {
        return -1;
    }
    return table_cost(rc_view_lz_table_get(offsets, (length < offsets.num ? length : offsets.num) - 1), off);
}

// The furthest offset any table can write: nothing beyond it is worth looking at.
static uint32_t offset_top(rc_view_lz_table offsets)
{
    uint32_t top = 0;
    for (uint32_t t = 0; t < offsets.num; t++) {
        rc_view_i32 table = rc_view_lz_table_get(offsets, t);
        for (uint32_t i = table.num; i-- > top + 1;) {
            if (table_cost(table, i) >= 0) {
                top = i;
                break;
            }
        }
    }
    return top;
}

// Whether a match run costs the same whatever its count, from 1 to n: then every match carries its own
// flag, and ending a run or carrying it on makes no difference.
static bool runs_flat(rc_view_i32 match_run, uint32_t n)
{
    int64_t header = table_cost(match_run, 1);
    if (header < 0) {
        return false;
    }
    for (uint32_t k = 2; k <= n; k++) {
        if (table_cost(match_run, k) != header) {
            return false;
        }
    }
    return true;
}

typedef struct lz_search {
    rc_view_bytes data;
    lz_costs      costs;
    rc_span_i64   lits;
    rc_span_i64   lit_end;
    rc_span_u32   lit_from;
    rc_span_i64   match_end;
    rc_span_u32   match_by;
    lz_pool       pool;
    rc_span_u32   seen;        // per byte value: its latest position so far, or RC_INDEX_NONE
    rc_span_u32   previous;    // per position: the latest one before it holding the same byte
    uint32_t      furthest;    // the furthest offset any table can write

    // The per-offset search: every match when runs are flat, the last of each run for repeats.
    bool               flat;
    bool               repeats;
    rc_span_u32        matched;     // per offset: how many bytes up to here match at that offset
    rc_span_u32        last;        // per offset: one past the last position whose byte matched there
    rc_span_i64        open;        // per position: the cheapest place to write a run's next match from,
    rc_span_u32        open_by;     //   header included, and the entry it extends
    rc_span_i64        reach_cost;  // per length: the cheapest match ending here of at most that length,
    rc_span_u32        reach_len;   //   among those sharing the last offset table (its offset aside)
    uint32_t           reached;     // how far reach_cost is filled in at this position
    rc_array_lz_ending endings;

    // Repeats only.
    rc_span_i64        savings;     // per offset table: the most a repeat it serves saves (see holding_worth)
    rc_array_lz_queued queued;
    uint32_t           queued_free;
    rc_span_u32        runs;        // per offset: closed match runs that left it in hand
    rc_span_u32        literals;    // per offset: literal runs after those, ending in the current stretch
} lz_search;

// How much more than the best at its position a record holding offset off can cost and still be worth
// keeping: the best could always write that offset out in full where the record would repeat it, for at
// most the offset's cost and the table's saving more. lz_impossible: some repeat has no stand-in, so keep
// everything.
static int64_t holding_worth(const lz_search *s, uint32_t off)
{
    int64_t worth = 0;
    for (uint32_t t = 0; t < s->savings.num; t++) {
        int64_t save = rc_span_i64_get(s->savings, t);
        if (save == INT64_MIN) {
            continue;   // this table serves no repeat length
        }
        int64_t ocost = table_cost(rc_view_lz_table_get(s->costs.offset, t), off);
        if (save >= lz_impossible || ocost < 0) {
            return lz_impossible;
        }
        worth = ocost + save > worth ? ocost + save : worth;
    }
    return worth;
}

static void queue_drop(lz_search *s, uint32_t i)
{
    while (i != RC_INDEX_NONE) {
        lz_queued *q    = rc_array_lz_queued_at(&s->queued, i);
        uint32_t   next = q->next;
        pool_release(&s->pool, q->entry);
        q->next        = s->queued_free;
        s->queued_free = i;
        i              = next;
    }
}

// Queue a record at heads[at], dropping the older ones it makes useless.
static void queue_push(lz_search *s, rc_span_u32 heads, uint32_t at, lz_queued item, rc_arena *arena)
{
    uint32_t head = rc_span_u32_get(heads, at);
    while (head != RC_INDEX_NONE && rc_array_lz_queued_get(&s->queued, head).key >= item.key) {
        uint32_t next = rc_array_lz_queued_get(&s->queued, head).next;
        rc_array_lz_queued_at(&s->queued, head)->next = RC_INDEX_NONE;
        queue_drop(s, head);
        head = next;
    }

    item.next = head;
    uint32_t i;
    if (s->queued_free != RC_INDEX_NONE) {
        i = s->queued_free;
        s->queued_free = rc_array_lz_queued_get(&s->queued, i).next;
        rc_array_lz_queued_set(&s->queued, i, item);
    }
    else {
        i = rc_array_lz_queued_push(&s->queued, item, arena);
    }
    pool_hold(&s->pool, item.entry);
    rc_span_u32_set(heads, at, i);
}

// Fill reach_cost in up to length m at position p: the cheapest way to write a match there of at most m
// bytes, from wherever such a match could start - for the lengths that share the last offset table.
static void reach(lz_search *s, uint32_t p, uint32_t m)
{
    uint32_t first = s->costs.offset.num;
    while (s->reached < m) {
        uint32_t l     = ++s->reached;
        int64_t  lcost = table_cost(s->costs.length, l);
        int64_t  start = rc_span_i64_get(s->open, p - l);
        int64_t  c     = lcost >= 0 && start < lz_impossible ? start + lcost : lz_impossible;
        int64_t  best  = l == first ? lz_impossible : rc_span_i64_get(s->reach_cost, l - 1);
        rc_span_i64_set(s->reach_cost, l, c < best ? c : best);
        rc_span_u32_set(s->reach_len, l, c < best ? l : rc_span_u32_get(s->reach_len, l - 1));
    }
}

// Every closed match run ending at p, one per offset whose bytes match just before p, its last match
// either a new one at that offset (any length its bytes allow) or a repeat of it straight after a
// literal run. They wait in endings until match_end[p] is known. Returns the cheapest repeat to open a
// run here, for p's staircase.
static lz_repeat_start end_runs(lz_search *s, uint32_t p, rc_arena *arena)
{
    lz_repeat_start best    = {.cost = lz_impossible};
    uint32_t        tables  = s->costs.offset.num;
    uint32_t        len_max = table_top(s->costs.length);
    uint32_t        rep_max = table_top(s->costs.repeat);
    int64_t         header  = table_cost(s->costs.match_run, 1);
    rc_array_lz_ending_reset(&s->endings);
    s->reached = tables - 1;

    for (uint32_t j = rc_span_u32_get(s->previous, p - 1); j != RC_INDEX_NONE && tables != 0; j = rc_span_u32_get(s->previous, j)) {
        uint32_t off = p - 1 - j;
        if (off > s->furthest) {
            break;
        }
        uint32_t run = p >= 2 && rc_span_u32_get(s->last, off) == p - 1 ? rc_span_u32_get(s->matched, off) + 1 : 1;
        rc_span_u32_set(s->matched, off, run);
        rc_span_u32_set(s->last, off, p);

        // A new match: each length with a table of its own, then the longer ones together.
        lz_ending e = {.cost = lz_impossible};
        uint32_t  m = run < len_max ? run : len_max;
        for (uint32_t l = 1; l < tables && l <= m; l++) {
            int64_t ocost = table_cost(rc_view_lz_table_get(s->costs.offset, l - 1), off);
            int64_t lcost = table_cost(s->costs.length, l);
            int64_t start = rc_span_i64_get(s->open, p - l);
            if (ocost >= 0 && lcost >= 0 && start < lz_impossible && start + lcost + ocost < e.cost) {
                e = (lz_ending) {
                    .cost   = start + lcost + ocost,
                    .held   = off,
                    .from   = p - l,
                    .offset = off,
                    .prev   = rc_span_u32_get(s->open_by, p - l),
                };
            }
        }
        int64_t ocost = table_cost(rc_view_lz_table_get(s->costs.offset, tables - 1), off);
        if (m >= tables && ocost >= 0) {
            reach(s, p, m);
            int64_t c = rc_span_i64_get(s->reach_cost, m);
            if (c < lz_impossible && c + ocost < e.cost) {
                uint32_t from = p - rc_span_u32_get(s->reach_len, m);
                e = (lz_ending) {
                    .cost   = c + ocost,
                    .held   = off,
                    .from   = from,
                    .offset = off,
                    .prev   = rc_span_u32_get(s->open_by, from),
                };
            }
        }

        // A repeat from a literal run in this stretch: the stretch is what makes its bytes match.
        uint32_t *link = s->repeats ? rc_span_u32_at(s->literals, off) : &(uint32_t) {RC_INDEX_NONE};
        while (*link != RC_INDEX_NONE) {
            lz_queued q = rc_array_lz_queued_get(&s->queued, *link);
            if (p - q.pos > rep_max) {
                queue_drop(s, *link);
                *link = RC_INDEX_NONE;
                break;
            }
            int64_t r = table_cost(s->costs.repeat, p - q.pos);
            if (r >= 0) {
                if (header >= 0 && q.key + r + header < e.cost) {
                    e = (lz_ending) {
                        .cost   = q.key + r + header,
                        .held   = off,
                        .from   = q.pos,
                        .offset = 0,
                        .prev   = q.entry,
                    };
                }
                if (q.key + r < best.cost) {
                    best = (lz_repeat_start) {.cost = q.key + r, .from = q.pos, .after = q.entry};
                }
            }
            link = &rc_array_lz_queued_at(&s->queued, *link)->next;
        }

        if (e.cost < lz_impossible) {
            rc_array_lz_ending_push(&s->endings, e, arena);
        }
    }
    return best;
}

// With flat runs, the closed runs ending at p are every way to stand there after a match: the cheapest
// becomes match_end[p], kept as a record for later matches and the walk back to name.
static void close_flat(lz_search *s, uint32_t p, rc_arena *arena)
{
    uint32_t best = RC_INDEX_NONE;
    for (uint32_t i = 0; i < s->endings.num; i++) {
        if (best == RC_INDEX_NONE || rc_array_lz_ending_get(&s->endings, i).cost < rc_array_lz_ending_get(&s->endings, best).cost) {
            best = i;
        }
    }
    if (best == RC_INDEX_NONE) {
        return;
    }
    lz_ending e     = rc_array_lz_ending_get(&s->endings, best);
    uint32_t  entry = pool_add(&s->pool, (lz_entry) {
        .prev   = e.prev,
        .from   = e.from,
        .length = p - e.from,
        .offset = e.offset,
    }, arena);
    pool_hold(&s->pool, entry);
    rc_span_i64_set(s->match_end, p, e.cost);
    rc_span_u32_set(s->match_by, p, entry);
}

// The cheapest place at p to write a run's next match from, its header included: straight after the
// cheapest literal run, or - flat runs - carrying on a run that ended here, or - counted runs - after any
// partial run here.
static void open_runs(lz_search *s, uint32_t p)
{
    int64_t  best  = lz_impossible;
    uint32_t by    = RC_INDEX_NONE;
    int64_t  first = table_cost(s->costs.match_run, 1);
    if (first >= 0 && rc_span_i64_get(s->lit_end, p) < lz_impossible) {
        best = rc_span_i64_get(s->lit_end, p) + first;
    }
    if (s->flat) {
        if (p > 0 && rc_span_i64_get(s->match_end, p) < best) {
            best = rc_span_i64_get(s->match_end, p);
            by   = rc_span_u32_get(s->match_by, p);
        }
    }
    else {
        for (uint32_t i = rc_span_u32_get(s->pool.stairs, p); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&s->pool.entries, i).next) {
            lz_entry e      = rc_array_lz_entry_get(&s->pool.entries, i);
            int64_t  header = table_cost(s->costs.match_run, e.k + 1);
            if (header >= 0 && e.cost + header < best) {
                best = e.cost + header;
                by   = i;
            }
        }
    }
    rc_span_i64_set(s->open, p, best);
    rc_span_u32_set(s->open_by, p, by);
}

// The literal runs ending at p after a closed run that left offset o in hand, for every o a repeat
// could copy from at p. A stretch of bytes matching at o starts afresh where the byte before did not
// match, and the literal runs queued for the old one go with it.
static void end_literals(lz_search *s, uint32_t p, rc_arena *arena)
{
    if (p == s->data.num) {
        return;
    }
    uint32_t lit_max = table_top(s->costs.literal_run);
    for (uint32_t j = rc_span_u32_get(s->seen, rc_view_bytes_get(s->data, p)); j != RC_INDEX_NONE; j = rc_span_u32_get(s->previous, j)) {
        uint32_t off = p - j;
        if (off > s->furthest) {
            break;
        }
        if (rc_span_u32_get(s->last, off) != p) {
            queue_drop(s, rc_span_u32_get(s->literals, off));
            rc_span_u32_set(s->literals, off, RC_INDEX_NONE);
        }

        int64_t   best = lz_impossible;
        lz_queued from = {0};
        uint32_t *link = rc_span_u32_at(s->runs, off);
        while (*link != RC_INDEX_NONE) {
            lz_queued q = rc_array_lz_queued_get(&s->queued, *link);
            if (p - q.pos > lit_max) {
                queue_drop(s, *link);
                *link = RC_INDEX_NONE;
                break;
            }
            int64_t header = table_cost(s->costs.literal_run, p - q.pos);
            if (header >= 0 && q.key + header < best) {
                best = q.key + header;
                from = q;
            }
            link = &rc_array_lz_queued_at(&s->queued, *link)->next;
        }
        if (best >= lz_impossible) {
            continue;
        }
        best += rc_span_i64_get(s->lits, p);
        int64_t worth = holding_worth(s, off);
        if (worth < lz_impossible && best >= rc_span_i64_get(s->lit_end, p) + worth) {
            continue;   // the cheapest literal run here could write this offset out and still win
        }

        uint32_t e = pool_add(&s->pool, (lz_entry) {
            .prev    = from.entry,
            .from    = from.pos,
            .length  = p - from.pos,
            .literal = true,
        }, arena);
        queue_push(s, s->literals, off, (lz_queued) {.key = best, .pos = p, .entry = e}, arena);
    }
}

// Queue each closed run ending at p that could still be worth the offset it holds.
static void keep_endings(lz_search *s, uint32_t p, rc_arena *arena)
{
    int64_t best = rc_span_i64_get(s->match_end, p);
    for (uint32_t i = 0; i < s->endings.num; i++) {
        lz_ending e     = rc_array_lz_ending_get(&s->endings, i);
        int64_t   worth = holding_worth(s, e.held);
        if (worth < lz_impossible && e.cost >= best + worth) {
            continue;
        }
        uint32_t entry = pool_add(&s->pool, (lz_entry) {
            .prev   = e.prev,
            .from   = e.from,
            .length = p - e.from,
            .offset = e.offset,
        }, arena);
        queue_push(s, s->runs, e.held, (lz_queued) {.key = e.cost - rc_span_i64_get(s->lits, p), .pos = p, .entry = entry}, arena);
    }
}


lz_parse_result lz_parse(rc_view_bytes data, lz_costs costs, rc_arena *arena)
{
    uint32_t n       = data.num;
    uint32_t tables  = costs.offset.num;
    bool     flat    = runs_flat(costs.match_run, n);
    bool     repeats = false;

    // Repeats need a length they can be. Per offset table, the most a repeat of a length it serves ever
    // saves over writing that match out in full, the offset's own cost aside (see holding_worth).
    rc_span_i64 savings = i64_filled(tables, INT64_MIN, arena);
    for (uint32_t l = 1; l < costs.repeat.num && tables != 0; l++) {
        int64_t r = table_cost(costs.repeat, l);
        if (r >= 0) {
            uint32_t t = (l < tables ? l : tables) - 1;
            int64_t  c = table_cost(costs.length, l);
            int64_t  d = c < 0 ? lz_impossible : c - r;   // no full match to stand in: no bound
            if (d > rc_span_i64_get(savings, t)) {
                rc_span_i64_set(savings, t, d);
            }
            repeats = true;
        }
    }
    bool     per_offset = flat || repeats;
    uint32_t window     = per_offset ? n + 1 : 0;   // per-offset state, offsets 1..n - 1
    uint32_t held       = repeats ? n + 1 : 0;
    uint32_t lengths    = per_offset ? (table_top(costs.length) < n ? table_top(costs.length) : n) + 1 : 0;

    lz_search s = {
        .data        = data,
        .costs       = costs,
        .lits        = literal_sums(data, costs.literal, arena),
        .lit_end     = i64_filled(n + 1, lz_impossible, arena),
        .lit_from    = u32_filled(n + 1, 0, arena),
        .match_end   = i64_filled(n + 1, lz_impossible, arena),
        .match_by    = u32_filled(n + 1, RC_INDEX_NONE, arena),
        .pool        = {
            .entries = rc_array_lz_entry_make(n + 16, arena),
            .stairs  = u32_filled(n + 1, RC_INDEX_NONE, arena),
            .free    = RC_INDEX_NONE,
        },
        .seen        = u32_filled(256, RC_INDEX_NONE, arena),
        .previous    = u32_filled(n, RC_INDEX_NONE, arena),
        .furthest    = offset_top(costs.offset),
        .flat        = flat,
        .repeats     = repeats,
        .matched     = u32_filled(window, 0, arena),
        .last        = u32_filled(window, 0, arena),
        .open        = i64_filled(window, lz_impossible, arena),
        .open_by     = u32_filled(window, RC_INDEX_NONE, arena),
        .reach_cost  = i64_filled(lengths, lz_impossible, arena),
        .reach_len   = u32_filled(lengths, 0, arena),
        .savings     = repeats ? savings : (rc_span_i64) {0},
        .queued_free = RC_INDEX_NONE,
        .runs        = u32_filled(held, RC_INDEX_NONE, arena),
        .literals    = u32_filled(held, RC_INDEX_NONE, arena),
    };
    rc_span_i64_set(s.match_end, 0, 0);   // the start: literals come first

    rc_array_lz_band bands = literal_bands(costs.literal_run, n, arena);
    bool scan_literals = bands.num == 0;

    // Hash chains over each position's first two bytes, nearest first; best_offset[len] is the nearest
    // offset reaching at least len bytes at the current position, the cheapest for that length.
    rc_span_u32 chain_head  = u32_filled(65536, RC_INDEX_NONE, arena);
    rc_span_u32 chain_next  = u32_filled(n + 1, RC_INDEX_NONE, arena);
    rc_span_u32 best_offset = u32_filled(n + 1, 0, arena);

    for (uint32_t p = 0; p <= n; p++) {
        if (p > 0) {
            uint8_t b = rc_view_bytes_get(data, p - 1);   // p - 1 joins its byte's chain
            rc_span_u32_set(s.previous, p - 1, rc_span_u32_get(s.seen, b));
            rc_span_u32_set(s.seen, b, p - 1);
            if (per_offset) {
                lz_repeat_start r = end_runs(&s, p, arena);
                if (flat) {
                    close_flat(&s, p, arena);
                }
                else if (r.cost < lz_impossible && costs.match_run.num > 1) {
                    stairs_offer(&s.pool, p, (lz_entry) {
                        .cost   = r.cost,
                        .k      = 1,
                        .prev   = r.after,
                        .from   = r.from,
                        .length = p - r.from,
                    }, arena);
                }
            }
        }

        // Close every partial match run that has reached p.
        for (uint32_t i = rc_span_u32_get(s.pool.stairs, p); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&s.pool.entries, i).next) {
            lz_entry e = rc_array_lz_entry_get(&s.pool.entries, i);
            int64_t header = table_cost(costs.match_run, e.k);
            if (header >= 0 && e.cost + header < rc_span_i64_get(s.match_end, p)) {
                rc_span_i64_set(s.match_end, p, e.cost + header);
                rc_span_u32_set(s.match_by, p, i);
            }
        }

        // The best literal run ending at p, from wherever a match run (or the start) left off.
        if (scan_literals) {
            for (uint32_t i = 0; i < p; i++) {
                int64_t header = table_cost(costs.literal_run, p - i);
                int64_t from   = rc_span_i64_get(s.match_end, i);
                if (header < 0 || from >= lz_impossible) {
                    continue;
                }
                int64_t c = from + header + rc_span_i64_get(s.lits, p) - rc_span_i64_get(s.lits, i);
                if (c < rc_span_i64_get(s.lit_end, p)) {
                    rc_span_i64_set(s.lit_end, p, c);
                    rc_span_u32_set(s.lit_from, p, i);
                }
            }
        }
        else {
            for (uint32_t b = 0; b < bands.num; b++) {
                lz_band *band = rc_array_lz_band_at(&bands, b);
                if (p >= band->lo) {
                    uint32_t i = p - band->lo;   // a newly possible start, its match_end already final
                    if (rc_span_i64_get(s.match_end, i) < lz_impossible) {
                        int64_t key = rc_span_i64_get(s.match_end, i) - rc_span_i64_get(s.lits, i);
                        while (band->last > band->first) {
                            uint32_t back = rc_span_u32_get(band->deque, band->last - 1);
                            if (rc_span_i64_get(s.match_end, back) - rc_span_i64_get(s.lits, back) < key) {
                                break;
                            }
                            band->last--;
                        }
                        rc_span_u32_set(band->deque, band->last++, i);
                    }
                }
                while (band->last > band->first && rc_span_u32_get(band->deque, band->first) + band->hi < p) {
                    band->first++;   // that start is now further back than this band's longest run
                }
                if (band->last > band->first) {
                    uint32_t i = rc_span_u32_get(band->deque, band->first);
                    int64_t  c = rc_span_i64_get(s.match_end, i) + band->cost
                               + rc_span_i64_get(s.lits, p) - rc_span_i64_get(s.lits, i);
                    if (c < rc_span_i64_get(s.lit_end, p)) {
                        rc_span_i64_set(s.lit_end, p, c);
                        rc_span_u32_set(s.lit_from, p, i);
                    }
                }
            }
        }

        if (per_offset) {
            open_runs(&s, p);
        }
        if (repeats) {
            end_literals(&s, p, arena);
            keep_endings(&s, p, arena);
        }

        if (flat || p == n) {
            continue;   // flat runs had every match from the per-offset search; or no room for one
        }

        // The matches from p: for each length, the nearest offset that reaches it - the nearest earlier
        // copy of this byte for one byte, the hash chains beyond.
        uint32_t max_len = table_top(costs.length) < n - p ? table_top(costs.length) : n - p;
        uint32_t best    = 0;
        uint32_t near    = rc_span_u32_get(s.seen, rc_view_bytes_get(data, p));
        if (max_len >= 1 && near != RC_INDEX_NONE && p - near <= s.furthest) {
            rc_span_u32_set(best_offset, 1, p - near);
            best = 1;
        }
        if (p + 2 <= n) {
            uint32_t key = rc_view_bytes_get(data, p) | (uint32_t) rc_view_bytes_get(data, p + 1) << 8;
            for (uint32_t q = rc_span_u32_get(chain_head, key); q != RC_INDEX_NONE && best < max_len; q = rc_span_u32_get(chain_next, q)) {
                uint32_t off = p - q;
                if (off > s.furthest) {
                    break;
                }
                uint32_t len = 2;
                while (len < max_len && rc_view_bytes_get(data, q + len) == rc_view_bytes_get(data, p + len)) {
                    len++;
                }
                for (uint32_t l = best + 1; l <= len; l++) {
                    rc_span_u32_set(best_offset, l, off);
                }
                best = len > best ? len : best;
            }
            rc_span_u32_set(chain_next, p, rc_span_u32_get(chain_head, key));
            rc_span_u32_set(chain_head, key, p);
        }

        // Extend every way of standing at p into a match run by each of those matches: a fresh run after
        // a literal run, or one more match on a partial run already here.
        for (uint32_t len = 1; len <= best; len++) {
            uint32_t off   = rc_span_u32_get(best_offset, len);
            int64_t  lcost = table_cost(costs.length, len);
            int64_t  ocost = offset_cost(costs.offset, len, off);
            if (lcost < 0 || ocost < 0) {
                continue;
            }
            int64_t  m = lcost + ocost;
            uint32_t q = p + len;

            if (rc_span_i64_get(s.lit_end, p) < lz_impossible && costs.match_run.num > 1) {
                stairs_offer(&s.pool, q, (lz_entry) {
                    .cost   = rc_span_i64_get(s.lit_end, p) + m,
                    .k      = 1,
                    .prev   = RC_INDEX_NONE,
                    .from   = p,
                    .length = len,
                    .offset = off,
                }, arena);
            }
            for (uint32_t i = rc_span_u32_get(s.pool.stairs, p); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&s.pool.entries, i).next) {
                lz_entry e = rc_array_lz_entry_get(&s.pool.entries, i);
                if (e.k + 1 >= costs.match_run.num) {
                    continue;   // a run this long has no header to close it with
                }
                stairs_offer(&s.pool, q, (lz_entry) {
                    .cost   = e.cost + m,
                    .k      = e.k + 1,
                    .prev   = i,
                    .from   = p,
                    .length = len,
                    .offset = off,
                }, arena);
            }
        }
    }

    int64_t end_lit   = rc_span_i64_get(s.lit_end, n);
    int64_t end_match = rc_span_i64_get(s.match_end, n);
    int64_t total     = end_lit < end_match ? end_lit : end_match;
    if (total >= lz_impossible) {
        return (lz_parse_result) {0};
    }

    // Walk back from the end, collecting runs (and matches) last first, then turn both round. A match
    // run's first match says what came before it: the cheapest literal run to there, or (a repeat) one
    // particular literal run, which in turn names the last match of the run before.
    rc_array_lz_run   runs    = {0};
    rc_array_lz_match matches = {0};
    bool     literal = end_lit < end_match;
    uint32_t pos     = n;
    uint32_t e       = rc_span_u32_get(s.match_by, n);
    while (pos > 0) {
        if (literal) {
            uint32_t i = rc_span_u32_get(s.lit_from, pos);
            rc_array_lz_run_push(&runs, (lz_run) {.is_match = false, .start = i, .count = pos - i}, arena);
            pos     = i;
            e       = rc_span_u32_get(s.match_by, pos);
            literal = false;
            continue;
        }
        uint32_t count = 0;
        uint32_t from  = pos;
        while (e != RC_INDEX_NONE && !rc_array_lz_entry_get(&s.pool.entries, e).literal) {
            lz_entry entry = rc_array_lz_entry_get(&s.pool.entries, e);
            rc_array_lz_match_push(&matches, (lz_match) {.offset = entry.offset, .length = entry.length}, arena);
            count++;
            from = entry.from;
            e    = entry.prev;
        }
        rc_array_lz_run_push(&runs, (lz_run) {.is_match = true, .count = count}, arena);
        pos = from;
        if (e == RC_INDEX_NONE) {
            literal = true;
        }
        else {
            lz_entry lits = rc_array_lz_entry_get(&s.pool.entries, e);
            rc_array_lz_run_push(&runs, (lz_run) {.is_match = false, .start = lits.from, .count = lits.length}, arena);
            pos = lits.from;
            e   = lits.prev;
        }
    }

    rc_span_lz_run_reverse(runs.span);
    rc_span_lz_match_reverse(matches.span);
    uint32_t next_match = 0;
    for (uint32_t r = 0; r < runs.num; r++) {
        lz_run *run = rc_array_lz_run_at(&runs, r);
        if (run->is_match) {
            run->start  = next_match;
            next_match += run->count;
        }
    }

    return (lz_parse_result) {
        .ok      = true,
        .bits    = total,
        .runs    = runs.view,
        .matches = matches.view,
    };
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A gamma code's width: 2 * floor(log2 v) + 1, for v >= 1.
static int32_t gamma_bits(uint32_t v)
{
    int32_t b = 0;
    while (v > 1) {
        v >>= 1;
        b++;
    }
    return 2 * b + 1;
}

// The pitch's format at a given OB: gamma-coded counts, 8-bit literals, gamma(len - 1) lengths and
// gamma(((off - 1) >> ob) + 1) + ob offsets, each table covering values 0..n. No repeats, and one
// offset table for every length.
typedef struct test_tables {
    rc_span_i32      runs;
    rc_span_i32      lits;
    rc_span_i32      mruns;
    rc_span_i32      lens;
    rc_view_lz_table offs;
    rc_span_i32      reps;
} test_tables;

static rc_span_i32 test_table(uint32_t n, rc_arena *arena)
{
    return rc_array_i32_resize(&(rc_array_i32) {0}, n + 1, arena);
}

static test_tables pitch_tables(uint32_t n, uint32_t ob, rc_arena *arena)
{
    rc_span_i32 offs = test_table(n, arena);
    test_tables t    = {
        .runs = test_table(n, arena),
        .lits = rc_array_i32_resize(&(rc_array_i32) {0}, 256, arena),
        .lens = test_table(n, arena),
    };
    for (uint32_t v = 0; v <= n; v++) {
        rc_span_i32_set(t.runs, v, v == 0 ? -1 : gamma_bits(v));
        rc_span_i32_set(t.lens, v, v < 2 ? -1 : gamma_bits(v - 1));
        rc_span_i32_set(offs, v, v == 0 ? -1 : gamma_bits(((v - 1) >> ob) + 1) + (int32_t) ob);
    }
    for (uint32_t b = 0; b < 256; b++) {
        rc_span_i32_set(t.lits, b, 8);
    }
    rc_array_lz_table tables = {0};
    rc_array_lz_table_push(&tables, offs.view, arena);
    t.mruns = t.runs;
    t.offs  = tables.view;
    return t;
}

// The pitch's format with repeats, three ways: (0) repeats of 1 byte that no ordinary match can stand
// in for, (1) ordinary matches of 1 byte too, (2) ZX0's shape - a flag per match, no run counts.
static test_tables repeat_tables(uint32_t n, uint32_t ob, uint32_t flavour, rc_arena *arena)
{
    test_tables t = pitch_tables(n, ob, arena);
    t.reps = test_table(n, arena);
    for (uint32_t v = 0; v <= n; v++) {
        rc_span_i32_set(t.reps, v, v == 0 ? -1 : gamma_bits(v) + (flavour == 0 ? 0 : 1));
        if (flavour != 0) {
            rc_span_i32_set(t.lens, v, v == 0 ? -1 : gamma_bits(v));
        }
    }
    if (flavour == 2) {
        t.mruns = test_table(n, arena);
        for (uint32_t v = 0; v <= n; v++) {
            rc_span_i32_set(t.mruns, v, v == 0 ? -1 : 0);
        }
    }
    return t;
}

// Exomizer's shape: a flag per literal and per match (none per run), and an offset table for each of
// lengths 1, 2 and 3 or more whose costs follow no order (as fitted buckets do), the length 1 table
// reaching only so far. With repeats or without.
static test_tables exo_tables(uint32_t n, uint32_t seed, bool repeats, rc_arena *arena)
{
    test_tables t       = pitch_tables(n, 0, arena);
    rc_span_i32 offs[3] = {test_table(n, arena), test_table(n, arena), test_table(n, arena)};
    t.mruns = test_table(n, arena);
    for (uint32_t v = 0; v <= n; v++) {
        seed = seed * 1103515245u + 12345u;
        rc_span_i32_set(t.runs, v, (int32_t) v);
        rc_span_i32_set(t.mruns, v, v == 0 ? -1 : 0);
        rc_span_i32_set(t.lens, v, v == 0 ? -1 : gamma_bits(v) + 1);
        for (uint32_t k = 0; k < 3; k++) {
            int32_t c = v == 0 || (k == 0 && v > 4) ? -1 : (int32_t) (2 + ((seed >> (8 + 3 * k)) % 7));
            rc_span_i32_set(offs[k], v, c);
        }
    }
    rc_array_lz_table tables = {0};
    for (uint32_t k = 0; k < 3; k++) {
        rc_array_lz_table_push(&tables, offs[k].view, arena);
    }
    t.offs = tables.view;
    if (repeats) {
        t.reps = test_table(n, arena);
        for (uint32_t v = 0; v <= n; v++) {
            rc_span_i32_set(t.reps, v, v == 0 ? -1 : gamma_bits(v) + 1);
        }
    }
    return t;
}

static lz_costs pitch_costs(test_tables t)
{
    return (lz_costs) {
        .literal_run = t.runs.view,
        .literal     = t.lits.view,
        .match_run   = t.mruns.view,
        .length      = t.lens.view,
        .offset      = t.offs,
        .repeat      = t.reps.view,
    };
}

// A table's cost, -1 if that value is impossible.
static int64_t test_cost(rc_view_i32 t, uint32_t i)
{
    return i < t.num && rc_view_i32_get(t, i) >= 0 ? rc_view_i32_get(t, i) : -1;
}

// A match's offset cost, from the table for its length (the last table serving longer ones).
static int64_t test_offset_cost(test_tables t, uint32_t len, uint32_t off)
{
    uint32_t k = len < t.offs.num ? len - 1 : t.offs.num - 1;
    return test_cost(rc_view_lz_table_get(t.offs, k), off);
}

// Replay a parse the way a decoder would, checking it rebuilds data and costs what it claims. A
// repeat (offset 0) must open a match run and copies from the offset in hand.
static bool replays(rc_view_bytes data, lz_parse_result r, test_tables t)
{
    uint8_t  out[512];
    uint32_t pos  = 0;
    uint32_t held = 0;
    int64_t  bits = 0;
    for (uint32_t i = 0; i < r.runs.num; i++) {
        lz_run run = rc_view_lz_run_get(r.runs, i);
        if (i > 0 && run.is_match == rc_view_lz_run_get(r.runs, i - 1).is_match) {
            return false;   // runs must alternate
        }
        if (i == 0 && run.is_match) {
            return false;   // and start with literals
        }
        int64_t header = test_cost((run.is_match ? t.mruns : t.runs).view, run.count);
        if (header < 0) {
            return false;
        }
        bits += header;
        for (uint32_t k = 0; k < run.count; k++) {
            if (run.is_match) {
                lz_match m   = rc_view_lz_match_get(r.matches, run.start + k);
                uint32_t off = m.offset == 0 ? held : m.offset;
                int64_t  lc  = test_cost(t.lens.view, m.length);
                int64_t  oc  = m.offset == 0 ? 0 : test_offset_cost(t, m.length, m.offset);
                int64_t  c   = m.offset == 0 ? test_cost(t.reps.view, m.length) : lc + oc;
                if ((m.offset == 0 && (k != 0 || held == 0)) || off == 0 || off > pos || c < 0
                    || (m.offset != 0 && (lc < 0 || oc < 0))) {
                    return false;
                }
                for (uint32_t c2 = 0; c2 < m.length; c2++, pos++) {
                    out[pos] = out[pos - off];
                }
                bits += c;
                held  = off;
            }
            else {
                out[pos++] = rc_view_bytes_get(data, run.start + k);
                bits += 8;
            }
        }
    }
    if (pos != data.num || bits != r.bits) {
        return false;
    }
    for (uint32_t i = 0; i < pos; i++) {
        if (out[i] != rc_view_bytes_get(data, i)) {
            return false;
        }
    }
    return true;
}

// The true optimum, found independently: every offset tried (not just the nearest per length), every
// match count kept apart (no staircase) and the offset in hand tracked exactly, memoised over (position,
// matches so far in the current run, offset in hand - 0 for none).
enum { brute_max = 12 };

typedef struct brute {
    rc_view_bytes data;
    test_tables   t;
    int64_t       lit[brute_max + 1][brute_max + 1];                   // literals next at pos (or the end)
    int64_t       match[brute_max + 1][brute_max + 1][brute_max + 1];  // in a match run at pos with k matches
    bool          lit_done[brute_max + 1][brute_max + 1];
    bool          match_done[brute_max + 1][brute_max + 1][brute_max + 1];
} brute;

static int64_t brute_match(brute *b, uint32_t pos, uint32_t k, uint32_t held);

static int64_t brute_literal(brute *b, uint32_t pos, uint32_t held)
{
    if (pos == b->data.num) {
        return 0;   // the stream may end after any whole run
    }
    if (!b->lit_done[pos][held]) {
        int64_t best = INT64_MAX;
        for (uint32_t len = 1; pos + len <= b->data.num; len++) {
            int64_t header = test_cost(b->t.runs.view, len);
            int64_t rest   = pos + len == b->data.num ? 0 : brute_match(b, pos + len, 0, held);
            if (header >= 0 && rest != INT64_MAX) {
                int64_t c = header + 8 * (int64_t) len + rest;
                best = c < best ? c : best;
            }
        }
        b->lit[pos][held]      = best;
        b->lit_done[pos][held] = true;
    }
    return b->lit[pos][held];
}

static int64_t brute_match(brute *b, uint32_t pos, uint32_t k, uint32_t held)
{
    if (!b->match_done[pos][k][held]) {
        int64_t best = INT64_MAX;
        if (k > 0) {
            int64_t header = test_cost(b->t.mruns.view, k);
            int64_t rest   = brute_literal(b, pos, held);
            if (header >= 0 && rest != INT64_MAX) {
                best = header + rest;
            }
        }
        for (uint32_t off = 1; off <= pos; off++) {
            bool repeat = k == 0 && off == held;
            for (uint32_t len = 1; pos + len <= b->data.num; len++) {
                if (rc_view_bytes_get(b->data, pos + len - 1) != rc_view_bytes_get(b->data, pos + len - 1 - off)) {
                    break;
                }
                int64_t lcost = test_cost(b->t.lens.view, len);
                int64_t ocost = test_offset_cost(b->t, len, off);
                int64_t rcost = repeat ? test_cost(b->t.reps.view, len) : -1;
                int64_t rest  = brute_match(b, pos + len, k + 1, off);
                if (rest == INT64_MAX) {
                    continue;
                }
                if (lcost >= 0 && ocost >= 0 && lcost + ocost + rest < best) {
                    best = lcost + ocost + rest;
                }
                if (rcost >= 0 && rcost + rest < best) {
                    best = rcost + rest;
                }
            }
        }
        b->match[pos][k][held]      = best;
        b->match_done[pos][k][held] = true;
    }
    return b->match[pos][k][held];
}

RC_TEST(lzparse, matches_brute_force_on_small_inputs)
{
    // Small inputs over a two-letter alphabet, so matches abound and run counts genuinely compete:
    // the staircase and the nearest-offset candidates must still find the true optimum.
    rc_arena arena = rc_arena_make_default();
    uint32_t seed = 12345;
    for (uint32_t trial = 0; trial < 60; trial++) {
        uint8_t  bytes[12];
        uint32_t n = 1 + trial % 12;
        for (uint32_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            bytes[i] = (uint8_t) ((seed >> 16) & 1);
        }
        rc_view_bytes data = {.data = bytes, .num = n};
        for (uint32_t ob = 0; ob < 3; ob++) {
            test_tables     t = pitch_tables(n, ob, &arena);
            lz_parse_result r = lz_parse(data, pitch_costs(t), &arena);
            RC_CHECK_TRUE(r.ok);
            RC_CHECK_TRUE(replays(data, r, t));
            brute b = {.data = data, .t = t};
            RC_CHECK(r.bits, ==, brute_literal(&b, 0, 0));
        }
    }
    rc_arena_deinit(&arena);
}

RC_TEST(lzparse, repeats_match_brute_force_on_small_inputs)
{
    // The same over two and three letters, with repeats: the offset in hand, per-offset queues and
    // their pruning must still find the true optimum, in all three shapes of table.
    rc_arena arena = rc_arena_make_default();
    uint32_t seed    = 777;
    uint32_t repeats = 0;
    for (uint32_t trial = 0; trial < 90; trial++) {
        uint8_t  bytes[12];
        uint32_t n = 1 + trial % 12;
        for (uint32_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            bytes[i] = (uint8_t) ((seed >> 16) % (2 + trial % 2));
        }
        rc_view_bytes data = {.data = bytes, .num = n};
        for (uint32_t flavour = 0; flavour < 3; flavour++) {
            test_tables     t = repeat_tables(n, trial % 3, flavour, &arena);
            lz_parse_result r = lz_parse(data, pitch_costs(t), &arena);
            RC_CHECK_TRUE(r.ok);
            RC_CHECK_TRUE(replays(data, r, t));
            brute b = {.data = data, .t = t};
            RC_CHECK(r.bits, ==, brute_literal(&b, 0, 0));
            for (uint32_t i = 0; i < r.matches.num; i++) {
                repeats += rc_view_lz_match_get(r.matches, i).offset == 0;
            }
        }
    }
    RC_CHECK(repeats, >, 20u);   // the trials really do exercise them
    rc_arena_deinit(&arena);
}

RC_TEST(lzparse, exomizer_shape_matches_brute_force)
{
    // A flag per item and offset tables by length whose costs follow no order: every match must try
    // every offset (a farther one can be cheaper) and price it by its length's table - with repeats too.
    rc_arena arena = rc_arena_make_default();
    uint32_t seed  = 99;
    for (uint32_t trial = 0; trial < 90; trial++) {
        uint8_t  bytes[12];
        uint32_t n = 1 + trial % 12;
        for (uint32_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            bytes[i] = (uint8_t) ((seed >> 16) % (2 + trial % 2));
        }
        rc_view_bytes data = {.data = bytes, .num = n};
        for (uint32_t repeats = 0; repeats < 2; repeats++) {
            test_tables     t = exo_tables(n, seed ^ trial, repeats == 1, &arena);
            lz_parse_result r = lz_parse(data, pitch_costs(t), &arena);
            RC_CHECK_TRUE(r.ok);
            RC_CHECK_TRUE(replays(data, r, t));
            brute b = {.data = data, .t = t};
            RC_CHECK(r.bits, ==, brute_literal(&b, 0, 0));
        }
    }
    rc_arena_deinit(&arena);
}

RC_TEST(lzparse, repeats_replay_on_longer_inputs)
{
    // Longer inputs, beyond the brute force: blocks repeated with the odd byte changed (a match, a hole,
    // a repeat), which every parse must still replay exactly, at the cost it claims.
    rc_arena arena = rc_arena_make_default();
    uint32_t seed = 4242;
    for (uint32_t trial = 0; trial < 12; trial++) {
        uint8_t  bytes[400];
        uint32_t n     = 100 + trial * 25;
        uint32_t block = 3 + trial % 7;
        for (uint32_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            bytes[i] = i < block ? (uint8_t) (seed >> 16) : bytes[i - block];
            if ((seed >> 8) % 11 == 0) {
                bytes[i] = (uint8_t) (seed >> 20);
            }
        }
        rc_view_bytes data = {.data = bytes, .num = n};
        test_tables     t = trial % 4 == 3 ? exo_tables(n, trial, true, &arena) : repeat_tables(n, trial % 3, trial % 3, &arena);
        lz_parse_result r = lz_parse(data, pitch_costs(t), &arena);
        RC_CHECK_TRUE(r.ok);
        RC_CHECK_TRUE(replays(data, r, t));

        // Repeats never make it dearer than the same tables without them.
        test_tables     plain = t;
        plain.reps            = (rc_span_i32) {0};
        lz_parse_result r0    = lz_parse(data, pitch_costs(plain), &arena);
        RC_CHECK_TRUE(r0.ok && replays(data, r0, plain));
        RC_CHECK(r.bits, <=, r0.bits);
    }
    rc_arena_deinit(&arena);
}

RC_TEST(lzparse, long_runs_and_edges)
{
    rc_arena arena = rc_arena_make_default();

    // Nothing at all is an empty parse.
    test_tables     t0 = pitch_tables(0, 0, &arena);
    lz_parse_result r0 = lz_parse((rc_view_bytes) {0}, pitch_costs(t0), &arena);
    RC_CHECK_TRUE(r0.ok);
    RC_CHECK(r0.runs.num, ==, 0u);
    RC_CHECK(r0.bits, ==, 0);

    // 200 zeroes: one literal, then a single overlapping match at offset 1 does the rest.
    uint8_t zeroes[200] = {0};
    rc_view_bytes z = {.data = zeroes, .num = 200};
    test_tables     tz = pitch_tables(200, 0, &arena);
    lz_parse_result rz = lz_parse(z, pitch_costs(tz), &arena);
    RC_CHECK_TRUE(rz.ok && replays(z, rz, tz));
    RC_CHECK(rz.runs.num, ==, 2u);
    RC_CHECK(rc_view_lz_match_get(rz.matches, 0).offset, ==, 1u);
    RC_CHECK(rc_view_lz_match_get(rz.matches, 0).length, ==, 199u);

    // A capped header table (counts up to 4 only) still parses, splitting long runs of literals.
    uint8_t distinct[20];
    for (uint32_t i = 0; i < 20; i++) {
        distinct[i] = (uint8_t) i;
    }
    rc_view_bytes d  = {.data = distinct, .num = 20};
    test_tables   td = pitch_tables(20, 0, &arena);
    lz_costs      capped = pitch_costs(td);
    capped.literal_run   = rc_span_i32_get_head(td.runs, 5).view;
    lz_parse_result rd   = lz_parse(d, capped, &arena);
    RC_CHECK_FALSE(rd.ok);   // literals must alternate with matches, and there are no matches here

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
