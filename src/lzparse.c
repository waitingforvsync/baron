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

enum { lz_min_match = 2 };

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


// One partial match run that has reached a position: k matches costing `cost`, the header unpaid. Each
// remembers the match that brought it here, so the winning runs can be walked back from the end.
typedef struct lz_entry {
    int64_t  cost;
    uint32_t k;
    uint32_t prev;     // the entry the match extended, or RC_INDEX_NONE: the run opened at `from`
    uint32_t from;     // where the match starts
    uint32_t length;
    uint32_t offset;
    uint32_t next;     // the next entry at the same position (or on the free list)
} lz_entry;

#define RC_ARRAY_TYPE lz_entry
#define RC_ARRAY_NAME lz_entry
#include "richc/template/array.h"

// Every position's staircase, as linked lists threaded through one pool. An entry pushed off a
// staircase was never used yet (nothing extends a position's entries until that position is reached,
// and by then its staircase is final), so its slot goes back on the free list.
typedef struct lz_stairs {
    rc_array_lz_entry pool;
    rc_span_u32       head;   // per position: first entry, or RC_INDEX_NONE
    uint32_t          free;
} lz_stairs;

static uint32_t stairs_alloc(lz_stairs *s, lz_entry e, rc_arena *arena)
{
    if (s->free != RC_INDEX_NONE) {
        uint32_t i = s->free;
        s->free = rc_array_lz_entry_get(&s->pool, i).next;
        rc_array_lz_entry_set(&s->pool, i, e);
        return i;
    }
    return rc_array_lz_entry_push(&s->pool, e, arena);
}

// Offer a partial run to position q's staircase. It is refused if one already there has no more
// matches and costs no more (the header never gets cheaper as the count grows, so that one can never
// do worse); any it beats the same way are dropped.
static void stairs_offer(lz_stairs *s, uint32_t q, lz_entry e, rc_arena *arena)
{
    for (uint32_t i = rc_span_u32_get(s->head, q); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&s->pool, i).next) {
        lz_entry o = rc_array_lz_entry_get(&s->pool, i);
        if (o.k <= e.k && o.cost <= e.cost) {
            return;
        }
    }

    uint32_t *link = rc_span_u32_at(s->head, q);
    while (*link != RC_INDEX_NONE) {
        lz_entry *o = rc_array_lz_entry_at(&s->pool, *link);
        if (o->k >= e.k && o->cost >= e.cost) {
            uint32_t gone = *link;
            *link   = o->next;
            o->next = s->free;
            s->free = gone;
        }
        else {
            link = &o->next;
        }
    }

    e.next = rc_span_u32_get(s->head, q);
    uint32_t i = stairs_alloc(s, e, arena);
    rc_span_u32_set(s->head, q, i);
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
    uint32_t top = table.num == 0 ? 0 : (table.num - 1 < n ? table.num - 1 : n);
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


lz_parse_result lz_parse(rc_view_bytes data, lz_costs costs, rc_arena *arena)
{
    uint32_t n = data.num;

    // Prefix sums of literal costs, so a literal run [i, j) costs lits[j] - lits[i] plus its header. A
    // byte that cannot be a literal costs "impossible", which no real total reaches.
    rc_span_i64 lits = rc_array_i64_resize(&(rc_array_i64) {0}, n + 1, arena);
    rc_span_i64_set(lits, 0, 0);
    for (uint32_t i = 0; i < n; i++) {
        int64_t c = table_cost(costs.literal, rc_view_bytes_get(data, i));
        rc_span_i64_set(lits, i + 1, rc_span_i64_get(lits, i) + (c < 0 ? lz_impossible : c));
    }

    rc_span_i64 lit_end   = rc_array_i64_resize(&(rc_array_i64) {0}, n + 1, arena);
    rc_span_u32 lit_from  = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena);
    rc_span_i64 match_end = rc_array_i64_resize(&(rc_array_i64) {0}, n + 1, arena);
    rc_span_u32 match_by  = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena);
    for (uint32_t p = 0; p <= n; p++) {
        rc_span_i64_set(lit_end, p, lz_impossible);
        rc_span_i64_set(match_end, p, lz_impossible);
        rc_span_u32_set(match_by, p, RC_INDEX_NONE);
    }
    rc_span_i64_set(match_end, 0, 0);   // the start: literals come first

    lz_stairs stairs = {
        .pool = rc_array_lz_entry_make(n + 16, arena),
        .head = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena),
        .free = RC_INDEX_NONE,
    };
    for (uint32_t p = 0; p <= n; p++) {
        rc_span_u32_set(stairs.head, p, RC_INDEX_NONE);
    }

    rc_array_lz_band bands = literal_bands(costs.literal_run, n, arena);
    bool scan_literals = bands.num == 0;

    // Hash chains over each position's first two bytes, nearest first; best_offset[len] is the nearest
    // offset reaching at least len bytes at the current position, the cheapest for that length.
    rc_span_u32 chain_head  = rc_array_u32_resize(&(rc_array_u32) {0}, 65536, arena);
    rc_span_u32 chain_next  = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena);
    rc_span_u32 best_offset = rc_array_u32_resize(&(rc_array_u32) {0}, n + 1, arena);
    for (uint32_t h = 0; h < 65536; h++) {
        rc_span_u32_set(chain_head, h, RC_INDEX_NONE);
    }

    for (uint32_t p = 0; p <= n; p++) {
        // Close every partial match run that has reached p.
        for (uint32_t i = rc_span_u32_get(stairs.head, p); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&stairs.pool, i).next) {
            lz_entry e = rc_array_lz_entry_get(&stairs.pool, i);
            int64_t header = table_cost(costs.match_run, e.k);
            if (header >= 0 && e.cost + header < rc_span_i64_get(match_end, p)) {
                rc_span_i64_set(match_end, p, e.cost + header);
                rc_span_u32_set(match_by, p, i);
            }
        }

        // The best literal run ending at p, from wherever a match run (or the start) left off.
        if (scan_literals) {
            for (uint32_t i = 0; i < p; i++) {
                int64_t header = table_cost(costs.literal_run, p - i);
                int64_t from   = rc_span_i64_get(match_end, i);
                if (header < 0 || from >= lz_impossible) {
                    continue;
                }
                int64_t c = from + header + rc_span_i64_get(lits, p) - rc_span_i64_get(lits, i);
                if (c < rc_span_i64_get(lit_end, p)) {
                    rc_span_i64_set(lit_end, p, c);
                    rc_span_u32_set(lit_from, p, i);
                }
            }
        }
        else {
            for (uint32_t b = 0; b < bands.num; b++) {
                lz_band *band = rc_array_lz_band_at(&bands, b);
                if (p >= band->lo) {
                    uint32_t i = p - band->lo;   // a newly possible start, its match_end already final
                    if (rc_span_i64_get(match_end, i) < lz_impossible) {
                        int64_t key = rc_span_i64_get(match_end, i) - rc_span_i64_get(lits, i);
                        while (band->last > band->first) {
                            uint32_t back = rc_span_u32_get(band->deque, band->last - 1);
                            if (rc_span_i64_get(match_end, back) - rc_span_i64_get(lits, back) < key) {
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
                    int64_t  c = rc_span_i64_get(match_end, i) + band->cost
                               + rc_span_i64_get(lits, p) - rc_span_i64_get(lits, i);
                    if (c < rc_span_i64_get(lit_end, p)) {
                        rc_span_i64_set(lit_end, p, c);
                        rc_span_u32_set(lit_from, p, i);
                    }
                }
            }
        }

        if (p + lz_min_match > n) {
            continue;   // no room for a match from here
        }

        // The matches from p: for each length, the nearest offset that reaches it.
        uint32_t max_len = n - p;
        if (costs.length.num == 0) {
            max_len = 0;
        }
        else if (costs.length.num - 1 < max_len) {
            max_len = costs.length.num - 1;
        }
        uint32_t best = 1;
        uint32_t key  = rc_view_bytes_get(data, p) | (uint32_t) rc_view_bytes_get(data, p + 1) << 8;
        for (uint32_t q = rc_span_u32_get(chain_head, key); q != RC_INDEX_NONE && best < max_len; q = rc_span_u32_get(chain_next, q)) {
            uint32_t off = p - q;
            if (table_cost(costs.offset, off) < 0) {
                break;   // offsets only get dearer from here
            }
            uint32_t len = lz_min_match;
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

        // Extend every way of standing at p into a match run by each of those matches: a fresh run after
        // a literal run, or one more match on a partial run already here.
        for (uint32_t len = lz_min_match; len <= best; len++) {
            uint32_t off   = rc_span_u32_get(best_offset, len);
            int64_t  lcost = table_cost(costs.length, len);
            int64_t  ocost = table_cost(costs.offset, off);
            if (lcost < 0 || ocost < 0) {
                continue;
            }
            int64_t  m = lcost + ocost;
            uint32_t q = p + len;

            if (rc_span_i64_get(lit_end, p) < lz_impossible && costs.match_run.num > 1) {
                stairs_offer(&stairs, q, (lz_entry) {
                    .cost   = rc_span_i64_get(lit_end, p) + m,
                    .k      = 1,
                    .prev   = RC_INDEX_NONE,
                    .from   = p,
                    .length = len,
                    .offset = off,
                }, arena);
            }
            for (uint32_t i = rc_span_u32_get(stairs.head, p); i != RC_INDEX_NONE; i = rc_array_lz_entry_get(&stairs.pool, i).next) {
                lz_entry e = rc_array_lz_entry_get(&stairs.pool, i);
                if (e.k + 1 >= costs.match_run.num) {
                    continue;   // a run this long has no header to close it with
                }
                stairs_offer(&stairs, q, (lz_entry) {
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

    int64_t end_lit   = rc_span_i64_get(lit_end, n);
    int64_t end_match = rc_span_i64_get(match_end, n);
    int64_t total     = end_lit < end_match ? end_lit : end_match;
    if (total >= lz_impossible) {
        return (lz_parse_result) {0};
    }

    // Walk back from the end, collecting runs (and matches) last first, then turn both round.
    rc_array_lz_run   runs    = {0};
    rc_array_lz_match matches = {0};
    bool     literal = end_lit < end_match;
    uint32_t pos     = n;
    while (pos > 0) {
        if (literal) {
            uint32_t i = rc_span_u32_get(lit_from, pos);
            rc_array_lz_run_push(&runs, (lz_run) {.is_match = false, .start = i, .count = pos - i}, arena);
            pos = i;
        }
        else {
            uint32_t count = 0;
            uint32_t e     = rc_span_u32_get(match_by, pos);
            uint32_t from  = pos;
            while (e != RC_INDEX_NONE) {
                lz_entry entry = rc_array_lz_entry_get(&stairs.pool, e);
                rc_array_lz_match_push(&matches, (lz_match) {.offset = entry.offset, .length = entry.length}, arena);
                count++;
                from = entry.from;
                e    = entry.prev;
            }
            rc_array_lz_run_push(&runs, (lz_run) {.is_match = true, .count = count}, arena);
            pos = from;
        }
        literal = !literal;
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
// gamma(((off - 1) >> ob) + 1) + ob offsets, each table covering values 0..n.
typedef struct test_tables {
    rc_span_i32 runs;
    rc_span_i32 lits;
    rc_span_i32 lens;
    rc_span_i32 offs;
} test_tables;

static test_tables pitch_tables(uint32_t n, uint32_t ob, rc_arena *arena)
{
    test_tables t = {
        .runs = rc_array_i32_resize(&(rc_array_i32) {0}, n + 1, arena),
        .lits = rc_array_i32_resize(&(rc_array_i32) {0}, 256, arena),
        .lens = rc_array_i32_resize(&(rc_array_i32) {0}, n + 1, arena),
        .offs = rc_array_i32_resize(&(rc_array_i32) {0}, n + 1, arena),
    };
    for (uint32_t v = 0; v <= n; v++) {
        rc_span_i32_set(t.runs, v, v == 0 ? -1 : gamma_bits(v));
        rc_span_i32_set(t.lens, v, v < 2 ? -1 : gamma_bits(v - 1));
        rc_span_i32_set(t.offs, v, v == 0 ? -1 : gamma_bits(((v - 1) >> ob) + 1) + (int32_t) ob);
    }
    for (uint32_t b = 0; b < 256; b++) {
        rc_span_i32_set(t.lits, b, 8);
    }
    return t;
}

static lz_costs pitch_costs(test_tables t)
{
    return (lz_costs) {
        .literal_run = t.runs.view,
        .literal     = t.lits.view,
        .match_run   = t.runs.view,
        .length      = t.lens.view,
        .offset      = t.offs.view,
    };
}

// Replay a parse the way a decoder would, checking it rebuilds data and costs what it claims.
static bool replays(rc_view_bytes data, lz_parse_result r, test_tables t)
{
    uint8_t out[256];
    uint32_t pos  = 0;
    int64_t  bits = 0;
    for (uint32_t i = 0; i < r.runs.num; i++) {
        lz_run run = rc_view_lz_run_get(r.runs, i);
        if (i > 0 && run.is_match == rc_view_lz_run_get(r.runs, i - 1).is_match) {
            return false;   // runs must alternate
        }
        if (i == 0 && run.is_match) {
            return false;   // and start with literals
        }
        bits += rc_span_i32_get(t.runs, run.count);
        for (uint32_t k = 0; k < run.count; k++) {
            if (run.is_match) {
                lz_match m = rc_view_lz_match_get(r.matches, run.start + k);
                if (m.offset == 0 || m.offset > pos) {
                    return false;
                }
                for (uint32_t c = 0; c < m.length; c++, pos++) {
                    out[pos] = out[pos - m.offset];
                }
                bits += rc_span_i32_get(t.lens, m.length) + rc_span_i32_get(t.offs, m.offset);
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

// The true optimum, found independently: every offset tried (not just the nearest per length) and every
// match count kept apart (no staircase), memoised over (position, matches so far in the current run).
enum { brute_max = 12 };

typedef struct brute {
    rc_view_bytes data;
    test_tables   t;
    int64_t       lit[brute_max + 1];                   // literals next at pos (or the end)
    int64_t       match[brute_max + 1][brute_max + 1];  // inside a match run at pos with k matches, header unpaid
    bool          lit_done[brute_max + 1];
    bool          match_done[brute_max + 1][brute_max + 1];
} brute;

static int64_t brute_match(brute *b, uint32_t pos, uint32_t k);

static int64_t brute_literal(brute *b, uint32_t pos)
{
    if (pos == b->data.num) {
        return 0;   // the stream may end after any whole run
    }
    if (!b->lit_done[pos]) {
        int64_t best = INT64_MAX;
        for (uint32_t len = 1; pos + len <= b->data.num; len++) {
            int64_t rest = pos + len == b->data.num ? 0 : brute_match(b, pos + len, 0);
            if (rest != INT64_MAX) {
                int64_t c = rc_span_i32_get(b->t.runs, len) + 8 * (int64_t) len + rest;
                best = c < best ? c : best;
            }
        }
        b->lit[pos]      = best;
        b->lit_done[pos] = true;
    }
    return b->lit[pos];
}

static int64_t brute_match(brute *b, uint32_t pos, uint32_t k)
{
    if (!b->match_done[pos][k]) {
        int64_t best = INT64_MAX;
        if (k > 0) {
            int64_t rest = brute_literal(b, pos);
            if (rest != INT64_MAX) {
                best = rc_span_i32_get(b->t.runs, k) + rest;
            }
        }
        for (uint32_t off = 1; off <= pos; off++) {
            for (uint32_t len = 2; pos + len <= b->data.num; len++) {
                if (rc_view_bytes_get(b->data, pos + len - 1) != rc_view_bytes_get(b->data, pos + len - 1 - off)
                    || rc_view_bytes_get(b->data, pos) != rc_view_bytes_get(b->data, pos - off)) {
                    break;
                }
                int64_t rest = brute_match(b, pos + len, k + 1);
                if (rest != INT64_MAX) {
                    int64_t c = rc_span_i32_get(b->t.lens, len) + rc_span_i32_get(b->t.offs, off) + rest;
                    best = c < best ? c : best;
                }
            }
        }
        b->match[pos][k]      = best;
        b->match_done[pos][k] = true;
    }
    return b->match[pos][k];
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
            RC_CHECK(r.bits, ==, brute_literal(&b, 0));
        }
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
