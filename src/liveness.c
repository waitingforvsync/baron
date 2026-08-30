#include "liveness.h"

#include "richc/macros.h"


// The small set algebra the fixpoint needs (copy / union / equality) lives in richc's rc_bitset now. The
// dataflow sets are byte-wide (nbytes bits), the public results variable-wide (num_vars bits); sets only
// ever meet others of their own width, so the equal-width preconditions hold throughout.

// Allocate `n` zeroed bitsets each `width` bits wide (NULL if n == 0).
static rc_bitset *make_rows(uint32_t n, uint32_t width, rc_arena *arena)
{
    if (n == 0) {
        return NULL;
    }
    rc_bitset *rows = rc_arena_alloc_type(arena, rc_bitset, n);
    for (uint32_t i = 0; i < n; i++) {
        rows[i] = (rc_bitset) {0};
        rc_bitset_resize(&rows[i], width, arena);
    }
    return rows;
}

// ---- the byte id space ----
// Liveness is tracked per BYTE, not per variable: vreg v's bytes occupy [base[v], base[v] + width[v]) in a
// dense byte id space, and the dataflow sets below are byte sets. This is what makes partial writes come
// out right for multi-byte variables: a store redefines exactly the byte it hits, so `STA ptr` alone
// leaves the MSB's old value live straight through it, while a `STA ptr : STA ptr+1` pair accumulates into
// a full kill through the fixpoint (across blocks too - each byte's kill propagates independently). The
// PUBLIC results stay at variable level: the interference rows and live-in/out sets are projected back,
// a variable being live iff any of its bytes is - the allocator places whole variables, so that is the
// granularity it consumes.

// Which of its variable's bytes an instruction touches, as offset windows within the variable.
//   read  - bytes whose OLD value the instruction consumes: the addressed byte (two for an indirect
//           pointer deref), or the whole variable for an indexed / unknown-offset access.
//   write - bytes the instruction PROVABLY redefines: a direct store's one byte. An indexed or
//           unknown-offset store redefines nothing provable (it still pins the variable for interference).
typedef struct touch_window {
    uint32_t read_first, read_count;
    uint32_t write_first, write_count;
} touch_window;

static touch_window insn_window(zp_insn n, uint16_t width)
{
    touch_window w = {0};
    if (n.var_kill) {
        w.write_first = 0;    // a ZA_DISCARD "writes" the whole variable: the old value is promised dead here.
        w.write_count = width;   // No bytes are read, and the caller must not treat this as a store (no pin)
        return w;
    }
    bool direct = !n.var_indexed && n.var_offset != RC_INDEX_NONE && n.var_offset < width;
    if (n.rw & vref_read) {
        uint32_t access = n.var_indirect ? 2u : 1u;
        if (direct && n.var_offset + access <= width) {
            w.read_first = n.var_offset;
            w.read_count = access;
        }
        else {
            w.read_first = 0;   // indexed / unknown offset: any byte's old value may be consumed
            w.read_count = width;
        }
    }
    if ((n.rw & vref_write) && direct) {
        w.write_first = n.var_offset;
        w.write_count = 1;   // a 6502 store writes one byte
    }
    return w;
}

static void add_edge(rc_bitset *interfere, uint32_t a, uint32_t b)
{
    if (a != b) {
        rc_bitset_set(&interfere[a], b);
        rc_bitset_set(&interfere[b], a);
    }
}

// Does this block hand control back to the caller of the routine containing it? True for an RTS/RTI - and
// for a transfer OUT of the program, because the external routine's own RTS returns to OUR caller (the
// tail-call idiom); that includes an external ZA_CANJUMP arm of a dispatch, which the CFG wires no edge for.
// A ZA_RETURN annotation is the declared form of exactly that: the jump/branch hands straight back to our
// caller (the inline-data trick's computed exit), no external routine in between. An RTS wearing a
// ZA_CANJUMP is the dispatch trick - control continues at the declared targets, not the caller - so it does
// NOT return here (its targets' own exits do), unless one of its arms is external; likewise a jump/branch
// whose declared arms all resolved in-program. The annotations are consulted BEFORE a resolved literal
// target, because a declared set replaces a self-modified operand's placeholder edge in the CFG too.
static bool block_returns(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, basic_block blk)
{
    if (blk.num_insns == 0) {
        return false;
    }
    zp_insn last = rc_view_zp_insn_get(insns, blk.first_insn + blk.num_insns - 1);
    if (last.flow != zp_flow_jump && last.flow != zp_flow_branch && last.flow != zp_flow_return) {
        return false;   // normal/call/skip flow: not an exit of any kind (a skip always resumes in-stream)
    }
    bool annotated = false;
    for (uint32_t j = 0; j < cflows.num; j++) {
        zp_cflow cf = rc_view_zp_cflow_get(cflows, j);
        if (cf.site != last.pc) {
            continue;
        }
        if (cf.kind == zp_cflow_za_return) {
            return true;   // the declared "hands back to our caller" exit
        }
        if (cf.kind == zp_cflow_za_canjump) {
            annotated = true;
            if (cfg_block_at(g, last.section, cf.target) == RC_INDEX_NONE) {
                return true;   // an external arm hands back, via the external routine's RTS
            }
        }
    }
    if (annotated) {
        return false;   // every declared arm resolved in-program: control continues at them
    }
    if (last.flow == zp_flow_return) {
        return true;   // a bare RTS/RTI returns
    }
    if (cfg_target_block(g, last) != RC_INDEX_NONE) {
        return false;   // a plain resolved jump/branch: an ordinary edge, not an exit
    }
    return cfg_target_is_external(g, last);   // an unannotated external jump/branch out
}

// The bytes a call definitely writes whichever arm it takes: the intersection of its callees' current
// must-write sets (mwb, indexed by entry block). Empty when any arm is external (it returns having written
// nothing of ours) or untrackable. A fresh set in `arena` each call - the sets are tiny and short-lived.
static rc_bitset call_kill_bytes(call_targets ct, const rc_bitset *mwb, uint32_t nbytes, rc_arena *arena)
{
    rc_bitset out = {0};
    rc_bitset_resize(&out, nbytes, arena);
    if (!ct.unknown && !ct.external && ct.blocks.view.num != 0) {
        rc_bitset_copy(&out, &mwb[rc_array_u32_get(&ct.blocks, 0)]);
        for (uint32_t a = 1; a < ct.blocks.view.num; a++) {
            rc_bitset_intersection(&out, &mwb[rc_array_u32_get(&ct.blocks, a)]);
        }
    }
    return out;
}

liveness liveness_analyze(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_var vars,
                          uint32_t entry_block, rc_arena *arena, rc_arena scratch)
{
    uint32_t num_vars = vars.num;
    uint32_t nb = g.blocks.num;
    liveness lv = {
        .num_vars   = num_vars,
        .num_blocks = nb,
        .live_in    = make_rows(nb, num_vars, arena),
        .live_out   = make_rows(nb, num_vars, arena),
        .interfere  = make_rows(num_vars, num_vars, arena),
        .must_write = make_rows(nb, num_vars, arena),
        .classes    = num_vars ? rc_arena_alloc_zero_type(arena, vreg_class, num_vars) : NULL,
    };
    if (nb == 0 || num_vars == 0) {
        return lv;
    }

    // Stand up the byte id space: contiguous per-variable byte runs, plus the byte -> owning-vreg map the
    // projections read.
    uint32_t *base = rc_arena_alloc_type(&scratch, uint32_t, num_vars);
    uint32_t nbytes = 0;
    for (uint32_t v = 0; v < num_vars; v++) {
        base[v] = nbytes;
        nbytes += rc_view_zp_var_get(vars, v).width;
    }
    uint32_t *owner = rc_arena_alloc_type(&scratch, uint32_t, nbytes);
    for (uint32_t v = 0; v < num_vars; v++) {
        for (uint32_t k = 0; k < rc_view_zp_var_get(vars, v).width; k++) {
            owner[base[v] + k] = v;
        }
    }

    // Each call site's callee entry blocks, resolved once (ZA_CANCALL overrides included). A call is treated
    // as a USE of its callee's live-in set - the callee's inputs - which is what gives an argument stored
    // by the caller a live range reaching the JSR, so nothing can be coloured over it in between; and as a
    // KILL of the bytes its callees definitely write (the must-write sets below) - the caller's read after
    // the call receives the callee's value, so the pre-call byte is dead and a delivered result's range
    // starts at its call, not at the top of the caller. The arms that resolve to no blocks inject and kill
    // nothing: an external callee touches none of our bytes, and a computed unannotated call is the user's
    // responsibility (ZA_CANCALL declares the targets whose inputs and writes then count).
    call_targets *calls = rc_arena_alloc_zero_type(&scratch, call_targets, insns.num);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.flow == zp_flow_call) {
            calls[i] = cfg_call_targets(g, cflows, n, &scratch);
        }
    }

    // A full set for the unknown_succ taint: a block whose control may leave to an address we cannot model
    // must assume every byte is live out (the computed target might read any of them).
    rc_bitset full = {0};
    rc_bitset_resize(&full, nbytes, &scratch);
    for (uint32_t i = 0; i < nbytes; i++) {
        rc_bitset_set(&full, i);
    }

    // ---- must-write (definite assignment) ----
    // For every routine entered by a call, the bytes written on EVERY path from its entry to a return: a
    // forward "must" analysis (meet = intersection, non-entry blocks start at FULL and only shrink), run
    // per entry over the blocks reachable from it, inside an outer fixpoint so a call inside a routine
    // contributes its own callees' (still-shrinking) sets - recursion converges downward from FULL. A
    // routine with no returning path vacuously must-writes everything (its caller's post-call code never
    // runs); an unknown-succ block anywhere in the extent forfeits the lot (Guard 1 refuses such programs
    // anyway). Sound in one direction only: an under-approximation just kills less.
    bool *is_entry = rc_arena_alloc_zero_type(&scratch, bool, nb);
    for (uint32_t i = 0; i < insns.num; i++) {
        for (uint32_t c = 0; c < calls[i].blocks.view.num; c++) {
            is_entry[rc_array_u32_get(&calls[i].blocks, c)] = true;
        }
    }

    // Predecessor lists, once, for the forward meets.
    uint32_t *pred_count = rc_arena_alloc_zero_type(&scratch, uint32_t, nb);
    uint32_t *pred_first = rc_arena_alloc_type(&scratch, uint32_t, nb);
    uint32_t  npreds     = 0;
    for (uint32_t b = 0; b < nb; b++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, b);
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            pred_count[cfg_succ(g, blk, s)]++;
        }
        npreds += blk.succ_count;
    }
    // Two separate loops, deliberately: the prefix sum reads pred_count[b - 1], so zeroing the counts
    // (they are reused as the fill cursors) must wait until the whole sum is done - fused into one loop,
    // each pred_first after the first came out zero and every block's preds landed on top of each other.
    for (uint32_t b = 0; b < nb; b++) {
        pred_first[b] = (b == 0) ? 0 : pred_first[b - 1] + pred_count[b - 1];
    }
    for (uint32_t b = 0; b < nb; b++) {
        pred_count[b] = 0;   // reused as the fill cursor
    }
    uint32_t *preds = npreds ? rc_arena_alloc_type(&scratch, uint32_t, npreds) : NULL;
    for (uint32_t b = 0; b < nb; b++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, b);
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            uint32_t t = cfg_succ(g, blk, s);
            preds[pred_first[t] + pred_count[t]++] = b;
        }
    }

    // mwb[e] = the byte set routine e definitely writes, for entry blocks (others unused). Start FULL.
    rc_bitset *mwb = make_rows(nb, nbytes, &scratch);
    for (uint32_t e = 0; e < nb; e++) {
        if (is_entry[e]) {
            rc_bitset_copy(&mwb[e], &full);
        }
    }
    rc_bitset *mwin  = make_rows(nb, nbytes, &scratch);   // per-entry working rows, re-seeded each visit
    rc_bitset *mwout = make_rows(nb, nbytes, &scratch);
    rc_bitset  acc   = {0}; rc_bitset_resize(&acc, nbytes, &scratch);
    rc_bitset  mrow  = {0}; rc_bitset_resize(&mrow, nbytes, &scratch);
    bool      *in_ext = rc_arena_alloc_type(&scratch, bool, nb);   // extent of the entry under analysis
    uint32_t  *stack  = rc_arena_alloc_type(&scratch, uint32_t, nb);

    bool mw_changed = true;
    while (mw_changed) {
        mw_changed = false;
        for (uint32_t e = 0; e < nb; e++) {
            if (!is_entry[e]) {
                continue;
            }
            // The routine's extent: blocks reachable from e through intraprocedural edges.
            for (uint32_t b = 0; b < nb; b++) { in_ext[b] = false; }
            uint32_t sp = 0;
            stack[sp++] = e;
            in_ext[e] = true;
            bool tainted = false;
            while (sp > 0) {
                basic_block blk = rc_array_basic_block_get(&g.blocks, stack[--sp]);
                tainted = tainted || blk.unknown_succ;
                for (uint32_t s = 0; s < blk.succ_count; s++) {
                    uint32_t t = cfg_succ(g, blk, s);
                    if (!in_ext[t]) { in_ext[t] = true; stack[sp++] = t; }
                }
            }

            rc_bitset_reset(&acc);   // accumulates the intersection over returning exits
            bool any_return = false;
            if (!tainted) {
                // Forward must fixpoint over the extent: in = meet of in-extent preds' outs ({} at the
                // entry), out = in + every definite write in the block (a straight line accumulates
                // unconditionally, so order within the block is irrelevant).
                for (uint32_t b = 0; b < nb; b++) {
                    if (in_ext[b]) {
                        rc_bitset_copy(&mwin[b], &full);
                        rc_bitset_copy(&mwout[b], &full);
                    }
                }
                bool pass = true;
                while (pass) {
                    pass = false;
                    for (uint32_t b = 0; b < nb; b++) {
                        if (!in_ext[b]) {
                            continue;
                        }
                        if (b == e) {
                            rc_bitset_reset(&mrow);   // the routine starts here having written nothing
                        }
                        else {
                            rc_bitset_copy(&mrow, &full);
                            for (uint32_t p = 0; p < pred_count[b]; p++) {
                                uint32_t pb = preds[pred_first[b] + p];
                                if (in_ext[pb]) {
                                    rc_bitset_intersection(&mrow, &mwout[pb]);
                                }
                            }
                        }
                        if (!rc_bitset_is_equal(&mrow, &mwin[b])) {
                            rc_bitset_copy(&mwin[b], &mrow);
                            pass = true;
                        }
                        basic_block blk = rc_array_basic_block_get(&g.blocks, b);
                        for (uint32_t k = 0; k < blk.num_insns; k++) {
                            uint32_t ni = blk.first_insn + k;
                            zp_insn  n  = rc_view_zp_insn_get(insns, ni);
                            if (n.flow == zp_flow_call) {
                                rc_bitset ck = call_kill_bytes(calls[ni], mwb, nbytes, &scratch);
                                rc_bitset_union(&mrow, &ck);
                            }
                            if (n.vreg == RC_INDEX_NONE) {
                                continue;
                            }
                            touch_window w = insn_window(n, rc_view_zp_var_get(vars, n.vreg).width);
                            for (uint32_t i = 0; i < w.write_count; i++) {
                                rc_bitset_set(&mrow, base[n.vreg] + w.write_first + i);
                            }
                        }
                        if (!rc_bitset_is_equal(&mrow, &mwout[b])) {
                            rc_bitset_copy(&mwout[b], &mrow);
                            pass = true;
                        }
                    }
                }
                for (uint32_t b = 0; b < nb; b++) {
                    basic_block blk = rc_array_basic_block_get(&g.blocks, b);
                    if (!in_ext[b] || !block_returns(g, insns, cflows, blk)) {
                        continue;
                    }
                    // A returning exit carries whatever the routine wrote before handing back.
                    if (!any_return) {
                        rc_bitset_copy(&acc, &mwout[b]);
                        any_return = true;
                    }
                    else {
                        rc_bitset_intersection(&acc, &mwout[b]);
                    }
                }
            }
            if (!tainted && !any_return) {
                rc_bitset_copy(&acc, &full);   // never returns: vacuously writes everything
            }
            if (!rc_bitset_is_equal(&acc, &mwb[e])) {
                rc_bitset_copy(&mwb[e], &acc);
                mw_changed = true;
            }
        }
    }

    // Freeze each call's byte-level kill set, and project the var-level must-write rows for the finalize
    // sweep (a variable is killed only when EVERY one of its bytes is definitely written).
    rc_bitset *ckills = rc_arena_alloc_zero_type(&scratch, rc_bitset, insns.num);
    for (uint32_t i = 0; i < insns.num; i++) {
        if (rc_view_zp_insn_get(insns, i).flow == zp_flow_call) {
            ckills[i] = call_kill_bytes(calls[i], mwb, nbytes, &scratch);
        }
    }

    // ---- return edges ----
    // The dual of the call-input injection: a routine's RETURNING exits see everything live AFTER each of
    // its call sites, so a value written for the caller - an escaping result - stays live from its store
    // to the RTS, and the routine's own later writes (or a sibling local) cannot land on its byte. Without
    // this, the must-write kill would leave an escaping value dead the moment its producer stores it (its
    // only reads are in the caller, which intraprocedural liveness cannot see). ret_from[b] lists the call
    // sites whose callees' extents contain returning block b; after[i] snapshots the live set just after
    // call i, maintained inside the fixpoint below.
    rc_array_u32 *ret_from = rc_arena_alloc_zero_type(&scratch, rc_array_u32, nb);
    for (uint32_t i = 0; i < insns.num; i++) {
        for (uint32_t c = 0; c < calls[i].blocks.view.num; c++) {
            for (uint32_t b = 0; b < nb; b++) { in_ext[b] = false; }
            uint32_t sp = 0;
            stack[sp++] = rc_array_u32_get(&calls[i].blocks, c);
            in_ext[stack[0]] = true;
            while (sp > 0) {
                basic_block blk = rc_array_basic_block_get(&g.blocks, stack[--sp]);
                for (uint32_t s = 0; s < blk.succ_count; s++) {
                    uint32_t t = cfg_succ(g, blk, s);
                    if (!in_ext[t]) { in_ext[t] = true; stack[sp++] = t; }
                }
            }
            for (uint32_t b = 0; b < nb; b++) {
                if (in_ext[b] && block_returns(g, insns, cflows, rc_array_basic_block_get(&g.blocks, b))) {
                    rc_array_u32_push(&ret_from[b], i, &scratch);
                }
            }
        }
    }
    rc_bitset *after = rc_arena_alloc_zero_type(&scratch, rc_bitset, insns.num);
    for (uint32_t i = 0; i < insns.num; i++) {
        if (rc_view_zp_insn_get(insns, i).flow == zp_flow_call) {
            rc_bitset_resize(&after[i], nbytes, &scratch);
        }
    }
    for (uint32_t e = 0; e < nb; e++) {
        if (!is_entry[e]) {
            continue;
        }
        for (uint32_t v = 0; v < num_vars; v++) {
            uint16_t width = rc_view_zp_var_get(vars, v).width;
            bool     all   = true;
            for (uint32_t i = 0; i < width && all; i++) {
                all = rc_bitset_is_set(&mwb[e], base[v] + i);
            }
            if (all) {
                rc_bitset_set(&lv.must_write[e], v);
            }
        }
    }

    // Backward fixpoint over the byte sets. Round-robin in reverse block order (a backward analysis
    // converges fastest walking predecessors last); tiny CFGs, so no worklist. live_out = union of
    // successors' live_in (+ full if the block's exit is unknown); live_in comes from walking the block's
    // instructions backward - a call adds its callees' live-in, a write removes its provable bytes, a read
    // adds its bytes. (The walk replaces the use/kill set equations: a call's effect depends on another
    // block's evolving live-in, which a precomputed use set cannot express.)
    rc_bitset *bin  = make_rows(nb, nbytes, &scratch);
    rc_bitset *bout = make_rows(nb, nbytes, &scratch);
    rc_bitset new_out = {0}; rc_bitset_resize(&new_out, nbytes, &scratch);
    rc_bitset new_in  = {0}; rc_bitset_resize(&new_in,  nbytes, &scratch);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = nb; bi-- > 0; ) {
            basic_block block = rc_array_basic_block_get(&g.blocks, bi);
            rc_bitset_reset(&new_out);
            for (uint32_t k = 0; k < block.succ_count; k++) {
                rc_bitset_union(&new_out, &bin[cfg_succ(g, block, k)]);
            }
            if (block.unknown_succ) {
                rc_bitset_union(&new_out, &full);
            }
            for (uint32_t r = 0; r < ret_from[bi].view.num; r++) {
                // A returning block hands control back to each caller: what is live after those calls is
                // live here - the return edge that keeps an escaping result alive inside its producer.
                rc_bitset_union(&new_out, &after[rc_array_u32_get(&ret_from[bi], r)]);
            }
            rc_bitset_copy(&new_in, &new_out);
            for (uint32_t k = block.num_insns; k-- > 0; ) {
                uint32_t ni = block.first_insn + k;
                zp_insn  n  = rc_view_zp_insn_get(insns, ni);
                if (n.flow == zp_flow_call) {
                    // Snapshot the live-after set for the return edges, then apply the call's transfer:
                    // the callee's definite writes end the pre-call values; its inputs are consumed here.
                    // A grown snapshot forces another sweep - a return block earlier in THIS sweep read
                    // the stale one, and only the changed flag brings it back for the bigger set.
                    if (!rc_bitset_is_equal(&after[ni], &new_in)) {
                        rc_bitset_copy(&after[ni], &new_in);
                        changed = true;
                    }
                    for (uint32_t i = rc_bitset_get_first_set(&ckills[ni]); i != RC_INDEX_NONE;
                         i = rc_bitset_get_next_set(&ckills[ni], i + 1)) {
                        rc_bitset_clear(&new_in, i);
                    }
                    for (uint32_t c = 0; c < calls[ni].blocks.view.num; c++) {
                        rc_bitset_union(&new_in, &bin[rc_array_u32_get(&calls[ni].blocks, c)]);
                    }
                }
                if (n.vreg == RC_INDEX_NONE) {
                    continue;
                }
                touch_window w = insn_window(n, rc_view_zp_var_get(vars, n.vreg).width);
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_clear(&new_in, base[n.vreg] + w.write_first + i);
                }
                for (uint32_t i = 0; i < w.read_count; i++) {
                    rc_bitset_set(&new_in, base[n.vreg] + w.read_first + i);
                }
            }
            if (!rc_bitset_is_equal(&new_out, &bout[bi]) || !rc_bitset_is_equal(&new_in, &bin[bi])) {
                changed = true;
                rc_bitset_copy(&bout[bi], &new_out);
                rc_bitset_copy(&bin[bi], &new_in);
            }
        }
    }

    // Project the byte sets to the VARIABLE-level results the callers consume: a variable is live iff any
    // of its bytes is.
    for (uint32_t b = 0; b < nb; b++) {
        for (uint32_t i = rc_bitset_get_first_set(&bin[b]); i != RC_INDEX_NONE;
             i = rc_bitset_get_next_set(&bin[b], i + 1)) {
            rc_bitset_set(&lv.live_in[b], owner[i]);
        }
        for (uint32_t i = rc_bitset_get_first_set(&bout[b]); i != RC_INDEX_NONE;
             i = rc_bitset_get_next_set(&bout[b], i + 1)) {
            rc_bitset_set(&lv.live_out[b], owner[i]);
        }
    }

    // Interference, per instruction. Sweep each block backward from its byte live-out: at any WRITE the
    // variable is pinned to its bytes, so it interferes with the owner of every other live byte (indexed
    // and unknown-offset stores pin too, even though they kill nothing); then advance the live set to
    // before the instruction - remove the provably-rewritten bytes, add the read ones. A lone `STA ptr`
    // removes only the LSB's byte, so the MSB keeps the pointer live and overlap with it still registers;
    // once the MSB's store joins it the whole variable goes dead and the range genuinely ends. All the "in
    // reuses in's byte", "keep hits the whole callee footprint" legality falls out of these overlaps.
    rc_bitset live = {0}; rc_bitset_resize(&live, nbytes, &scratch);
    for (uint32_t b = 0; b < nb; b++) {
        basic_block block = rc_array_basic_block_get(&g.blocks, b);
        rc_bitset_copy(&live, &bout[b]);
        for (uint32_t k = block.num_insns; k-- > 0; ) {
            uint32_t ni = block.first_insn + k;
            zp_insn  n  = rc_view_zp_insn_get(insns, ni);
            if (n.flow == zp_flow_call) {
                // Same transfer as the dataflow: the callee's definite writes end the old values (a
                // delivered result's range starts here, not at the top of the caller), and its inputs are
                // consumed - live from here back to their stores, the range that lets an argument's bytes
                // register overlap with anything written in between.
                for (uint32_t i = rc_bitset_get_first_set(&ckills[ni]); i != RC_INDEX_NONE;
                     i = rc_bitset_get_next_set(&ckills[ni], i + 1)) {
                    rc_bitset_clear(&live, i);
                }
                for (uint32_t c = 0; c < calls[ni].blocks.view.num; c++) {
                    rc_bitset_union(&live, &bin[rc_array_u32_get(&calls[ni].blocks, c)]);
                }
            }
            if (n.vreg == RC_INDEX_NONE) {
                continue;
            }
            touch_window w = insn_window(n, rc_view_zp_var_get(vars, n.vreg).width);
            if (n.var_kill) {
                // A ZA_DISCARD ends the old value's range without storing anything: clear the bytes, pin
                // nothing - another variable may own them at this very instant, and that is the point.
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_clear(&live, base[n.vreg] + w.write_first + i);
                }
            }
            else if (n.rw & vref_write) {
                for (uint32_t o = rc_bitset_get_first_set(&live); o != RC_INDEX_NONE;
                     o = rc_bitset_get_next_set(&live, o + 1)) {
                    add_edge(lv.interfere, n.vreg, owner[o]);
                }
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_clear(&live, base[n.vreg] + w.write_first + i);
                }
            }
            for (uint32_t i = 0; i < w.read_count; i++) {
                rc_bitset_set(&live, base[n.vreg] + w.read_first + i);
            }
        }
    }
    // The routine's inputs are all simultaneously live at entry (the spec's synthetic ENTRY def = In(R)), so
    // they pairwise interfere - an edge no real-instruction def would create for a read-only input.
    if (entry_block < nb) {
        rc_bitset *lin = &bin[entry_block];
        for (uint32_t a = rc_bitset_get_first_set(lin); a != RC_INDEX_NONE; a = rc_bitset_get_next_set(lin, a + 1)) {
            for (uint32_t b = rc_bitset_get_next_set(lin, a + 1); b != RC_INDEX_NONE; b = rc_bitset_get_next_set(lin, b + 1)) {
                add_edge(lv.interfere, owner[a], owner[b]);
            }
        }
    }

    // Classification. Scan every instruction for whether each vreg is ever read / written, then combine with
    // its live-in-at-entry status (see vreg_class). input wins over temp/output when a var is both live-in
    // and rewritten (an in-out param); C3 will separate those with real caller liveness.
    rc_bitset seen_read  = {0}; rc_bitset_resize(&seen_read,  num_vars, &scratch);
    rc_bitset seen_write = {0}; rc_bitset_resize(&seen_write, num_vars, &scratch);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.vreg == RC_INDEX_NONE) {
            continue;
        }
        if (n.rw & vref_read)  { rc_bitset_set(&seen_read,  n.vreg); }
        if (n.rw & vref_write) { rc_bitset_set(&seen_write, n.vreg); }
    }
    for (uint32_t v = 0; v < num_vars; v++) {
        bool read    = rc_bitset_is_set(&seen_read, v);
        bool written = rc_bitset_is_set(&seen_write, v);
        bool live_in = entry_block < nb && rc_bitset_is_set(&lv.live_in[entry_block], v);
        if (!read && !written) {
            lv.classes[v] = vreg_class_unused;
        } else if (live_in) {
            lv.classes[v] = vreg_class_input;      // read before any write on some path from entry
        } else if (written && !read) {
            lv.classes[v] = vreg_class_output;     // written, never read inside -> escapes to the caller
        } else {
            lv.classes[v] = vreg_class_temp;       // written and read inside
        }
    }

    return lv;
}

bool liveness_interferes(const liveness *lv, uint32_t a, uint32_t b)
{
    RC_ASSERT(lv != NULL);
    return a != b && a < lv->num_vars && b < lv->num_vars && rc_bitset_is_set(&lv->interfere[a], b);
}

vreg_class liveness_class_of(const liveness *lv, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return vreg < lv->num_vars ? lv->classes[vreg] : vreg_class_unused;
}

bool liveness_is_live_in(const liveness *lv, uint32_t block, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return block < lv->num_blocks && vreg < lv->num_vars && rc_bitset_is_set(&lv->live_in[block], vreg);
}

bool liveness_is_live_out(const liveness *lv, uint32_t block, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return block < lv->num_blocks && vreg < lv->num_vars && rc_bitset_is_set(&lv->live_out[block], vreg);
}

// The bytes an instruction MAY write - the gate for the entry-input harvest below. The allocator's
// insn_window counts only PROVABLE redefinitions (an indexed store proves nothing, so it kills nothing);
// the input question is the opposite one - "could anything in the program have supplied this value?" -
// so here an indexed / unknown-offset store counts for its WHOLE variable (it may have written any
// byte), while a ZA_DISCARD counts for nothing at all: it declares the old value dead, it supplies no
// new one.
static touch_window may_write_window(zp_insn n, uint16_t width)
{
    touch_window w = {0};
    if (n.var_kill || !(n.rw & vref_write)) {
        return w;
    }
    bool direct = !n.var_indexed && n.var_offset != RC_INDEX_NONE && n.var_offset < width;
    w.write_first = direct ? n.var_offset : 0;
    w.write_count = direct ? 1u : width;
    return w;
}

// The shared plumbing of the entry-input walk's two phases: the graph and stream, the may-write
// summaries the flow consumes, and the working rows it fills (the footprint walk's context pattern).
typedef struct rbw_ctx {
    cfg              g;
    rc_view_zp_insn  insns;
    rc_view_zp_var   vars;
    call_targets    *calls;        // per-insn call arms (empty rows for non-calls)
    rc_bitset       *mayb;         // per-entry may-write summaries (phase 1's product)
    const uint32_t  *base;         // vreg -> first byte id
    const uint32_t  *preds, *pred_first, *pred_count;
    bool            *in_ext;      // the extent of the entry under analysis
    uint32_t        *stack;
    rc_bitset       *mayin, *mayout;   // per-block "bytes some route may have written by here"
    rc_bitset       *row;          // one scratch row
} rbw_ctx;

// One entry's forward may-write flow: walk its extent into in_ext, then run the union fixpoint into
// mayin/mayout - a byte is in mayin[b] once ANY route from the entry to b contains a write of it. The
// union meet needs no entry special case (an entry with no in-extent predecessors meets nothing = empty)
// and deliberately includes loop back edges. gen(b) = the block's own may-writes plus its callees'
// summaries (union over a call's arms - one writing arm is enough for "may"). Returns whether the
// extent is tainted by an unknown successor.
static bool may_write_flow(rbw_ctx *c, uint32_t e)
{
    uint32_t nb = c->g.blocks.num;
    for (uint32_t b = 0; b < nb; b++) { c->in_ext[b] = false; }
    uint32_t sp = 0;
    c->stack[sp++] = e;
    c->in_ext[e] = true;
    bool tainted = false;
    while (sp > 0) {
        basic_block blk = rc_array_basic_block_get(&c->g.blocks, c->stack[--sp]);
        tainted = tainted || blk.unknown_succ;
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            uint32_t t = cfg_succ(c->g, blk, s);
            if (!c->in_ext[t]) { c->in_ext[t] = true; c->stack[sp++] = t; }
        }
    }
    for (uint32_t b = 0; b < nb; b++) {
        if (c->in_ext[b]) {
            rc_bitset_reset(&c->mayin[b]);
            rc_bitset_reset(&c->mayout[b]);
        }
    }
    bool pass = true;
    while (pass) {
        pass = false;
        for (uint32_t b = 0; b < nb; b++) {
            if (!c->in_ext[b]) {
                continue;
            }
            rc_bitset_reset(c->row);
            for (uint32_t p = 0; p < c->pred_count[b]; p++) {
                uint32_t pb = c->preds[c->pred_first[b] + p];
                if (c->in_ext[pb]) {
                    rc_bitset_union(c->row, &c->mayout[pb]);
                }
            }
            if (!rc_bitset_is_equal(c->row, &c->mayin[b])) {
                rc_bitset_copy(&c->mayin[b], c->row);
                pass = true;
            }
            basic_block blk = rc_array_basic_block_get(&c->g.blocks, b);
            for (uint32_t k = 0; k < blk.num_insns; k++) {
                uint32_t ni = blk.first_insn + k;
                zp_insn  n  = rc_view_zp_insn_get(c->insns, ni);
                if (n.flow == zp_flow_call) {
                    for (uint32_t a = 0; a < c->calls[ni].blocks.view.num; a++) {
                        rc_bitset_union(c->row, &c->mayb[rc_array_u32_get(&c->calls[ni].blocks, a)]);
                    }
                }
                if (n.vreg == RC_INDEX_NONE) {
                    continue;
                }
                touch_window w = may_write_window(n, rc_view_zp_var_get(c->vars, n.vreg).width);
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_set(c->row, c->base[n.vreg] + w.write_first + i);
                }
            }
            if (!rc_bitset_is_equal(c->row, &c->mayout[b])) {
                rc_bitset_copy(&c->mayout[b], c->row);
                pass = true;
            }
        }
    }
    return tainted;
}

// The entry-input walk: which variables can a routine read while NOTHING in the program could yet have
// written them - values that can only have come from outside, which is fatal at an allocator-chosen
// address. Deliberately gated on MAY-write, not the sibling engine's must-write: a write on ANY route
// to the read silences it. A definite-assignment gate flagged correct programs whose guarding
// correlations no analysis of this IR can see - the flag-guarded init (writes skipped exactly when a
// "nothing to do" flag is set, reads guarded by the same flag) and the value-correlated dispatch
// (written under X==0, read only in the handler dispatched when X==0) - and drowned the real signal.
// The price, paid knowingly: purely loop-carried state (read at a loop's top, written only later inside
// it) is silenced by its own back edge. Calls apply summaries, not edges: a callee's may-writes extend
// the caller's set, and its own input set counts only where the caller's set does not already cover it
// - which keeps one call site's context out of another. Two phases, because an interleave would not
// converge cleanly: the may-write summaries grow to their fixpoint first, then - coverage frozen - the
// input summaries grow to theirs (interleaved, an early under-covered harvest could flag a read the
// final coverage silences, and a grow-only summary would never take it back).
rc_bitset liveness_read_before_write(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows,
                                     rc_view_zp_var vars, uint32_t root, rc_arena *arena, rc_arena scratch)
{
    uint32_t num_vars = vars.num;
    uint32_t nb = g.blocks.num;
    rc_bitset result = {0};
    rc_bitset_resize(&result, num_vars ? num_vars : 1, arena);
    if (nb == 0 || num_vars == 0 || root >= nb) {
        return result;
    }

    uint32_t *base = rc_arena_alloc_type(&scratch, uint32_t, num_vars);
    uint32_t nbytes = 0;
    for (uint32_t v = 0; v < num_vars; v++) {
        base[v] = nbytes;
        nbytes += rc_view_zp_var_get(vars, v).width;
    }
    uint32_t *owner = rc_arena_alloc_type(&scratch, uint32_t, nbytes);
    for (uint32_t v = 0; v < num_vars; v++) {
        for (uint32_t k = 0; k < rc_view_zp_var_get(vars, v).width; k++) {
            owner[base[v] + k] = v;
        }
    }

    call_targets *calls = rc_arena_alloc_zero_type(&scratch, call_targets, insns.num);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.flow == zp_flow_call) {
            calls[i] = cfg_call_targets(g, cflows, n, &scratch);
        }
    }

    // Summaries are needed for every call-target entry plus the root itself (the root is a routine too,
    // just one nothing inside the program calls).
    bool *is_entry = rc_arena_alloc_zero_type(&scratch, bool, nb);
    is_entry[root] = true;
    for (uint32_t i = 0; i < insns.num; i++) {
        for (uint32_t c = 0; c < calls[i].blocks.view.num; c++) {
            is_entry[rc_array_u32_get(&calls[i].blocks, c)] = true;
        }
    }

    uint32_t *pred_count = rc_arena_alloc_zero_type(&scratch, uint32_t, nb);
    uint32_t *pred_first = rc_arena_alloc_type(&scratch, uint32_t, nb);
    uint32_t  npreds     = 0;
    for (uint32_t b = 0; b < nb; b++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, b);
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            pred_count[cfg_succ(g, blk, s)]++;
        }
        npreds += blk.succ_count;
    }
    // Two separate loops, deliberately: the prefix sum reads pred_count[b - 1], so zeroing the counts
    // (they are reused as the fill cursors) must wait until the whole sum is done - fused into one loop,
    // each pred_first after the first came out zero and every block's preds landed on top of each other.
    for (uint32_t b = 0; b < nb; b++) {
        pred_first[b] = (b == 0) ? 0 : pred_first[b - 1] + pred_count[b - 1];
    }
    for (uint32_t b = 0; b < nb; b++) {
        pred_count[b] = 0;   // reused as the fill cursor
    }
    uint32_t *preds = npreds ? rc_arena_alloc_type(&scratch, uint32_t, npreds) : NULL;
    for (uint32_t b = 0; b < nb; b++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, b);
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            uint32_t t = cfg_succ(g, blk, s);
            preds[pred_first[t] + pred_count[t]++] = b;
        }
    }

    rc_bitset full = {0};
    rc_bitset_resize(&full, nbytes, &scratch);
    for (uint32_t i = 0; i < nbytes; i++) {
        rc_bitset_set(&full, i);
    }

    // mayb[e] = bytes routine e MAY have written by the time it returns (the union over its returning
    // routes); rbwb[e] = bytes some read in e consumes that nothing could yet have written. Entry rows
    // only; both grow from empty.
    rc_bitset *mayb   = make_rows(nb, nbytes, &scratch);
    rc_bitset *rbwb   = make_rows(nb, nbytes, &scratch);
    rc_bitset *mayin  = make_rows(nb, nbytes, &scratch);
    rc_bitset *mayout = make_rows(nb, nbytes, &scratch);
    rc_bitset  acc   = {0}; rc_bitset_resize(&acc, nbytes, &scratch);
    rc_bitset  row   = {0}; rc_bitset_resize(&row, nbytes, &scratch);
    rc_bitset  reads = {0}; rc_bitset_resize(&reads, nbytes, &scratch);
    bool      *in_ext = rc_arena_alloc_type(&scratch, bool, nb);
    uint32_t  *stack  = rc_arena_alloc_type(&scratch, uint32_t, nb);
    rbw_ctx c = {
        .g = g, .insns = insns, .vars = vars, .calls = calls, .mayb = mayb, .base = base,
        .preds = preds, .pred_first = pred_first, .pred_count = pred_count,
        .in_ext = in_ext, .stack = stack, .mayin = mayin, .mayout = mayout, .row = &row,
    };

    // Phase 1: the may-write summaries, to their fixpoint - monotone, since a callee's growing summary
    // only ever grows its callers' flows. A routine's summary is the union over its RETURNING exits
    // (a write on a never-returning path cannot precede a post-call read); no returning path at all
    // vacuously covers everything (the caller's continuation never runs). A tainted extent contributes
    // nothing - Guard 1 refuses the program anyway, no point stacking warnings on top of its error.
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t e = 0; e < nb; e++) {
            if (!is_entry[e]) {
                continue;
            }
            bool tainted = may_write_flow(&c, e);
            rc_bitset_reset(&acc);
            if (!tainted) {
                bool any_return = false;
                for (uint32_t b = 0; b < nb; b++) {
                    basic_block blk = rc_array_basic_block_get(&g.blocks, b);
                    if (in_ext[b] && block_returns(g, insns, cflows, blk)) {
                        rc_bitset_union(&acc, &mayout[b]);
                        any_return = true;
                    }
                }
                if (!any_return) {
                    rc_bitset_copy(&acc, &full);
                }
            }
            if (!rc_bitset_is_equal(&acc, &mayb[e])) {
                rc_bitset_copy(&mayb[e], &acc);
                changed = true;
            }
        }
    }

    // Phase 2: the input harvest, to its own fixpoint with the coverage frozen. Replay each block from
    // its stable may-in and collect the reads that land outside the running set - checking each
    // instruction's reads BEFORE applying its writes, so a read-modify-write consumes the old value. A
    // callee contributes its own input set, filtered by what this caller may already have covered, then
    // extends the running set with its may-writes. Reads come from insn_window (its read side is the
    // consumption question, unchanged); only the coverage side uses the may window.
    changed = true;
    while (changed) {
        changed = false;
        for (uint32_t e = 0; e < nb; e++) {
            if (!is_entry[e]) {
                continue;
            }
            bool tainted = may_write_flow(&c, e);
            rc_bitset_reset(&reads);
            if (!tainted) {
                for (uint32_t b = 0; b < nb; b++) {
                    if (!in_ext[b]) {
                        continue;
                    }
                    rc_bitset_copy(&acc, &mayin[b]);
                    basic_block blk = rc_array_basic_block_get(&g.blocks, b);
                    for (uint32_t k = 0; k < blk.num_insns; k++) {
                        uint32_t ni = blk.first_insn + k;
                        zp_insn  n  = rc_view_zp_insn_get(insns, ni);
                        if (n.flow == zp_flow_call) {
                            for (uint32_t a = 0; a < calls[ni].blocks.view.num; a++) {
                                rc_bitset *crbw = &rbwb[rc_array_u32_get(&calls[ni].blocks, a)];
                                for (uint32_t bit = rc_bitset_get_first_set(crbw); bit != RC_INDEX_NONE;
                                     bit = rc_bitset_get_next_set(crbw, bit + 1)) {
                                    if (!rc_bitset_is_set(&acc, bit)) {
                                        rc_bitset_set(&reads, bit);
                                    }
                                }
                            }
                            for (uint32_t a = 0; a < calls[ni].blocks.view.num; a++) {
                                rc_bitset_union(&acc, &mayb[rc_array_u32_get(&calls[ni].blocks, a)]);
                            }
                        }
                        if (n.vreg == RC_INDEX_NONE) {
                            continue;
                        }
                        uint16_t width = rc_view_zp_var_get(vars, n.vreg).width;
                        touch_window r = insn_window(n, width);
                        for (uint32_t i = 0; i < r.read_count; i++) {
                            uint32_t bit = base[n.vreg] + r.read_first + i;
                            if (!rc_bitset_is_set(&acc, bit)) {
                                rc_bitset_set(&reads, bit);
                            }
                        }
                        touch_window w = may_write_window(n, width);
                        for (uint32_t i = 0; i < w.write_count; i++) {
                            rc_bitset_set(&acc, base[n.vreg] + w.write_first + i);
                        }
                    }
                }
            }
            rc_bitset_union(&reads, &rbwb[e]);   // an ascending chain now the coverage is frozen
            if (!rc_bitset_is_equal(&reads, &rbwb[e])) {
                rc_bitset_copy(&rbwb[e], &reads);
                changed = true;
            }
        }
    }

    for (uint32_t bit = rc_bitset_get_first_set(&rbwb[root]); bit != RC_INDEX_NONE;
         bit = rc_bitset_get_next_set(&rbwb[root], bit + 1)) {
        rc_bitset_set(&result, owner[bit]);
    }
    return result;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Append one instruction with an explicit vreg + rw and return the running pc, so a test writes a stream as a
// column of touches without hand-tracking addresses. vreg RC_INDEX_NONE / rw vref_none = a non-var insn.
static uint32_t touch(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, zp_flow flow, uint32_t target,
                      uint32_t vreg, uint8_t rw, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = rw, .vreg = vreg, .target = target,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + size;
}

// A registry of `n` one-byte variables (vreg i = vars[i]) - the common case; a test that needs a wider
// variable builds its own registry.
static rc_view_zp_var width1_vars(uint32_t n, rc_arena *arena)
{
    rc_array_zp_var vars = rc_array_zp_var_make(n ? n : 1, arena);
    for (uint32_t i = 0; i < n; i++) {
        rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("v"), .scope = 0, .width = 1, .def = (cursor) {0}},
                             arena);
    }
    return vars.view;
}

// The spec's `mul` routine: vreg 0 = in1 (read-only), 1 = tmp (write then read), 2 = out1 (write-only).
//   LDA in1 : ASL A : STA tmp : LDA in1 : CLC : ADC tmp : STA out1 : RTS
static rc_array_zp_insn build_mul(rc_arena *arena)
{
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,          arena);   // LDA in1
    pc = touch(&insns, pc, 1, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // ASL A
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write,         arena);   // STA tmp
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,          arena);   // LDA in1
    pc = touch(&insns, pc, 1, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // CLC
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,          arena);   // ADC tmp
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 2, vref_write,         arena);   // STA out1
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // RTS
    (void) pc;
    return insns;
}

RC_TEST(liveness, mul_interference)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = build_mul(&arena);
    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(3, &arena), 0, &arena, scratch);

    // in1 overlaps tmp (both live at n3..n4) -> they interfere. But in1 dies before out1 is born, and tmp
    // dies before out1 is born, so out1 reuses their bytes - no interference. This IS the reuse fact.
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // in1 - tmp
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 2));   // in1 - out1
    RC_CHECK_FALSE(liveness_interferes(&lv, 1, 2));   // tmp - out1

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, mul_classification)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = build_mul(&arena);
    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(3, &arena), 0, &arena, scratch);

    RC_CHECK_TRUE(liveness_class_of(&lv, 0) == vreg_class_input);    // in1 read before written
    RC_CHECK_TRUE(liveness_class_of(&lv, 1) == vreg_class_temp);     // tmp born and consumed inside
    RC_CHECK_TRUE(liveness_class_of(&lv, 2) == vreg_class_output);   // out1 written, never read inside
    RC_CHECK_TRUE(liveness_is_live_in(&lv, 0, 0));                   // in1 is live on entry...
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 1));                  // ...tmp and out1 are not
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 2));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, disjoint_locals_reuse_a_byte)
{
    // The user's headline case: STA var1 : LDA var1 : STA var2 : LDA var2. var1 dies before var2 is born, so
    // their live ranges are disjoint and they can share one zero-page byte.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA var1
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA var1
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA var2
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,  &arena);   // LDA var2
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 1));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, loop_carries_value_across_back_edge)
{
    // A back-edge forces the fixpoint to iterate: v0 is read at the loop top and never written, so it must be
    // live-in at the top on every iteration (a loop-invariant input), which only holds once live-out of the
    // branch block feeds live-in of the top back in.
    //   2000 .loop LDA v0   (read v0)          <- branch target
    //   2002       STA v1   (write v1)
    //   2004       BNE 2000 (branch back; fall-through 2006)
    //   2006       RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA v0
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA v1
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x2000,        RC_INDEX_NONE, vref_none, &arena);   // BNE loop
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);
    RC_CHECK_TRUE(liveness_is_live_in(&lv, 0, 0));    // v0 live around the loop
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // and still live out (back-edge carries it)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

// touch() with an explicit byte window: `offset` into the variable, `indirect` for a 2-byte pointer deref.
static uint32_t touch_at(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, uint32_t vreg, uint8_t rw,
                         uint32_t offset, bool indirect, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = zp_flow_normal, .rw = rw, .vreg = vreg,
                   .var_offset = offset, .var_indirect = indirect, .target = RC_INDEX_NONE,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + size;
}

// A 2-byte pointer (vreg 0) and a 1-byte temp (vreg 1), for the partial-write tests.
static rc_view_zp_var pointer_and_temp(rc_arena *arena)
{
    rc_array_zp_var vars = rc_array_zp_var_make(2, arena);
    rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("p"), .scope = 0, .width = 2, .def = (cursor) {0}}, arena);
    rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("t"), .scope = 0, .width = 1, .def = (cursor) {0}}, arena);
    return vars.view;
}

RC_TEST(liveness, partial_write_keeps_a_pointer_live)
{
    // The demo.6502 shape: a 2-byte pointer p whose MSB is written once up front, while a loop rewrites
    // only the LSB before each deref. The LSB store redefines one byte - the MSB's old value flows through
    // it - so p stays live around the whole loop and must interfere with the loop temp t, or an
    // overlapping allocation would clobber the MSB. (A per-variable kill rule treated the LSB store as a
    // full redefinition, severed the range, and let t share p's bytes - the demo's broken globe.)
    //   2000        STA p+1   (the init)
    //   2002 .loop  STA p     (LSB only)
    //   2004        LDA (p),Y (deref: reads both bytes)
    //   2006        STA t : LDA t
    //   200A        BNE loop
    //   200C        RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch_at(&insns, pc, 2, 0, vref_write, 1, false, &arena);   // STA p+1
    pc = touch_at(&insns, pc, 2, 0, vref_write, 0, false, &arena);   // STA p
    pc = touch_at(&insns, pc, 2, 0, vref_read,  0, true,  &arena);   // LDA (p),Y
    pc = touch_at(&insns, pc, 2, 1, vref_write, 0, false, &arena);   // STA t
    pc = touch_at(&insns, pc, 2, 1, vref_read,  0, false, &arena);   // LDA t
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x2002, RC_INDEX_NONE, vref_none, &arena);   // BNE loop
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, pointer_and_temp(&arena), 0, &arena, scratch);
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));                     // the MSB rides through STA p
    RC_CHECK_TRUE(liveness_is_live_in(&lv, cfg_block_at(g, 0, 0x2002), 0));   // p live around the loop

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, full_byte_rewrite_kills_a_pointer)
{
    // The contrast: when BOTH bytes are stored before the deref, the per-byte kills accumulate into a
    // full redefinition, the old range genuinely ends, and a temp that dies beforehand still shares the
    // pointer's bytes. (So yes - partial writes across the width of a variable are tracked, and a total
    // rewrite is a kill.)
    //   2000  STA t : LDA t          (t's whole life)
    //   2004  STA p : STA p+1        (a full rewrite, byte by byte)
    //   2008  LDA (p),Y : RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch_at(&insns, pc, 2, 1, vref_write, 0, false, &arena);   // STA t
    pc = touch_at(&insns, pc, 2, 1, vref_read,  0, false, &arena);   // LDA t
    pc = touch_at(&insns, pc, 2, 0, vref_write, 0, false, &arena);   // STA p
    pc = touch_at(&insns, pc, 2, 0, vref_write, 1, false, &arena);   // STA p+1
    pc = touch_at(&insns, pc, 2, 0, vref_read,  0, true,  &arena);   // LDA (p),Y
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, pointer_and_temp(&arena), 0, &arena, scratch);
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 1));   // t is dead before the rewrite begins - reuse is safe
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 0));   // and nothing of p's old value flows in

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

// Push a ZA_DISCARD marker for `vreg`: size 0, no rw - the tests pre-resolve vregs, so identity fields stay 0.
static uint32_t kill_marker(rc_array_zp_insn *insns, uint32_t pc, uint32_t vreg, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = 0, .flow = zp_flow_normal, .rw = vref_none, .vreg = vreg,
                   .var_kill = true, .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE,
                   .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc;
}

RC_TEST(liveness, za_discard_kills_without_pinning)
{
    // v0 is read with no prior write, so it is normally live-in from the top - and the STA v1 above the
    // read would pin v1 against it. A ZA_DISCARD between them promises the inflowing value is dead: the
    // range is severed, v0 is no longer live-in, and crucially the marker itself pins nothing (it is a
    // promise, not a store - v1 may own the byte at that instant).
    //   2000  STA v1
    //   2002  (ZA_DISCARD v0)
    //   2002  LDA v0
    //   2004  RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA v1
    kill_marker(&insns, pc, 0, &arena);                                                // ZA_DISCARD v0
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA v0
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 0));   // the inflowing value is declared dead
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 1));   // so the earlier store pins nothing against v0

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, za_discard_covers_every_byte_and_feeds_must_write)
{
    // A 2-byte pointer read with no provable write leaks both bytes to live-in; one ZA_DISCARD covers the
    // whole width. And inside a CALLED routine the marker counts as a definite rewrite: the caller's
    // pre-call value dies at the JSR, exactly as if the callee had provably stored every byte.
    //   caller 2000: JSR 3000 ; LDA (p),Y ; RTS      callee 3000: (ZA_DISCARD p) ; RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena);   // JSR 3000
    rc_array_zp_insn_push(&insns,
        (zp_insn) {.pc = pc, .size = 2, .flow = zp_flow_normal, .rw = vref_read, .vreg = 0,
                   .var_indirect = true, .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE,
                   .target_def = cursor_none(), .at = (cursor) {0}},
        &arena);                                                                          // LDA (p),Y
    pc += 2;
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    kill_marker(&insns, 0x3000, 0, &arena);                                               // ZA_DISCARD p
    touch(&insns, 0x3000, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, pointer_and_temp(&arena), 0, &arena, scratch);
    uint32_t callee = cfg_block_at(g, 0, 0x3000);
    RC_CHECK_TRUE(callee != RC_INDEX_NONE);
    RC_CHECK_TRUE(rc_bitset_is_set(&lv.must_write[callee], 0));   // the ZA_DISCARD is a definite full rewrite
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 0));               // so p's range starts AT the call, not before

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, za_return_block_is_returning_exit)
{
    // A callee that exits via an annotated computed jump (the inline-data idiom: pop the return address,
    // consume the data, JMP past it) must behave exactly like one ending in RTS: its writes count for
    // must-write (the caller's pre-call value dies at the JSR) and the caller's live-after set flows into
    // the annotated block through the return edges (an escaping result stays live to the exit).
    //   caller 2000: STA keep ; JSR 3000 ; LDA keep ; LDA res ; RTS      (keep = v0, res = v1)
    //   callee 3000: STA res ; JMP (ind)  <- ZA_RETURN
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA keep
    pc = touch(&insns, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena); // JSR 3000
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA keep
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,  &arena);   // LDA res
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    pc = 0x3000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA res
    pc = touch(&insns, pc, 3, zp_flow_jump, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // JMP (ind)
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(1, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x3002, .target = RC_INDEX_NONE,
                                                .kind = zp_cflow_za_return}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, cflows.view, width1_vars(2, &arena), RC_INDEX_NONE, &arena, scratch);
    uint32_t callee = cfg_block_at(g, 0, 0x3000);
    RC_CHECK_TRUE(callee != RC_INDEX_NONE);
    RC_CHECK_TRUE(rc_bitset_is_set(&lv.must_write[callee], 1));   // the exit counts as a returning path
    RC_CHECK_TRUE(liveness_is_live_out(&lv, callee, 1));          // res rides the return edge to the caller
    RC_CHECK_TRUE(liveness_is_live_out(&lv, callee, 0));          // keep is live after the call, so here too

    // Without the annotation the computed exit taints: must-write forfeits the lot (the contrast that
    // proves ZA_RETURN is doing the work).
    cfg gu = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lt = liveness_analyze(gu, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), RC_INDEX_NONE, &arena, scratch);
    RC_CHECK_FALSE(rc_bitset_is_set(&lt.must_write[cfg_block_at(gu, 0, 0x3000)], 1));

    // The branch flavour: a CONDITIONAL return past the data. The annotated block is a returning exit AND
    // keeps its fall-through successor; both paths definitely write res, so the must-write intersection
    // still holds it.
    //   callee 3000: STA res ; BNE <self-modified> <- ZA_RETURN ; RTS
    rc_array_zp_insn cond = rc_array_zp_insn_make(8, &arena);
    pc = 0x2000;
    pc = touch(&cond, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA keep
    pc = touch(&cond, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena); // JSR 3000
    pc = touch(&cond, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA keep
    pc = touch(&cond, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,  &arena);   // LDA res
    pc = touch(&cond, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    pc = 0x3000;
    pc = touch(&cond, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA res
    pc = touch(&cond, pc, 2, zp_flow_branch, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // BNE (self-mod)
    pc = touch(&cond, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;
    cfg gc = cfg_build(cond.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lc = liveness_analyze(gc, cond.view, cflows.view, width1_vars(2, &arena), RC_INDEX_NONE, &arena, scratch);
    uint32_t centry = cfg_block_at(gc, 0, 0x3000);
    basic_block cb = rc_array_basic_block_get(&gc.blocks, centry);
    RC_CHECK(cb.succ_count, ==, 1u);   // the not-taken edge to the RTS block survives...
    RC_CHECK_FALSE(cb.unknown_succ);   // ...and the self-modified operand does not taint
    RC_CHECK_TRUE(rc_bitset_is_set(&lc.must_write[centry], 1));
    RC_CHECK_TRUE(liveness_is_live_out(&lc, centry, 1));   // res live at the conditional exit too

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, unknown_successor_keeps_everything_live)
{
    // The conservative taint end to end: a var defined just before an indirect jump must be assumed live out
    // (the computed target might read it), so it interferes with every other var - reuse is refused rather
    // than an unsound allocation emitted.
    //   2000 STA v0    (write v0)
    //   2002 JMP (ind) (jump, target unknown -> unknown_succ)
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA v0
    pc = touch(&insns, pc, 3, zp_flow_jump,   RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // JMP (ind)
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);   // v1 exists in the id space, unused
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // v0 forced live out by the taint...
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 1));   // ...as is v1
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // so v0's def collides with v1 - no reuse across it

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, diamond_meet_reads_real_predecessors)
{
    // A regression for the predecessor-list build: the prefix sum once read counts its own loop had
    // already zeroed, so every pred_first came out 0 and the lists landed on top of one another. In this
    // diamond the empty arm's recorded "predecessor" was then ITSELF, which parked its definite-write set
    // at the FULL seed - so the join's meet claimed v written on every path, killing a caller's live
    // value at the JSR. Small straight-line programs dodged the scatter by luck; the diamond does not.
    //   caller 2000: JSR 3000 ; RTS
    //   callee 3000: BEQ 3008 ; JMP 300A ; 3008: STA v ; 300A: LDA v ; RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena);       // JSR 3000
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    pc = 0x3000;
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x3008, RC_INDEX_NONE, vref_none, &arena);     // BEQ 3008
    pc = touch(&insns, pc, 3, zp_flow_jump, 0x300A, RC_INDEX_NONE, vref_none, &arena);       // JMP 300A
    pc = 0x3008;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);         // STA v
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read, &arena);          // LDA v @300A
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(1, &arena), 0, &arena, scratch);
    uint32_t callee = cfg_block_at(g, 0, 0x3000);
    RC_CHECK_TRUE(callee != RC_INDEX_NONE);
    RC_CHECK_FALSE(rc_bitset_is_set(&lv.must_write[callee], 0));   // one arm writes nothing: not definite

    // The entry-input walk sees the same diamond through its MAY gate: the STA arm writes v on one
    // route to the join, so the join's read is no external input - even as must-write rightly refuses
    // the definite claim above. The two engines answer opposite questions about one shape.
    rc_bitset inputs = liveness_read_before_write(g, insns.view, (rc_view_zp_cflow) {0},
                                                  width1_vars(1, &arena), callee, &arena, scratch);
    RC_CHECK_FALSE(rc_bitset_is_set(&inputs, 0));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, entry_inputs_are_may_write_gated)
{
    // The entry-input walk's four corners. It warns only when NO route in the program could have written
    // the value before the read: a plain read-before-anything IS an input; an indexed-only init is NOT
    // (the store proves no byte, but may have written any); a ZA_DISCARD supplies nothing, so a read
    // after one still IS an input; and loop-carried state is silenced by its own back edge - the
    // documented price of the may gate.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing

    // 2000: LDA v ; RTS -> an input.
    rc_array_zp_insn plain = rc_array_zp_insn_make(4, &arena);
    touch(&plain, 0x2000, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read, &arena);
    touch(&plain, 0x2002, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    cfg g1 = cfg_build(plain.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_bitset in1 = liveness_read_before_write(g1, plain.view, (rc_view_zp_cflow) {0},
                                               width1_vars(1, &arena), 0, &arena, scratch);
    RC_CHECK_TRUE(rc_bitset_is_set(&in1, 0));

    // 2000: STA v,X ; LDA v ; RTS -> not an input (the indexed store may have supplied it).
    rc_array_zp_insn idx = rc_array_zp_insn_make(4, &arena);
    rc_array_zp_insn_push(&idx,
        (zp_insn) {.pc = 0x2000, .size = 2, .flow = zp_flow_normal, .rw = vref_write, .vreg = 0,
                   .var_indexed = true, .var_offset = RC_INDEX_NONE, .target = RC_INDEX_NONE,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        &arena);
    touch(&idx, 0x2002, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read, &arena);
    touch(&idx, 0x2004, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    cfg g2 = cfg_build(idx.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_bitset in2 = liveness_read_before_write(g2, idx.view, (rc_view_zp_cflow) {0},
                                               width1_vars(1, &arena), 0, &arena, scratch);
    RC_CHECK_FALSE(rc_bitset_is_set(&in2, 0));

    // 2000: (ZA_DISCARD v) ; LDA v ; RTS -> still an input: a discard declares the old value dead,
    // it does not supply a new one.
    rc_array_zp_insn disc = rc_array_zp_insn_make(4, &arena);
    kill_marker(&disc, 0x2000, 0, &arena);
    touch(&disc, 0x2000, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read, &arena);
    touch(&disc, 0x2002, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    cfg g3 = cfg_build(disc.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_bitset in3 = liveness_read_before_write(g3, disc.view, (rc_view_zp_cflow) {0},
                                               width1_vars(1, &arena), 0, &arena, scratch);
    RC_CHECK_TRUE(rc_bitset_is_set(&in3, 0));

    // 2000: .top LDA v ; STA v ; BNE top ; RTS -> silenced: the back edge carries the write around to
    // the read, so "some route wrote it first" holds even though iteration one reads it cold.
    rc_array_zp_insn loop = rc_array_zp_insn_make(6, &arena);
    touch(&loop, 0x2000, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read, &arena);
    touch(&loop, 0x2002, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);
    touch(&loop, 0x2004, 2, zp_flow_branch, 0x2000, RC_INDEX_NONE, vref_none, &arena);
    touch(&loop, 0x2006, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    cfg g4 = cfg_build(loop.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_bitset in4 = liveness_read_before_write(g4, loop.view, (rc_view_zp_cflow) {0},
                                               width1_vars(1, &arena), 0, &arena, scratch);
    RC_CHECK_FALSE(rc_bitset_is_set(&in4, 0));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, skip_swallowed_write_is_branch_path_only)
{
    // The BIT-skip trick, seen by must-write: the callee's STA keep is swallowed by a BITABS, so it runs
    // only when the BEQ takes - the fall-through path returns WITHOUT writing keep, and the intersection
    // over returning paths must come up empty. The contrast stream drops the marker (plain data in the
    // gap): the gap-skip fall-through then routes THROUGH the store, and must-write claims it - which is
    // exactly the unsoundness the marker exists to prevent.
    //   caller 2000: JSR 3000 ; RTS
    //   callee 3000: BEQ 3005 ; LDA # ; BITABS ; 3005: STA keep ; 3007: RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena);     // JSR 3000
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    pc = 0x3000;
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x3005, RC_INDEX_NONE, vref_none, &arena);   // BEQ 3005
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // LDA #
    rc_array_zp_insn_push(&insns,
        (zp_insn) {.pc = pc, .size = 1, .flow = zp_flow_skip, .skip_bytes = 2, .rw = vref_none,
                   .vreg = RC_INDEX_NONE, .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE,
                   .target_def = cursor_none(), .at = (cursor) {0}},
        &arena);                                                                            // BITABS @3004
    pc += 1;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);        // STA keep @3005
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS @3007
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(1, &arena), 0, &arena, scratch);
    uint32_t callee = cfg_block_at(g, 0, 0x3000);
    RC_CHECK_TRUE(callee != RC_INDEX_NONE);
    RC_CHECK_FALSE(rc_bitset_is_set(&lv.must_write[callee], 0));   // the store is NOT on every returning path

    // The contrast: same program with plain data where the marker was. The gap-skip edge routes the
    // fall-through through the store, so must-write (wrongly, for the real trick) claims keep.
    rc_array_zp_insn bare = rc_array_zp_insn_make(8, &arena);
    pc = 0x2000;
    pc = touch(&bare, pc, 3, zp_flow_call, 0x3000, RC_INDEX_NONE, vref_none, &arena);      // JSR 3000
    pc = touch(&bare, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    pc = 0x3000;
    pc = touch(&bare, pc, 2, zp_flow_branch, 0x3005, RC_INDEX_NONE, vref_none, &arena);    // BEQ 3005
    pc = touch(&bare, pc, 2, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);    // LDA #
    pc = 0x3005;                                                                            // a data byte @3004
    pc = touch(&bare, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);         // STA keep @3005
    pc = touch(&bare, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);    // RTS @3007
    (void) pc;

    cfg gb = cfg_build(bare.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    liveness lb = liveness_analyze(gb, bare.view, (rc_view_zp_cflow) {0}, width1_vars(1, &arena), 0, &arena, scratch);
    uint32_t cb = cfg_block_at(gb, 0, 0x3000);
    RC_CHECK_TRUE(cb != RC_INDEX_NONE);
    RC_CHECK_TRUE(rc_bitset_is_set(&lb.must_write[cb], 0));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
