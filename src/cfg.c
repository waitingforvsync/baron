#include "cfg.h"

#include "richc/bitset.h"
#include "richc/macros.h"


enum {
    cfg_addr_space = 0x10000,   // the 6502's 64 KB - the leader bitset spans one bit per address, PER section
};


// A resolved target location: which (section, pc) a control transfer lands on. `found` is false for a computed
// / unregistered / absent target (the caller then leaves the edge unknown).
typedef struct target_loc {
    uint32_t section;
    uint32_t pc;
    bool     found;
} target_loc;

// Where does `n`'s control-transfer target land? A named-label target (target_scope set) resolves through the
// markers to the label's OWN section - this is what crosses a section, because the label picks the bank the
// raw address never could. If it named no label, or a label with no marker (a bare constant), we fall back to
// the numeric target, resolved WITHIN n's own section only. A computed / absent target is `found = false` -
// and so is ANY indirect jump: its operand names the vector cell it dispatches through, never the place
// control lands, so there is no direct location to hand back.
static target_loc resolve_target_loc(rc_view_zp_label labels, zp_insn n)
{
    if (n.target_via != zp_target_via_direct) {
        return (target_loc) {.section = 0, .pc = 0, .found = false};
    }
    if (n.target_scope != RC_INDEX_NONE) {
        for (uint32_t i = 0; i < labels.num; i++) {
            zp_label l = rc_view_zp_label_get(labels, i);
            if (l.scope == n.target_scope && cursor_is_equal(l.def, n.target_def)) {
                return (target_loc) {.section = l.section, .pc = l.pc, .found = true};
            }
        }
        // A symbolic target with no marker (e.g. a `name = expr` constant, not a code label): treat the
        // resolved value as a bare address in n's own section, exactly like a numeric target.
    }
    if (n.target != RC_INDEX_NONE) {
        return (target_loc) {.section = n.section, .pc = n.target, .found = true};
    }
    return (target_loc) {.section = 0, .pc = 0, .found = false};
}

// The leader-set index for (section, pc): one 64 KB span of bits per section. Callers only pass sections that
// exist in the stream (< num_sections) and pcs < cfg_addr_space, so the index is always in range.
static uint32_t leader_index(uint32_t section, uint32_t pc)
{
    return section * cfg_addr_space + pc;
}

// Is (section, pc) the leader (entry) of a basic block? Leaders come from: the first instruction, every
// section boundary, every branch/jump/call TARGET, and the instruction after any branch / jump / return. A
// JSR does NOT boundary its fall-through (a call is in-block) but its target IS a leader (the callee's entry).
static bool addr_is_leader(const rc_bitset *leaders, uint32_t section, uint32_t pc)
{
    return pc < cfg_addr_space && rc_bitset_is_set(leaders, leader_index(section, pc));
}

// Mark (section, pc) a leader, ignoring a pc past 0xFFFF (a target past the address space, or the fall-through
// of the last byte of memory - neither names a real block).
static void mark_leader(rc_bitset *leaders, uint32_t section, uint32_t pc)
{
    if (pc < cfg_addr_space) {
        rc_bitset_set(leaders, leader_index(section, pc));
    }
}

// The block whose entry is exactly (section, pc), or RC_INDEX_NONE. The list is short (one per leader), so a
// linear scan is fine. Turns a fall-through / in-section target LOCATION into a successor block INDEX.
static uint32_t block_at(rc_view_basic_block blocks, uint32_t section, uint32_t pc)
{
    if (pc == RC_INDEX_NONE) {
        return RC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < blocks.num; i++) {
        basic_block b = rc_view_basic_block_get(blocks, i);
        if (b.section == section && b.pc == pc) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

uint32_t cfg_block_at(cfg g, uint32_t section, uint32_t pc)
{
    return block_at(g.blocks.view, section, pc);
}

// Does `n`'s control transfer leave the assembled program for EXTERNAL code (an OS or ROM entry)? Only
// meaningful once the caller has failed to place the target as a block. The policy: a destination we can pin
// to a constant address, yet which matches nothing we assembled, is a transfer OUT of the program - and
// external code cannot touch a ZA_AUTO variable, because ZA_POOL names precisely the bytes nothing outside
// the program uses. So a JSR &FFEE is a benign call with an empty footprint and a JMP (&FFFC) a clean exit,
// no annotation required. What stays conservative is flow whose destination we genuinely cannot pin down:
// a jump through a vector WE assembled (its contents may point back into our own code), an indexed dispatch
// table, or an unresolved target.
static bool target_is_external(rc_view_zp_label labels, zp_insn n)
{
    switch (n.target_via) {
        case zp_target_via_direct:
            // A known constant (or a named non-label constant, or even a label off the code stream): the
            // caller found no block there, so it points outside the program.
            return resolve_target_loc(labels, n).found;
        case zp_target_via_vector:
            // A literal vector address (JMP (&FFFC)) - or a named constant standing for one (wrchv = &20E) -
            // is a cell outside the program: whatever it dispatches to is by policy external. A vector that
            // IS one of our labels - or a ZA_AUTO variable the allocator owns - is a cell we assembled, whose
            // run-time contents may point anywhere, including back at us - that stays computed (annotate
            // with ZA_CANJUMP).
            if (n.target_is_zpvar) {
                return false;
            }
            if (n.target_scope == RC_INDEX_NONE) {
                return true;
            }
            for (uint32_t i = 0; i < labels.num; i++) {
                zp_label l = rc_view_zp_label_get(labels, i);
                if (l.scope == n.target_scope && cursor_is_equal(l.def, n.target_def)) {
                    return false;
                }
            }
            return true;
        case zp_target_via_table:
        default:
            return false;   // JMP (table,X): an indexed dispatch through our own memory - always computed
    }
}

bool cfg_target_is_external(cfg g, zp_insn n)
{
    return target_is_external(g.labels, n);
}

uint32_t cfg_target_block(cfg g, zp_insn n)
{
    target_loc tl = resolve_target_loc(g.labels, n);
    return tl.found ? block_at(g.blocks.view, tl.section, tl.pc) : RC_INDEX_NONE;
}

uint32_t cfg_succ(cfg g, basic_block b, uint32_t i)
{
    RC_ASSERT(i < b.succ_count);
    return rc_view_u32_get(g.succs.view, b.succ_first + i);
}

call_targets cfg_call_targets(cfg g, rc_view_zp_cflow cflows, zp_insn n, rc_arena *arena)
{
    call_targets t = {.blocks = rc_array_u32_make(4, arena), .unknown = false, .external = false};

    bool annotated = false;
    for (uint32_t j = 0; j < cflows.num; j++) {
        zp_cflow cf = rc_view_zp_cflow_get(cflows, j);
        if (cf.kind == zp_cflow_za_cancall && cf.site == n.pc) {
            annotated = true;
            // ZA_CANCALL names a same-section address. Every declared target is a block leader, so an
            // in-program address always has a block; one without is an external arm and contributes nothing.
            uint32_t tb = cfg_block_at(g, n.section, cf.target);
            if (tb != RC_INDEX_NONE) {
                rc_array_u32_push(&t.blocks, tb, arena);
            }
            else {
                t.external = true;
            }
        }
    }
    if (!annotated) {
        uint32_t tb = cfg_target_block(g, n);
        if (tb != RC_INDEX_NONE) {
            rc_array_u32_push(&t.blocks, tb, arena);
        }
        else if (cfg_target_is_external(g, n)) {
            t.external = true;
        }
        else {
            t.unknown = true;   // a computed call we cannot follow
        }
    }
    return t;
}

// Append `succ_block` to the shared pool as one more successor of `block`. The pool grows independently of the
// blocks array, so a `block` pointer into the blocks array stays valid across this (only the pool relocates).
// A block's successors are pushed contiguously, so succ_first (set before the first push) + succ_count spans
// them.
static void add_succ(cfg *g, basic_block *block, uint32_t succ_block, rc_arena *arena)
{
    rc_array_u32_push(&g->succs, succ_block, arena);
    block->succ_count++;
}

// Does `cflows` carry an annotation of `kind` sited at `pc`? A linear scan - annotations are few.
static bool cflow_at(rc_view_zp_cflow cflows, zp_cflow_kind kind, uint32_t pc)
{
    for (uint32_t i = 0; i < cflows.num; i++) {
        zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
        if (cf.kind == (uint8_t) kind && cf.site == pc) {
            return true;
        }
    }
    return false;
}

cfg cfg_build(rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_label labels,
              rc_view_zp_entry entries, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena != NULL);
    cfg result = {.blocks = rc_array_basic_block_make(16, arena), .succs = rc_array_u32_make(32, arena),
                  .labels = labels};
    if (insns.num == 0) {
        return result;
    }

    // The leader set is one 64 KB span per section, so the SAME address in two sections (paged banks) is two
    // distinct leaders. Size it from the highest section index in the stream (sections are numbered densely
    // from 0 in first-sighting order).
    uint32_t num_sections = 0;
    for (uint32_t i = 0; i < insns.num; i++) {
        uint32_t s = rc_view_zp_insn_get(insns, i).section;
        if (s + 1 > num_sections) {
            num_sections = s + 1;
        }
    }

    // Pass 1: find the block leaders. The first instruction always leads; a control-flow instruction marks its
    // resolved target LOCATION (branch/jump/call - possibly in another section, via a label) and, unless it is
    // a call, the location after it (branch/jump/return end a block, a call falls through in-block). We model
    // BOTH edges of a conditional branch - the sound default for liveness (a spurious edge only lengthens live
    // ranges, never shortens them; a MISSING edge is the only unsound case, which is why computed flow taints).
    rc_bitset leaders = {0};
    rc_bitset_resize(&leaders, num_sections * cfg_addr_space, &scratch);
    mark_leader(&leaders, rc_view_zp_insn_get(insns, 0).section, rc_view_zp_insn_get(insns, 0).pc);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        uint32_t after = insn.pc + insn.size;   // the fall-through is always in THIS instruction's section
        target_loc tl = resolve_target_loc(labels, insn);
        switch (insn.flow) {
            case zp_flow_branch:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }   // taken edge
                mark_leader(&leaders, insn.section, after);                   // fall-through edge
                break;
            case zp_flow_jump:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }
                mark_leader(&leaders, insn.section, after);   // boundary (block after an unconditional jump)
                break;
            case zp_flow_call:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }   // callee entry (via call graph)
                break;
            case zp_flow_return:
                mark_leader(&leaders, insn.section, after);    // boundary (block after a return)
                break;
            case zp_flow_skip:
                // The BIT-skip trick: the swallowed instruction at `after` runs only when branched to
                // directly, so it must start its own block (no edge from the skip reaches it); the
                // fall-through resumes past the swallowed bytes - the resume point needs a block of its
                // own even mid-run (the ZA_ENTRY precedent). A leader bit on a data address (nothing
                // recorded at the resume) is harmless: pass 2 only consults leaders at recorded pcs.
                mark_leader(&leaders, insn.section, after);
                mark_leader(&leaders, insn.section, after + insn.skip_bytes);
                break;
            case zp_flow_normal:
            default:
                break;
        }
        // A declared annotation target is a code entry just as a literal target is: mark it a leader, so an
        // in-program declared target always gets its own block (even mid-run). That is what lets a declared
        // target with NO block reliably mean "off the assembled stream" - an external arm - in the edge
        // wiring and the footprint walk, rather than an address we merely failed to split at. A call takes
        // ZA_CANCALL (its callee arms) and ZA_RETURNTO (its resumption points); a jump, branch or return
        // takes ZA_CANJUMP - the return being the RTS-dispatch trick (jump to a pushed address), the branch
        // a self-modified operand. ZA_RETURN and ZA_UNREACHABLE carry no target; the RC_INDEX_NONE guard
        // skips them. A skip takes no annotation at all (annotation_site never sites one there), so we
        // leave it out rather than let it scan for ZA_CANJUMPs it can never own.
        if (insn.flow != zp_flow_normal && insn.flow != zp_flow_skip) {
            for (uint32_t j = 0; j < cflows.num; j++) {
                zp_cflow cf = rc_view_zp_cflow_get(cflows, j);
                bool fits = (insn.flow == zp_flow_call)
                                ? cf.kind == zp_cflow_za_cancall || cf.kind == zp_cflow_za_returnto
                                : cf.kind == zp_cflow_za_canjump;
                if (fits && cf.site == insn.pc && cf.target != RC_INDEX_NONE) {
                    mark_leader(&leaders, insn.section, cf.target);
                }
            }
        }
    }
    // A ZA_ENTRY / ZA_INTERRUPT marker is a code entry too: mark it a leader so a mid-run entry address starts
    // its own block - and so "no block at (section, pc)" reliably means the marker sits on no instruction
    // (data, or the end of a section), which finalize reports. A marker in a section with no recorded insns
    // has no leader space at all; skip it here and let the same finalize check catch it.
    for (uint32_t i = 0; i < entries.num; i++) {
        zp_entry e = rc_view_zp_entry_get(entries, i);
        if (e.section < num_sections) {
            mark_leader(&leaders, e.section, e.pc);
        }
    }

    // Pass 2: cut the instruction stream into blocks. A new block starts at the first instruction, at every
    // SECTION change (which keeps each block single-section and its pc monotonic even as sections interleave),
    // at any instruction that is a leader in its section, and after any block TERMINATOR - a branch, jump or
    // return, or a call whose continuation an annotation reroutes (ZA_RETURNTO) or severs (ZA_UNREACHABLE
    // sited just past it). The terminator cut matters where inline data displaces the next instruction: the
    // after-address leader from pass 1 then marks a pc no instruction sits on, and without the cut the
    // terminator would sit mid-block, losing its edges. The block runs until the next such start.
    uint32_t current  = RC_INDEX_NONE;
    uint32_t prev_sec = 0;
    uint32_t prev_pc  = 0;
    bool prev_cuts    = false;
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        // Within one section pc must never step backward - the property that keeps (section, pc) an
        // unambiguous block identity. It holds by construction (a section's org is fixed at open and its
        // cursor only advances), so this asserts the invariant rather than handling a violation. Equal pcs
        // DO occur: a size-0 ZA_DISCARD marker shares its address with the instruction after it, which is also
        // why a leader (or a terminator cut) starts a new block only when the address CHANGES - both same-pc
        // records belong to one block, marker first. A real terminator always advances the pc, so its cut is
        // never lost to that guard.
        RC_ASSERT(i == 0 || insn.section != prev_sec || insn.pc >= prev_pc);
        bool new_addr = i == 0 || insn.section != prev_sec || insn.pc != prev_pc;
        if (i == 0 || insn.section != prev_sec
            || (new_addr && (addr_is_leader(&leaders, insn.section, insn.pc) || prev_cuts))) {
            current = rc_array_basic_block_push(
                &result.blocks,
                (basic_block) {
                    .section      = insn.section,
                    .pc           = insn.pc,
                    .first_insn   = i,
                    .num_insns    = 0,
                    .succ_first   = 0,
                    .succ_count   = 0,
                    .unknown_succ = false,
                },
                arena);
        }
        rc_array_basic_block_at(&result.blocks, current)->num_insns++;
        // Does THIS instruction force a cut before the next record? A branch/jump/return always ends its
        // block; so does a skip (its fall-through resumes PAST the record at pc+1, which must never glue
        // into our block); a call does too when an annotation reroutes its continuation or declares there
        // is none. ZA_UNREACHABLE just past a NORMAL instruction cuts as well (pass 3 severs that
        // fall-through the same way); its size > 0 guard keeps a size-0 ZA_DISCARD marker - which shares
        // its neighbour's pc - from matching an annotation meant for the neighbour.
        prev_cuts = insn.flow == zp_flow_branch || insn.flow == zp_flow_jump || insn.flow == zp_flow_return
                 || insn.flow == zp_flow_skip
                 || (insn.flow == zp_flow_call
                     && (cflow_at(cflows, zp_cflow_za_returnto, insn.pc)
                      || cflow_at(cflows, zp_cflow_za_unreachable, insn.pc + insn.size)))
                 || (insn.flow == zp_flow_normal && insn.size > 0
                     && cflow_at(cflows, zp_cflow_za_unreachable, insn.pc + insn.size));
        prev_sec = insn.section;
        prev_pc  = insn.pc;
    }

    // Pass 3: wire successor edges from each block's LAST instruction's control-flow class into the shared
    // pool. A branch has two (fall-through + target), a jump one (target), a return none, and a call / normal
    // terminator falls through to the next block. Fall-through stays in the block's own section; the target is
    // resolved by resolve_target_loc (a named label may cross sections). Annotations adjust this: declared
    // ZA_CANJUMP targets REPLACE a jump's or branch's taken edge (a self-modified operand's placeholder must
    // not wire a wrong edge, so the programmer's word beats the literal - as it already does for calls);
    // ZA_RETURN says the transfer hands control back to whoever called this routine (no edge - liveness's
    // return machinery supplies the semantics); ZA_UNREACHABLE prunes a fall-through edge; ZA_RETURNTO
    // reroutes a call's continuation to its declared resumption points. A target that names no block and is
    // not annotated yields the unknown_succ taint - UNLESS it is external (a constant destination off the
    // stream, or a constant OS vector), which is a clean exit out of the program (see target_is_external).
    for (uint32_t bi = 0; bi < result.blocks.num; bi++) {
        basic_block *block = rc_array_basic_block_at(&result.blocks, bi);
        zp_insn last = rc_view_zp_insn_get(insns, block->first_insn + block->num_insns - 1);
        uint32_t after = last.pc + last.size;
        target_loc tl  = resolve_target_loc(labels, last);
        block->succ_first = result.succs.num;
        switch (last.flow) {
            case zp_flow_branch:
            case zp_flow_jump: {
                if (last.flow == zp_flow_branch) {
                    // Not-taken (in-stream) fall-through, unless ZA_UNREACHABLE asserts control cannot reach
                    // it. Pure pc arithmetic, deliberately without the call/normal arm's data-gap skip: a
                    // not-taken branch landing in inline data is a broken program, not a continuation.
                    uint32_t ft = block_at(result.blocks.view, last.section, after);
                    if (ft != RC_INDEX_NONE && !cflow_at(cflows, zp_cflow_za_unreachable, after)) {
                        add_succ(&result, block, ft, arena);
                    }
                }
                // The taken edge. Declared ZA_CANJUMP targets replace the literal outright (resolved in the
                // transfer's own section) - the pass-1 leader marking guarantees every in-program declared
                // target has its own block, so "no block" reliably means off the assembled stream: an
                // EXTERNAL arm, contributing no edge and no taint (same policy as an unannotated JSR to a
                // constant). ZA_RETURN also contributes no edge: control hands back to whoever called us,
                // which block_returns turns into the routine's exit. Only an unannotated transfer falls
                // back to the literal, and taints when the destination is computed and possibly ours.
                bool returns   = cflow_at(cflows, zp_cflow_za_return, last.pc);
                bool annotated = false;
                for (uint32_t i = 0; i < cflows.num; i++) {
                    zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
                    if (cf.kind == zp_cflow_za_canjump && cf.site == last.pc) {
                        annotated = true;
                        uint32_t tb = block_at(result.blocks.view, last.section, cf.target);
                        if (tb != RC_INDEX_NONE) {
                            add_succ(&result, block, tb, arena);
                        }
                    }
                }
                if (!annotated && !returns) {
                    uint32_t taken = tl.found ? block_at(result.blocks.view, tl.section, tl.pc) : RC_INDEX_NONE;
                    if (taken != RC_INDEX_NONE) {
                        add_succ(&result, block, taken, arena);
                    }
                    else if (!target_is_external(labels, last)) {
                        block->unknown_succ = true;   // a destination we cannot place -> conservative
                    }
                    // else: a transfer out of the program (a constant destination off the stream, or a
                    // constant OS vector) - a clean exit, exactly like a return: control leaves for code
                    // that touches none of our variables. No edge, no taint.
                }
                break;
            }
            case zp_flow_return:
                // A genuine return: no successor, fully known. A ZA_CANJUMP at the RTS is the dispatch trick -
                // a jump in a return's clothing - and gets the declared edges exactly as a computed JMP
                // does (an external arm contributes no edge: control leaves for the OS and returns to our
                // caller through its RTS). An UNANNOTATED dispatch is indistinguishable from a real return,
                // so it stays a trusted precondition, never a taint.
                for (uint32_t i = 0; i < cflows.num; i++) {
                    zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
                    if (cf.kind == zp_cflow_za_canjump && cf.site == last.pc) {
                        uint32_t tb = block_at(result.blocks.view, last.section, cf.target);
                        if (tb != RC_INDEX_NONE) {
                            add_succ(&result, block, tb, arena);
                        }
                    }
                }
                break;
            case zp_flow_skip: {
                // The BIT-skip trick: exactly one successor, the resume address past the swallowed bytes.
                // NEVER an edge to pc+1 - the swallowed instruction runs only when branched to directly,
                // which is the entire point of modelling this (a swallowed store must not look like a
                // definite write on the fall-through path). ZA_UNREACHABLE sited at the resume severs,
                // as it does for a call/normal fall-through. If nothing was recorded at the resume (the
                // skip hops inline data), control carries on at the next recorded same-section
                // instruction - but the plain gap-skip loop would land on the swallowed block itself
                // (it sits at pc+1, BEFORE the resume), so we insist on pc >= resume. Nothing found is
                // the end of the program - a clean end, not an unknown.
                uint32_t resume = after + last.skip_bytes;
                if (!cflow_at(cflows, zp_cflow_za_unreachable, resume)) {
                    uint32_t ft = block_at(result.blocks.view, last.section, resume);
                    if (ft != RC_INDEX_NONE) {
                        add_succ(&result, block, ft, arena);
                    }
                    else {
                        for (uint32_t j = bi + 1; j < result.blocks.num; j++) {
                            basic_block cand = rc_array_basic_block_get(&result.blocks, j);
                            if (cand.section == last.section && cand.pc >= resume) {
                                add_succ(&result, block, j, arena);
                                break;
                            }
                        }
                    }
                }
                break;
            }
            case zp_flow_call:
            case zp_flow_normal:
            default: {
                // A call/normal terminator falls through in-stream (same section) - unless ZA_RETURNTO
                // reroutes the call's continuation to its declared resumption points (the caller side of the
                // inline-data idiom: the callee pops its return address and resumes the caller where the
                // annotation says), or ZA_UNREACHABLE - sited just past a never-returning call - severs it.
                // A rerouted arm with no block is off the assembled stream: no edge, no taint (finalize
                // warns, since a resumption point we did not assemble is almost always a mistyped label).
                bool redirected = false;
                if (last.flow == zp_flow_call) {
                    for (uint32_t i = 0; i < cflows.num; i++) {
                        zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
                        if (cf.kind == zp_cflow_za_returnto && cf.site == last.pc) {
                            redirected = true;
                            uint32_t tb = block_at(result.blocks.view, last.section, cf.target);
                            if (tb != RC_INDEX_NONE) {
                                add_succ(&result, block, tb, arena);
                            }
                        }
                    }
                }
                if (!redirected && !cflow_at(cflows, zp_cflow_za_unreachable, after)) {
                    uint32_t ft = block_at(result.blocks.view, last.section, after);
                    if (ft != RC_INDEX_NONE) {
                        add_succ(&result, block, ft, arena);
                    }
                    else {
                        // The fall-through pc has no block: inline data displaced the next instruction
                        // (JSR printstring : EQUS "text", 0 : ... - the callee consumes the data and
                        // resumes past it). Control carries on at the next recorded same-section
                        // instruction - the adjacency the in-block walkers already assume, expressed as an
                        // edge. Blocks tile the instruction list in order, so the first later block in our
                        // section starts at exactly that instruction; nothing found is the end of the
                        // program (or a routine falling off its end) - a clean end, not an unknown.
                        for (uint32_t j = bi + 1; j < result.blocks.num; j++) {
                            if (rc_array_basic_block_get(&result.blocks, j).section == last.section) {
                                add_succ(&result, block, j, arena);
                                break;
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    return result;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A tiny builder for the tests: append one instruction, defaulting the fields a CFG test does not care about
// (vreg/rw/at). Returns the running pc so a test can chain instructions without hand-tracking addresses.
static uint32_t push_insn(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, zp_flow flow,
                          uint32_t target, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = vref_none, .vreg = RC_INDEX_NONE,
                   .target = target, .target_scope = RC_INDEX_NONE, .target_def = cursor_none(),
                   .at = (cursor) {0}},
        arena);
    return pc + size;
}

// A BITZP/BITABS marker: one emitted byte whose operand fetch swallows the next `swallow` bytes at run
// time. Returns the pc just PAST the marker byte - where the swallowed instruction sits, not the resume.
static uint32_t push_skip(rc_array_zp_insn *insns, uint32_t pc, uint8_t swallow, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = 1, .flow = zp_flow_skip, .skip_bytes = swallow, .rw = vref_none,
                   .vreg = RC_INDEX_NONE, .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE,
                   .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + 1;
}

RC_TEST(cfg, empty_stream_is_empty)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    cfg g = cfg_build((rc_view_zp_insn) {0}, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 0u);
    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, straight_line_is_one_block)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    // LDA / STA / RTS - no interior control flow, so one block ending in a return (no successor).
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA zp
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // STA zp
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.first_insn, ==, 0u);
    RC_CHECK(b.num_insns, ==, 3u);
    RC_CHECK(b.succ_count, ==, 0u);            // RTS has no successor...
    RC_CHECK_FALSE(b.unknown_succ);            // ...and that is fully KNOWN (a return, not a computed exit)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, indirect_jump_is_unknown_successor)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #..  (normal)
    //   2002  JMP (ind) (jump with a computed target the assembler cannot see -> flow=jump, target=NONE)
    // The block must NOT read as a clean dead-end (that would let liveness conclude nothing is live out and
    // shrink a range the real target still needs); it is flagged unknown_succ so the analysis stays
    // conservative. This is the "be conservative rather than silently emit broken code" guarantee.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 3, zp_flow_jump, RC_INDEX_NONE, &arena);     // JMP (ind) - unknown target
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ_count, ==, 0u);           // no successor we can place...
    RC_CHECK_TRUE(b.unknown_succ);            // ...but control DOES leave, so stay conservative

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, branch_splits_into_blocks_with_edges)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDX #.. (normal, 2 bytes)
    //   2002  BNE 2006 (branch, 2 bytes; target = 2006, fall-through = 2004)
    //   2004  LDA #.. (normal, 2 bytes)   <- fall-through block
    //   2006  RTS      (return, 1 byte)   <- taken-target block
    // Expect three blocks: [2000,2002] ending in the branch, [2004] falling through to the RTS block, [2006].
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDX #
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2006, &arena);         // BNE 2006
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 3u);

    // Block 0: LDX + BNE, leader 2000, two successors - fall-through (block 1) and target (block 2).
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.pc, ==, 0x2000u);
    RC_CHECK(b0.num_insns, ==, 2u);
    RC_CHECK(b0.succ_count, ==, 2u);
    RC_CHECK(cfg_succ(g, b0, 0), ==, 1u);   // not taken -> the LDA block
    RC_CHECK(cfg_succ(g, b0, 1), ==, 2u);   // taken -> the RTS block

    // Block 1: the fall-through LDA, leader 2004, falls through to block 2.
    basic_block b1 = rc_array_basic_block_get(&g.blocks, 1);
    RC_CHECK(b1.pc, ==, 0x2004u);
    RC_CHECK(b1.num_insns, ==, 1u);
    RC_CHECK(b1.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b1, 0), ==, 2u);

    // Block 2: the RTS target, leader 2006, no successor.
    basic_block b2 = rc_array_basic_block_get(&g.blocks, 2);
    RC_CHECK(b2.pc, ==, 0x2006u);
    RC_CHECK(b2.num_insns, ==, 1u);
    RC_CHECK(b2.succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, jump_target_leads_backward_edge)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #..  (normal)          <- loop top, a JMP target
    //   2002  JMP 2000 (jump back)
    // A backward JMP: block 0 is [2000,2002] and jumps to itself (its own leader). One block, self-edge.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 3, zp_flow_jump, 0x2000, &arena);           // JMP 2000
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b, 0), ==, 0u);   // jumps back to itself

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, call_is_in_block_not_an_edge)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  JSR 3000 (call, 3 bytes)   <- callee entry 3000 is a leader, but NOT a successor edge
    //   2003  RTS      (return)
    // The JSR falls through in-block: block 0 = [2000(JSR), 2003(RTS)], no successor (ends in RTS). The callee
    // at 3000 has no instruction recorded here, so it forms no block - it would be reached via the call graph.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 3, zp_flow_call, 0x3000, &arena);   // JSR 3000
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);        // JSR and RTS in one block
    RC_CHECK(b.succ_count, ==, 0u);           // ends in RTS - no intraprocedural successor

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_canjump_wires_declared_targets)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #   (normal)  <- target A block
    //   2002  RTS
    //   2003  LDA #   (normal)  <- target B block
    //   2005  RTS
    //   2006  JMP (ind) (jump, target unknown) <- the dispatcher
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2002
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2003
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2005
    pc = push_insn(&insns, pc, 3, zp_flow_jump,   RC_INDEX_NONE, &arena);   // JMP (ind) @2006
    (void) pc;

    // ZA_CANJUMP @2006 -> {2000, 2003}: two real successor edges, and the taint cleared.
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(2, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2006, .target = 0x2000, .kind = zp_cflow_za_canjump}, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2006, .target = 0x2003, .kind = zp_cflow_za_canjump}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b = rc_array_basic_block_get(&g.blocks, cfg_block_at(g, 0, 0x2006));
    RC_CHECK_FALSE(b.unknown_succ);            // ZA_CANJUMP resolved it - no taint
    RC_CHECK(b.succ_count, ==, 2u);
    RC_CHECK(cfg_succ(g, b, 0), ==, cfg_block_at(g, 0, 0x2000));
    RC_CHECK(cfg_succ(g, b, 1), ==, cfg_block_at(g, 0, 0x2003));

    // Without the annotation the same JMP stays an unknown successor with no placeable edges.
    cfg g2 = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b2 = rc_array_basic_block_get(&g2.blocks, cfg_block_at(g2, 0, 0x2006));
    RC_CHECK_TRUE(b2.unknown_succ);
    RC_CHECK(b2.succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_canjump_external_arm_and_midblock_target)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #    (normal)
    //   2002  LDA #    (normal)  <- a declared ZA_CANJUMP target that sits MID-RUN: the leader marking must
    //   2004  RTS                   split the block here so the edge can be wired
    //   2005  JMP (ind) (dispatcher, target unknown)
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2002
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2004
    pc = push_insn(&insns, pc, 3, zp_flow_jump,   RC_INDEX_NONE, &arena);   // JMP (ind) @2005
    (void) pc;

    // ZA_CANJUMP @2005 -> {2002, FFEE}: one in-program arm (mid-run, forcing a block split) and one EXTERNAL
    // arm (an OS entry - off the stream, so it contributes a clean exit, not an edge and not a taint).
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(2, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2005, .target = 0x2002, .kind = zp_cflow_za_canjump}, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2005, .target = 0xFFEE, .kind = zp_cflow_za_canjump}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 3u);   // [2000], [2002,2004] (split by the declared target), [2005]
    basic_block b = rc_array_basic_block_get(&g.blocks, cfg_block_at(g, 0, 0x2005));
    RC_CHECK_FALSE(b.unknown_succ);   // both arms accounted for - no taint
    RC_CHECK(b.succ_count, ==, 1u);   // only the in-program arm is an edge
    RC_CHECK(cfg_succ(g, b, 0), ==, cfg_block_at(g, 0, 0x2002));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, external_constant_targets_are_clean_exits)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  BNE FFEE (branch out of the program - the taken arm leaves for external code)
    //   2002  JMP FFEE (jump out of the program)
    // A constant destination that matches nothing we assembled is a transfer to EXTERNAL code (an OS entry),
    // which by policy touches no ZA_AUTO variable: the branch keeps only its fall-through edge and the jump is
    // as clean an exit as an RTS - neither taints.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0xFFEE, &arena);   // BNE &FFEE
    pc = push_insn(&insns, pc, 3, zp_flow_jump,   0xFFEE, &arena);   // JMP &FFEE
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 2u);
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.succ_count, ==, 1u);          // fall-through to the JMP block only
    RC_CHECK(cfg_succ(g, b0, 0), ==, 1u);
    RC_CHECK_FALSE(b0.unknown_succ);          // the taken arm exits the program - fully known
    basic_block b1 = rc_array_basic_block_get(&g.blocks, 1);
    RC_CHECK(b1.succ_count, ==, 0u);
    RC_CHECK_FALSE(b1.unknown_succ);          // an exit, not computed flow

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, vector_jump_external_vs_own_label)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    // One instruction: JMP (vector) at 2000. Its operand names the vector CELL, so target stays NONE and the
    // via field says how control leaves. Three flavours of vector:
    cursor vec_def = {.source = 0, .pos = 42};
    zp_insn jmp = {.pc = 0x2000, .size = 3, .flow = zp_flow_jump, .rw = vref_none, .vreg = RC_INDEX_NONE,
                   .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE, .target_def = cursor_none(),
                   .target_via = (uint8_t) zp_target_via_vector, .at = (cursor) {0}};

    // (a) a literal constant cell - JMP (&FFFC): a fixed OS vector, so control leaves for external code.
    rc_array_zp_insn constant = rc_array_zp_insn_make(1, &arena);
    rc_array_zp_insn_push(&constant, jmp, &arena);
    cfg ga = cfg_build(constant.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block ba = rc_array_basic_block_get(&ga.blocks, 0);
    RC_CHECK_FALSE(ba.unknown_succ);   // a clean exit, no annotation needed
    RC_CHECK(ba.succ_count, ==, 0u);

    // (b) a NAMED cell that is one of OUR labels (a vector we assembled): its run-time contents may point
    // back into our own code, so this is genuinely computed - the taint stands until a ZA_CANJUMP says otherwise.
    zp_insn through_ours = jmp;
    through_ours.target_scope = 5;
    through_ours.target_def   = vec_def;
    rc_array_zp_insn owned = rc_array_zp_insn_make(1, &arena);
    rc_array_zp_insn_push(&owned, through_ours, &arena);
    rc_array_zp_label labels = rc_array_zp_label_make(1, &arena);
    rc_array_zp_label_push(&labels,
        (zp_label) {.scope = 5, .def = vec_def, .section = 0, .pc = 0x2100}, &arena);
    cfg gb = cfg_build(owned.view, (rc_view_zp_cflow) {0}, labels.view, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bb = rc_array_basic_block_get(&gb.blocks, 0);
    RC_CHECK_TRUE(bb.unknown_succ);
    RC_CHECK(bb.succ_count, ==, 0u);   // and NO edge to the vector cell's own address (it is data, not a target)

    // (c) the same named cell with NO marker (a `wrchv = &20E` constant, not a code label): a cell outside
    // the program, so external again.
    cfg gc = cfg_build(owned.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bc = rc_array_basic_block_get(&gc.blocks, 0);
    RC_CHECK_FALSE(bc.unknown_succ);
    RC_CHECK(bc.succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, size0_marker_shares_its_address)
{
    // A ZA_DISCARD marker is a size-0 record at the same pc as the instruction after it. The pair must land
    // in ONE block - marker first - even when that address is a leader, so (section, pc) stays a unique
    // block identity and a branch to the address still resolves.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    rc_array_zp_insn_push(&insns,
        (zp_insn) {.pc = pc, .size = 0, .flow = zp_flow_normal, .vreg = 0, .var_kill = true,
                   .target = RC_INDEX_NONE, .target_scope = RC_INDEX_NONE, .target_def = cursor_none(),
                   .at = (cursor) {0}},
        &arena);                                                            // (ZA_DISCARD) @2002
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2002
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2002,        &arena);   // BNE 2002 @2004
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2006
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 3u);                       // [2000] [marker+2002+branch] [2006]
    uint32_t bi = cfg_block_at(g, 0, 0x2002);
    RC_CHECK_TRUE(bi != RC_INDEX_NONE);
    basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
    RC_CHECK(blk.first_insn, ==, 1u);                     // the marker opens the block...
    RC_CHECK(blk.num_insns, ==, 3u);                      // ...and the branch closes it
    RC_CHECK(blk.succ_count, ==, 2u);                     // fall-through first, then the taken edge
    RC_CHECK(cfg_succ(g, blk, 1), ==, bi);                // the loop edge resolves to the marker's block

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, terminator_cut_across_data_gap)
{
    // Inline data after a JMP/RTS displaces the next instruction, so the after-address leader from pass 1
    // marks a pc no instruction sits on. The terminator cut must still end the block at the JMP - else it
    // sits mid-block and its taken edge is silently dropped (a MISSING edge, the unsound direction).
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #    (normal)          <- loop top, the JMP's target
    //   2002  JMP 2000 (jump back)
    //   2005  EQUS ... (5 data bytes - recorded as nothing, just a pc gap)
    //   200A  LDA #    (normal, NOT a leader by any other rule)
    //   200C  RTS
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 3, zp_flow_jump, 0x2000, &arena);           // JMP 2000 @2002
    pc += 5;                                                               // the data gap
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @200A
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @200C
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 2u);   // [2000,2002(JMP)] and [200A,200C] - the gap must not glue them
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.num_insns, ==, 2u);
    RC_CHECK(b0.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b0, 0), ==, 0u);   // the loop edge survives the gap
    RC_CHECK_FALSE(b0.unknown_succ);
    basic_block b1 = rc_array_basic_block_get(&g.blocks, 1);
    RC_CHECK(b1.pc, ==, 0x200Au);
    RC_CHECK(b1.succ_count, ==, 0u);        // ends in RTS; nothing (modelled) reaches it either

    // The RTS flavour: a routine's return followed by a data table and more (separately-entered) code.
    rc_array_zp_insn ret = rc_array_zp_insn_make(4, &arena);
    pc = 0x2000;
    pc = push_insn(&ret, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2000
    pc += 6;                                                             // the data gap
    pc = push_insn(&ret, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2007
    pc = push_insn(&ret, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2009
    (void) pc;
    cfg gr = cfg_build(ret.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(gr.blocks.num, ==, 2u);   // the RTS keeps its return semantics; the tail code is its own block
    RC_CHECK(rc_array_basic_block_get(&gr.blocks, 0).succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, gap_skip_call_fall_through)
{
    // The caller side of the inline-data idiom, at a block seam: the JSR ends its block (the post-data
    // instruction is a leader), so its fall-through is computed by pc arithmetic - which lands mid-data,
    // where no block exists. Control must skip the gap to the next recorded instruction, not dead-end
    // (a dead end empties the live-after set the callee's return edges inject - the unsound direction).
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  JSR 2010 (call)
    //   2003  EQUS ... (5 data bytes)
    //   2008  LDA #    (normal)  <- a branch target, so a LEADER: the JSR terminates its block
    //   200A  BNE 2008 (branch)
    //   200C  RTS
    //   2010  LDA #    (the callee)
    //   2012  RTS
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 3, zp_flow_call, 0x2010, &arena);           // JSR 2010 @2000
    pc += 5;                                                               // the data gap
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2008
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2008, &arena);         // BNE 2008 @200A
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @200C
    pc = 0x2010;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2010
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2012
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    uint32_t caller = cfg_block_at(g, 0, 0x2000);
    uint32_t resume = cfg_block_at(g, 0, 0x2008);
    RC_CHECK_TRUE(caller != RC_INDEX_NONE && resume != RC_INDEX_NONE);
    basic_block b = rc_array_basic_block_get(&g.blocks, caller);
    RC_CHECK(b.num_insns, ==, 1u);          // the JSR alone (the leader at 2008 cut it)
    RC_CHECK(b.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b, 0), ==, resume);   // the gap-skip edge to the post-data instruction
    RC_CHECK_FALSE(b.unknown_succ);

    // Trailing data at the end of the stream: nothing to resume at, so a clean dead end - no edge, no taint.
    rc_array_zp_insn tail = rc_array_zp_insn_make(2, &arena);
    push_insn(&tail, 0x2000, 3, zp_flow_call, 0xFFEE, &arena);   // JSR &FFEE, then only data to the end
    cfg gt = cfg_build(tail.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bt = rc_array_basic_block_get(&gt.blocks, 0);
    RC_CHECK(bt.succ_count, ==, 0u);
    RC_CHECK_FALSE(bt.unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_unreachable_severs_call_fall_through)
{
    // ZA_UNREACHABLE sited just past a JSR declares the call never returns: the fall-through edge must be
    // severed - both mid-block (the annotation forces a cut so the severing is expressible) and across a
    // data gap (where it suppresses the gap-skip edge).
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    //   2000  JSR FFEE (a never-returning external routine)
    //   2003  <- ZA_UNREACHABLE sited here
    //   2003  LDA #    (would otherwise share the JSR's block)
    //   2005  RTS
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 3, zp_flow_call, 0xFFEE, &arena);           // JSR &FFEE @2000
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2003
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2005
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(1, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2003, .target = RC_INDEX_NONE,
                                                .kind = zp_cflow_za_unreachable}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 2u);   // the annotation cuts after the JSR...
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.num_insns, ==, 1u);
    RC_CHECK(b0.succ_count, ==, 0u);   // ...and severs the edge
    RC_CHECK_FALSE(b0.unknown_succ);

    // The data-gap flavour: JSR + declared-dead data tail, then separately-entered code. The gap-skip edge
    // must be suppressed too.
    rc_array_zp_insn gapped = rc_array_zp_insn_make(4, &arena);
    pc = 0x2000;
    pc = push_insn(&gapped, pc, 3, zp_flow_call, 0xFFEE, &arena);           // JSR &FFEE @2000
    pc += 5;                                                               // the (never-consumed) data
    pc = push_insn(&gapped, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2008
    pc = push_insn(&gapped, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @200A
    (void) pc;
    cfg gg = cfg_build(gapped.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bg = rc_array_basic_block_get(&gg.blocks, 0);
    RC_CHECK(bg.num_insns, ==, 1u);
    RC_CHECK(bg.succ_count, ==, 0u);   // no gap-skip past a declared-dead continuation
    RC_CHECK_FALSE(bg.unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_return_on_jump)
{
    // ZA_RETURN declares a jump the routine's exit back to its own caller: no edge, no taint - whether the
    // jump is computed (JMP (ptr), which would otherwise taint) or a self-modified DIRECT jump whose
    // placeholder happens to name real code (which would otherwise wire a WRONG edge).
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    //   2000  LDA #     (normal)
    //   2002  JMP (ind) (computed - target NONE)
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 3, zp_flow_jump, RC_INDEX_NONE, &arena);     // JMP (ind) @2002
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(1, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2002, .target = RC_INDEX_NONE,
                                                .kind = zp_cflow_za_return}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.succ_count, ==, 0u);
    RC_CHECK_FALSE(b.unknown_succ);   // declared a return - the taint the unannotated twin gets is gone

    // The self-modified direct flavour: JMP 2000 would wire a (wrong) loop edge; ZA_RETURN overrides the
    // resolved literal, exactly as a declared ZA_CANJUMP set would.
    rc_array_zp_insn direct = rc_array_zp_insn_make(4, &arena);
    pc = 0x2000;
    pc = push_insn(&direct, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&direct, pc, 3, zp_flow_jump, 0x2000, &arena);           // JMP 2000 @2002 (placeholder)
    (void) pc;
    cfg gd = cfg_build(direct.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bd = rc_array_basic_block_get(&gd.blocks, 0);
    RC_CHECK(bd.succ_count, ==, 0u);   // the literal edge is NOT wired
    RC_CHECK_FALSE(bd.unknown_succ);
    // And without the annotation the same placeholder DOES wire its edge - the contrast that proves the override.
    cfg gu = cfg_build(direct.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(rc_array_basic_block_get(&gu.blocks, 0).succ_count, ==, 1u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_return_composes_with_canjump)
{
    // A dispatch-or-return: the jump may go to a declared in-program target OR hand back to the caller.
    // Both annotations sit on one site; the CANJUMP arm is an edge, the RETURN arm is not, and no taint.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);

    //   2000  LDA #     (a declared target)
    //   2002  RTS
    //   2003  JMP (ind) (the dispatcher)
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2002
    pc = push_insn(&insns, pc, 3, zp_flow_jump, RC_INDEX_NONE, &arena);     // JMP (ind) @2003
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(2, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2003, .target = 0x2000, .kind = zp_cflow_za_canjump}, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2003, .target = RC_INDEX_NONE, .kind = zp_cflow_za_return}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b = rc_array_basic_block_get(&g.blocks, cfg_block_at(g, 0, 0x2003));
    RC_CHECK(b.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b, 0), ==, cfg_block_at(g, 0, 0x2000));
    RC_CHECK_FALSE(b.unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_returnto_redirects_call)
{
    // The caller-side override: ZA_RETURNTO names where a data-consuming callee resumes this call, replacing
    // the fall-through entirely. The declared target is leader-marked (a mid-run resumption point splits its
    // block), the JSR terminates its own block, and an unresolvable arm contributes no edge and no taint.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  JSR 2010 (call)
    //   2003  EQUS ... (5 data bytes)
    //   2008  LDA #    (post-data code the redirect deliberately SKIPS)
    //   200A  LDA #    <- the declared resumption point, mid-run (only the annotation makes it a leader)
    //   200C  RTS
    //   2010  LDA #    (the callee)
    //   2012  RTS
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 3, zp_flow_call, 0x2010, &arena);           // JSR 2010 @2000
    pc += 5;                                                               // the data gap
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2008
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @200A
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @200C
    pc = 0x2010;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2010
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2012
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(2, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2000, .target = 0x200A, .kind = zp_cflow_za_returnto}, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2000, .target = 0x5000, .kind = zp_cflow_za_returnto}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    uint32_t resume = cfg_block_at(g, 0, 0x200A);
    RC_CHECK_TRUE(resume != RC_INDEX_NONE);   // the declared target split its block
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 1u);            // the annotated JSR terminates its block
    RC_CHECK(b.succ_count, ==, 1u);           // the 5000 arm is off the stream - nothing wired for it
    RC_CHECK(cfg_succ(g, b, 0), ==, resume);  // NOT the gap-skip edge to 2008
    RC_CHECK_FALSE(b.unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, za_canjump_on_branch)
{
    // A self-modified branch: its literal operand is a placeholder, so declared ZA_CANJUMP targets must
    // REPLACE the taken edge (not add to it) while the not-taken fall-through survives; and an unannotated
    // branch whose target cannot be placed still taints.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  BNE 2004 (branch - 2004 is the placeholder, 2006 the declared truth)
    //   2002  LDA #    (fall-through)
    //   2004  LDA #    (the placeholder's block - must get NO edge)
    //   2006  RTS      (the declared target)
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2004, &arena);         // BNE 2004 @2000
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2002
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2004
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2006
    (void) pc;
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(1, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2000, .target = 0x2006, .kind = zp_cflow_za_canjump}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.succ_count, ==, 2u);
    RC_CHECK(cfg_succ(g, b, 0), ==, cfg_block_at(g, 0, 0x2002));   // fall-through first, as ever
    RC_CHECK(cfg_succ(g, b, 1), ==, cfg_block_at(g, 0, 0x2006));   // the declared arm, not the placeholder
    RC_CHECK_FALSE(b.unknown_succ);

    // An unannotated branch with an unplaceable, non-external target still taints (the control case).
    rc_array_zp_insn bare = rc_array_zp_insn_make(2, &arena);
    push_insn(&bare, 0x2000, 2, zp_flow_branch, RC_INDEX_NONE, &arena);
    push_insn(&bare, 0x2002, 1, zp_flow_return, RC_INDEX_NONE, &arena);
    cfg gb = cfg_build(bare.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK_TRUE(rc_array_basic_block_get(&gb.blocks, 0).unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, entry_marker_splits_midrun_block)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);

    //   2000  LDA #    (normal)
    //   2002  LDA #    (normal)  <- a ZA_ENTRY marker mid-run: the leader marking must split the block here,
    //   2004  RTS                   so the entry address names a real block for the reachability roots
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2002
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2004
    (void) pc;

    rc_array_zp_entry entries = rc_array_zp_entry_make(2, &arena);
    rc_array_zp_entry_push(&entries, (zp_entry) {.section = 0, .pc = 0x2002}, &arena);
    // A marker in a section beyond the insn stream (a data-only section) has no leader space: it must be
    // skipped without trapping - finalize reports it as marking no instruction.
    rc_array_zp_entry_push(&entries, (zp_entry) {.section = 9, .pc = 0x3000, .interrupt = true}, &arena);

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, entries.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 2u);
    uint32_t bi = cfg_block_at(g, 0, 0x2002);
    RC_CHECK_TRUE(bi != RC_INDEX_NONE);
    RC_CHECK(rc_array_basic_block_get(&g.blocks, bi).first_insn, ==, 1u);
    RC_CHECK_TRUE(cfg_block_at(g, 9, 0x3000) == RC_INDEX_NONE);   // the stray marker resolved to nothing

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, skip_swallowed_insn_gets_own_block)
{
    // The BIT-skip trick in its classic shape: the swallowed instruction runs only via the branch, and the
    // marker's fall-through hops straight over it to the resume. Getting this wrong is the whole reason the
    // marker exists - before it, the gap-skip edge routed the fall-through THROUGH the swallowed store.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  BEQ 2005 (branch)
    //   2002  ADC zp1  (the fall-through flavour)
    //   2004  BITABS   (swallows the next 2 bytes)
    //   2005  ADC zp2  (the branch-taken flavour - swallowed on fall-through)
    //   2007  RTS      (the resume point: both paths converge here)
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2005, &arena);          // BEQ 2005 @2000
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // ADC zp1  @2002
    pc = push_skip(&insns, pc, 2, &arena);                                  // BITABS   @2004
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // ADC zp2  @2005
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS      @2007
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 4u);
    uint32_t fall      = cfg_block_at(g, 0, 0x2002);
    uint32_t swallowed = cfg_block_at(g, 0, 0x2005);
    uint32_t resume    = cfg_block_at(g, 0, 0x2007);
    RC_CHECK_TRUE(fall != RC_INDEX_NONE && swallowed != RC_INDEX_NONE && resume != RC_INDEX_NONE);

    basic_block bf = rc_array_basic_block_get(&g.blocks, fall);
    RC_CHECK(bf.num_insns, ==, 2u);            // ADC zp1 + the marker (the skip terminates the block)
    RC_CHECK(bf.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, bf, 0), ==, resume);   // over the swallowed instruction, never into it
    RC_CHECK_FALSE(bf.unknown_succ);

    basic_block bs = rc_array_basic_block_get(&g.blocks, swallowed);
    RC_CHECK(bs.num_insns, ==, 1u);
    RC_CHECK(bs.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, bs, 0), ==, resume);   // the branch-taken path falls through to the same resume

    basic_block bb = rc_array_basic_block_get(&g.blocks, 0);   // the BEQ
    RC_CHECK(bb.succ_count, ==, 2u);            // not-taken (2002) + taken (2005), untouched by the marker

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, skip_resumes_past_data_gap)
{
    // A skip hopping inline DATA: nothing is recorded at the resume address, so the edge must gap-skip to
    // the next recorded instruction - but constrained to pc >= resume, or it would land on the swallowed
    // block itself, which sits at pc+1, BEFORE the resume. The second stream proves that guard.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    //   2000  BITABS   (swallows 2 data bytes)
    //   2001  EQUB x,y (unrecorded)
    //   2003  EQUS ... (5 more unrecorded bytes - resume lands in data too)
    //   2008  LDA #    (the next recorded instruction)
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    push_skip(&insns, 0x2000, 2, &arena);                                 // BITABS @2000
    push_insn(&insns, 0x2008, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA    @2008

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    uint32_t after = cfg_block_at(g, 0, 0x2008);
    RC_CHECK_TRUE(after != RC_INDEX_NONE);
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b0, 0), ==, after);   // the constrained gap-skip
    RC_CHECK_FALSE(b0.unknown_succ);

    //   2000  BITABS   (swallows a RECORDED 2-byte instruction this time)
    //   2001  BEQ 2008 (swallowed - a later block than the marker's, at a pc BEFORE the resume)
    //   2003  EQUS ... (data at the resume address)
    //   2008  LDA #
    rc_array_zp_insn guard = rc_array_zp_insn_make(4, &arena);
    push_skip(&guard, 0x2000, 2, &arena);                                  // BITABS   @2000
    push_insn(&guard, 0x2001, 2, zp_flow_branch, 0x2008, &arena);          // BEQ 2008 @2001
    push_insn(&guard, 0x2008, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA      @2008

    cfg gg = cfg_build(guard.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    uint32_t swallowed = cfg_block_at(gg, 0, 0x2001);
    uint32_t resume    = cfg_block_at(gg, 0, 0x2008);
    RC_CHECK_TRUE(swallowed != RC_INDEX_NONE && resume != RC_INDEX_NONE);
    basic_block bm = rc_array_basic_block_get(&gg.blocks, 0);
    RC_CHECK(bm.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(gg, bm, 0), ==, resume);   // pc >= resume held: NOT the swallowed block at 2001

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, skb_resumes_at_pc_plus_two)
{
    // The 1-byte flavour (&24, BIT zp): resume is pc+2, hopping a single swallowed byte. And a skip that
    // runs off the end of the stream is a clean dead end - no edge, no taint - like any other fall-through.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    //   2000  BITZP  (swallows 1 byte)
    //   2001  INX    (swallowed)
    //   2002  RTS    (the resume)
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    uint32_t pc = 0x2000;
    pc = push_skip(&insns, pc, 1, &arena);                                  // BITZP @2000
    pc = push_insn(&insns, pc, 1, zp_flow_normal, RC_INDEX_NONE, &arena);   // INX   @2001
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS   @2002
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    uint32_t resume = cfg_block_at(g, 0, 0x2002);
    RC_CHECK_TRUE(resume != RC_INDEX_NONE);
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.num_insns, ==, 1u);
    RC_CHECK(b0.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b0, 0), ==, resume);

    rc_array_zp_insn tail = rc_array_zp_insn_make(2, &arena);
    push_skip(&tail, 0x2000, 2, &arena);   // BITABS with nothing after it at all
    cfg gt = cfg_build(tail.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block bt = rc_array_basic_block_get(&gt.blocks, 0);
    RC_CHECK(bt.succ_count, ==, 0u);
    RC_CHECK_FALSE(bt.unknown_succ);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, skip_respects_za_unreachable_at_resume)
{
    // ZA_UNREACHABLE sited at the resume address severs the skip's one edge, exactly as it severs a
    // call/normal fall-through - the composition costs nothing and someone will eventually want it.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena

    //   2000  BITABS
    //   2001  ADC zp (swallowed)
    //   2003  RTS    <- ZA_UNREACHABLE sited here (the resume address)
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    push_skip(&insns, 0x2000, 2, &arena);                                  // BITABS @2000
    push_insn(&insns, 0x2001, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // ADC zp @2001
    push_insn(&insns, 0x2003, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS    @2003
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(1, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2003, .target = RC_INDEX_NONE,
                                                .kind = zp_cflow_za_unreachable}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.num_insns, ==, 1u);
    RC_CHECK(b0.succ_count, ==, 0u);   // the resume edge is severed...
    RC_CHECK_FALSE(b0.unknown_succ);   // ...knowingly

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
