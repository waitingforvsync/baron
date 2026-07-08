#include "opcodes.h"

#include "assemble_internal.h"   // parse_result, int_argument, require_separator, syntax_error, semantic_error
#include "baron.h"               // scopes / overlays / source_files reached through b
#include "lexer.h"
#include "expression.h"
#include "richc/macros.h"


// (mnemonic, addr_mode) -> cell. Bytes are the predecessor's verbatim. A cell ORs in `cmos`
// only when the encoding is CMOS-only (a plain cell means both CPUs), and, for a future
// zero-page allocator, the call-analysis class: op_read / op_write (a memory operand; both =
// rmw) on the memory addressing modes only, and op_branch / op_jump / op_call / op_return on
// the relevant control-flow cells. Immediate, accumulator and implied carry no access flag.
static const uint16_t opcode_defs[mnemonic_max][addr_mode_max] = {
    [mnemonic_adc] = {
        [addr_mode_imm]  = 0x69,
        [addr_mode_zp]   = 0x65 | op_read,
        [addr_mode_zpx]  = 0x75 | op_read,
        [addr_mode_abs]  = 0x6D | op_read,
        [addr_mode_absx] = 0x7D | op_read,
        [addr_mode_absy] = 0x79 | op_read,
        [addr_mode_indx] = 0x61 | op_read,
        [addr_mode_indy] = 0x71 | op_read,
        [addr_mode_ind]  = 0x72 | cmos | op_read,
    },
    [mnemonic_and] = {
        [addr_mode_imm]  = 0x29,
        [addr_mode_zp]   = 0x25 | op_read,
        [addr_mode_zpx]  = 0x35 | op_read,
        [addr_mode_abs]  = 0x2D | op_read,
        [addr_mode_absx] = 0x3D | op_read,
        [addr_mode_absy] = 0x39 | op_read,
        [addr_mode_indx] = 0x21 | op_read,
        [addr_mode_indy] = 0x31 | op_read,
        [addr_mode_ind]  = 0x32 | cmos | op_read,
    },
    [mnemonic_asl] = {
        [addr_mode_acc]  = 0x0A,
        [addr_mode_zp]   = 0x06 | op_read | op_write,
        [addr_mode_zpx]  = 0x16 | op_read | op_write,
        [addr_mode_abs]  = 0x0E | op_read | op_write,
        [addr_mode_absx] = 0x1E | op_read | op_write,
    },
    [mnemonic_bcc] = {
        [addr_mode_rel] = 0x90 | op_branch,
    },
    [mnemonic_bcs] = {
        [addr_mode_rel] = 0xB0 | op_branch,
    },
    [mnemonic_beq] = {
        [addr_mode_rel] = 0xF0 | op_branch,
    },
    [mnemonic_bit] = {
        [addr_mode_imm]  = 0x89 | cmos,
        [addr_mode_zp]   = 0x24 | op_read,
        [addr_mode_zpx]  = 0x34 | op_read,
        [addr_mode_abs]  = 0x2C | op_read,
        [addr_mode_absx] = 0x3C | op_read,
    },
    [mnemonic_bmi] = {
        [addr_mode_rel] = 0x30 | op_branch,
    },
    [mnemonic_bne] = {
        [addr_mode_rel] = 0xD0 | op_branch,
    },
    [mnemonic_bpl] = {
        [addr_mode_rel] = 0x10 | op_branch,
    },
    [mnemonic_brk] = {
        [addr_mode_imp] = 0x00 | op_return,
    },
    [mnemonic_bvc] = {
        [addr_mode_rel] = 0x50 | op_branch,
    },
    [mnemonic_bvs] = {
        [addr_mode_rel] = 0x70 | op_branch,
    },
    [mnemonic_clc] = {
        [addr_mode_imp] = 0x18,
    },
    [mnemonic_cld] = {
        [addr_mode_imp] = 0xD8,
    },
    [mnemonic_cli] = {
        [addr_mode_imp] = 0x58,
    },
    [mnemonic_clv] = {
        [addr_mode_imp] = 0xB8,
    },
    [mnemonic_cmp] = {
        [addr_mode_imm]  = 0xC9,
        [addr_mode_zp]   = 0xC5 | op_read,
        [addr_mode_zpx]  = 0xD5 | op_read,
        [addr_mode_abs]  = 0xCD | op_read,
        [addr_mode_absx] = 0xDD | op_read,
        [addr_mode_absy] = 0xD9 | op_read,
        [addr_mode_indx] = 0xC1 | op_read,
        [addr_mode_indy] = 0xD1 | op_read,
        [addr_mode_ind]  = 0xD2 | cmos | op_read,
    },
    [mnemonic_cpx] = {
        [addr_mode_imm] = 0xE0,
        [addr_mode_zp]  = 0xE4 | op_read,
        [addr_mode_abs] = 0xEC | op_read,
    },
    [mnemonic_cpy] = {
        [addr_mode_imm] = 0xC0,
        [addr_mode_zp]  = 0xC4 | op_read,
        [addr_mode_abs] = 0xCC | op_read,
    },
    [mnemonic_dec] = {
        [addr_mode_acc]  = 0x3A | cmos,
        [addr_mode_zp]   = 0xC6 | op_read | op_write,
        [addr_mode_zpx]  = 0xD6 | op_read | op_write,
        [addr_mode_abs]  = 0xCE | op_read | op_write,
        [addr_mode_absx] = 0xDE | op_read | op_write,
    },
    [mnemonic_dex] = {
        [addr_mode_imp] = 0xCA,
    },
    [mnemonic_dey] = {
        [addr_mode_imp] = 0x88,
    },
    [mnemonic_eor] = {
        [addr_mode_imm]  = 0x49,
        [addr_mode_zp]   = 0x45 | op_read,
        [addr_mode_zpx]  = 0x55 | op_read,
        [addr_mode_abs]  = 0x4D | op_read,
        [addr_mode_absx] = 0x5D | op_read,
        [addr_mode_absy] = 0x59 | op_read,
        [addr_mode_indx] = 0x41 | op_read,
        [addr_mode_indy] = 0x51 | op_read,
        [addr_mode_ind]  = 0x52 | cmos | op_read,
    },
    [mnemonic_inc] = {
        [addr_mode_acc]  = 0x1A | cmos,
        [addr_mode_zp]   = 0xE6 | op_read | op_write,
        [addr_mode_zpx]  = 0xF6 | op_read | op_write,
        [addr_mode_abs]  = 0xEE | op_read | op_write,
        [addr_mode_absx] = 0xFE | op_read | op_write,
    },
    [mnemonic_inx] = {
        [addr_mode_imp] = 0xE8,
    },
    [mnemonic_iny] = {
        [addr_mode_imp] = 0xC8,
    },
    [mnemonic_jmp] = {
        [addr_mode_abs]    = 0x4C | op_jump,
        [addr_mode_ind16]  = 0x6C | op_jump,
        [addr_mode_ind16x] = 0x7C | cmos | op_jump,
    },
    [mnemonic_jsr] = {
        [addr_mode_abs] = 0x20 | op_call,
    },
    [mnemonic_lda] = {
        [addr_mode_imm]  = 0xA9,
        [addr_mode_zp]   = 0xA5 | op_read,
        [addr_mode_zpx]  = 0xB5 | op_read,
        [addr_mode_abs]  = 0xAD | op_read,
        [addr_mode_absx] = 0xBD | op_read,
        [addr_mode_absy] = 0xB9 | op_read,
        [addr_mode_indx] = 0xA1 | op_read,
        [addr_mode_indy] = 0xB1 | op_read,
        [addr_mode_ind]  = 0xB2 | cmos | op_read,
    },
    [mnemonic_ldx] = {
        [addr_mode_imm]  = 0xA2,
        [addr_mode_zp]   = 0xA6 | op_read,
        [addr_mode_zpy]  = 0xB6 | op_read,
        [addr_mode_abs]  = 0xAE | op_read,
        [addr_mode_absy] = 0xBE | op_read,
    },
    [mnemonic_ldy] = {
        [addr_mode_imm]  = 0xA0,
        [addr_mode_zp]   = 0xA4 | op_read,
        [addr_mode_zpx]  = 0xB4 | op_read,
        [addr_mode_abs]  = 0xAC | op_read,
        [addr_mode_absx] = 0xBC | op_read,
    },
    [mnemonic_lsr] = {
        [addr_mode_acc]  = 0x4A,
        [addr_mode_zp]   = 0x46 | op_read | op_write,
        [addr_mode_zpx]  = 0x56 | op_read | op_write,
        [addr_mode_abs]  = 0x4E | op_read | op_write,
        [addr_mode_absx] = 0x5E | op_read | op_write,
    },
    [mnemonic_nop] = {
        [addr_mode_imp] = 0xEA,
    },
    [mnemonic_ora] = {
        [addr_mode_imm]  = 0x09,
        [addr_mode_zp]   = 0x05 | op_read,
        [addr_mode_zpx]  = 0x15 | op_read,
        [addr_mode_abs]  = 0x0D | op_read,
        [addr_mode_absx] = 0x1D | op_read,
        [addr_mode_absy] = 0x19 | op_read,
        [addr_mode_indx] = 0x01 | op_read,
        [addr_mode_indy] = 0x11 | op_read,
        [addr_mode_ind]  = 0x12 | cmos | op_read,
    },
    [mnemonic_pha] = {
        [addr_mode_imp] = 0x48,
    },
    [mnemonic_php] = {
        [addr_mode_imp] = 0x08,
    },
    [mnemonic_pla] = {
        [addr_mode_imp] = 0x68,
    },
    [mnemonic_plp] = {
        [addr_mode_imp] = 0x28,
    },
    [mnemonic_rol] = {
        [addr_mode_acc]  = 0x2A,
        [addr_mode_zp]   = 0x26 | op_read | op_write,
        [addr_mode_zpx]  = 0x36 | op_read | op_write,
        [addr_mode_abs]  = 0x2E | op_read | op_write,
        [addr_mode_absx] = 0x3E | op_read | op_write,
    },
    [mnemonic_ror] = {
        [addr_mode_acc]  = 0x6A,
        [addr_mode_zp]   = 0x66 | op_read | op_write,
        [addr_mode_zpx]  = 0x76 | op_read | op_write,
        [addr_mode_abs]  = 0x6E | op_read | op_write,
        [addr_mode_absx] = 0x7E | op_read | op_write,
    },
    [mnemonic_rti] = {
        [addr_mode_imp] = 0x40 | op_return,
    },
    [mnemonic_rts] = {
        [addr_mode_imp] = 0x60 | op_return,
    },
    [mnemonic_sbc] = {
        [addr_mode_imm]  = 0xE9,
        [addr_mode_zp]   = 0xE5 | op_read,
        [addr_mode_zpx]  = 0xF5 | op_read,
        [addr_mode_abs]  = 0xED | op_read,
        [addr_mode_absx] = 0xFD | op_read,
        [addr_mode_absy] = 0xF9 | op_read,
        [addr_mode_indx] = 0xE1 | op_read,
        [addr_mode_indy] = 0xF1 | op_read,
        [addr_mode_ind]  = 0xF2 | cmos | op_read,
    },
    [mnemonic_sec] = {
        [addr_mode_imp] = 0x38,
    },
    [mnemonic_sed] = {
        [addr_mode_imp] = 0xF8,
    },
    [mnemonic_sei] = {
        [addr_mode_imp] = 0x78,
    },
    [mnemonic_sta] = {
        [addr_mode_zp]   = 0x85 | op_write,
        [addr_mode_zpx]  = 0x95 | op_write,
        [addr_mode_abs]  = 0x8D | op_write,
        [addr_mode_absx] = 0x9D | op_write,
        [addr_mode_absy] = 0x99 | op_write,
        [addr_mode_indx] = 0x81 | op_write,
        [addr_mode_indy] = 0x91 | op_write,
        [addr_mode_ind]  = 0x92 | cmos | op_write,
    },
    [mnemonic_stx] = {
        [addr_mode_zp]  = 0x86 | op_write,
        [addr_mode_zpy] = 0x96 | op_write,
        [addr_mode_abs] = 0x8E | op_write,
    },
    [mnemonic_sty] = {
        [addr_mode_zp]  = 0x84 | op_write,
        [addr_mode_zpx] = 0x94 | op_write,
        [addr_mode_abs] = 0x8C | op_write,
    },
    [mnemonic_tax] = {
        [addr_mode_imp] = 0xAA,
    },
    [mnemonic_tay] = {
        [addr_mode_imp] = 0xA8,
    },
    [mnemonic_tsx] = {
        [addr_mode_imp] = 0xBA,
    },
    [mnemonic_txa] = {
        [addr_mode_imp] = 0x8A,
    },
    [mnemonic_txs] = {
        [addr_mode_imp] = 0x9A,
    },
    [mnemonic_tya] = {
        [addr_mode_imp] = 0x98,
    },
    [mnemonic_bra] = {
        [addr_mode_rel] = 0x80 | cmos | op_branch,
    },
    [mnemonic_dea] = {
        [addr_mode_imp] = 0x3A | cmos,
    },
    [mnemonic_ina] = {
        [addr_mode_imp] = 0x1A | cmos,
    },
    [mnemonic_phx] = {
        [addr_mode_imp] = 0xDA | cmos,
    },
    [mnemonic_phy] = {
        [addr_mode_imp] = 0x5A | cmos,
    },
    [mnemonic_plx] = {
        [addr_mode_imp] = 0xFA | cmos,
    },
    [mnemonic_ply] = {
        [addr_mode_imp] = 0x7A | cmos,
    },
    [mnemonic_stz] = {
        [addr_mode_zp]   = 0x64 | cmos | op_write,
        [addr_mode_zpx]  = 0x74 | cmos | op_write,
        [addr_mode_abs]  = 0x9C | cmos | op_write,
        [addr_mode_absx] = 0x9E | cmos | op_write,
    },
    [mnemonic_trb] = {
        [addr_mode_zp]  = 0x14 | cmos | op_read | op_write,
        [addr_mode_abs] = 0x1C | cmos | op_read | op_write,
    },
    [mnemonic_tsb] = {
        [addr_mode_zp]  = 0x04 | cmos | op_read | op_write,
        [addr_mode_abs] = 0x0C | cmos | op_read | op_write,
    },
};


// Callers test presence of an encoding as `cell != 0`. That is safe only because no present
// cell is wholly zero: the one opcode byte that is 0x00 (BRK) carries op_return, which keeps its
// cell nonzero. So presence detection leans on every 0x00-byte instruction also carrying a flag
// - true today by coincidence of the table, not by construction.
uint16_t opcode_def(mnemonic m, addr_mode mode)
{
    RC_ASSERT(m < mnemonic_max && mode < addr_mode_max);
    return opcode_defs[m][mode];
}


// ---- operand + addressing-mode parsing ----

// The DEFAULT opcode-operand token table: the structural punctuation only - '#' (immediate) and the parens
// (indirect). No registers: in the BASE of an operand a bare A / X / Y is an ordinary symbol, never a
// register, so `STA temp` still resolves to `temp` even when temp is literally named `x`. Registers are
// recognised only where the grammar expects one (see operand_reg_tokens); everywhere else an identifier
// wins, and a longer identifier always beats a keyword anyway (a symbol "xyz" is not the X register).
static const token operand_token_entries[] = {
    {RC_STR("#"), {.type = lexeme_type_hash}},
    {RC_STR("("), {.type = lexeme_type_open_paren}},
    {RC_STR(")"), {.type = lexeme_type_close_paren}},
    // '}' closes a scope and so ends a statement: an implied/accumulator opcode may sit right before it
    // (`.routine { RTS }`). We must recognise it here rather than let it fall through as an unexpected char
    // that the no-operand peek would try to evaluate as an operand. It stays a `closer` in the statement
    // table (parse_block's brace handling relies on that); this operand-table row only lets the peek see it.
    {RC_STR("}"), {.type = lexeme_type_close_brace}},
};
static const token_table operand_tokens = RC_VIEW(operand_token_entries);

// The register-aware table: the same punctuation PLUS the registers A / X / Y. Used ONLY at the two operand
// positions where a register is grammatical - the accumulator (`ASL A`), and an index following a comma
// (`foo,X` / `(foo,X)` / `(foo),Y`). Kept separate from the default so a bare A/X/Y elsewhere stays a symbol;
// `ASL A` still resolves as accumulator mode first because we consult this table at the accumulator slot.
static const token operand_reg_token_entries[] = {
    {RC_STR("#"), {.type = lexeme_type_hash}},
    {RC_STR("("), {.type = lexeme_type_open_paren}},
    {RC_STR(")"), {.type = lexeme_type_close_paren}},
    {RC_STR("}"), {.type = lexeme_type_close_brace}},
    {RC_STR("A"), {.type = lexeme_type_register, .reg = {.which = reg_a}}},
    {RC_STR("X"), {.type = lexeme_type_register, .reg = {.which = reg_x}}},
    {RC_STR("Y"), {.type = lexeme_type_register, .reg = {.which = reg_y}}},
};
static const token_table operand_reg_tokens = RC_VIEW(operand_reg_token_entries);

static bool is_register(lexeme lx, reg_name which)
{
    return lx.type == lexeme_type_register && lx.reg.which == which;
}

static uint32_t mode_operand_bytes(addr_mode mode)
{
    switch (mode) {
        case addr_mode_imp:
        case addr_mode_acc:
            return 0;
        case addr_mode_imm:  case addr_mode_zp:  case addr_mode_zpx: case addr_mode_zpy:
        case addr_mode_indx: case addr_mode_indy: case addr_mode_ind: case addr_mode_rel:
            return 1;
        case addr_mode_abs:  case addr_mode_absx: case addr_mode_absy:
        case addr_mode_ind16: case addr_mode_ind16x:
            return 2;
        case addr_mode_max:
            break;
    }
    RC_UNREACHABLE();
}

typedef enum index_reg {
    index_none,
    index_x,
    index_y
} index_reg;

// Choose zero-page vs absolute for a plain/indexed operand. The rule that makes a forward
// reference assemble the same as a backward one: when the value is known, size it by the
// value (zp if it fits and a zp form exists); when it is not yet known, optimistically take
// the smallest available form, so a later pass can confirm it.
static addr_mode resolve_direct(mnemonic m, index_reg idx, bool known, int64_t addr)
{
    addr_mode zp = addr_mode_zp;
    addr_mode ab = addr_mode_abs;
    if (idx == index_x) { zp = addr_mode_zpx; ab = addr_mode_absx; }
    if (idx == index_y) { zp = addr_mode_zpy; ab = addr_mode_absy; }

    bool has_zp = opcode_def(m, zp) != 0;
    bool has_ab = opcode_def(m, ab) != 0;
    if (!has_ab) return zp;                 // only a zero-page form exists (or neither)
    if (!has_zp) return ab;                 // only an absolute form exists (e.g. JMP)
    if (!known)  return zp;                 // optimistic smallest while unresolved
    return (addr >= 0 && addr < 0x100) ? zp : ab;
}

// The outcome of looking for a trailing ",X" / ",Y" after a plain operand: which index (if
// any), the cursor past it, and any error.
typedef struct index_result {
    index_reg      reg;
    uint32_t       next;        // the separator position when there is no index
    error_type     error;
    uint32_t       error_at;
} index_result;

static index_result consume_index(rc_str source, uint32_t cursor)
{
    lexer_result a = lexer_next(source, cursor, operand_tokens);
    if (a.token.type != lexeme_type_comma) {
        return (index_result) {.reg = index_none, .next = cursor};
    }

    lexer_result reg = lexer_next(source, a.next, operand_reg_tokens);   // after a comma: a register is expected
    if (is_register(reg.token, reg_x)) {
        return (index_result) {.reg = index_x, .next = reg.next};
    }

    if (is_register(reg.token, reg_y)) {
        return (index_result) {.reg = index_y, .next = reg.next};
    }

    return (index_result) {
        .reg = index_none,
        .next = cursor,
        .error = error_type_bad_index_register,
        .error_at = a.next
    };
}

// Indirect modes dereference a zero-page POINTER: the pointer variable is read to form the effective
// address, so it is always a READ, whatever the instruction does to the pointed-to data.
static bool is_indirect_mode(addr_mode mode)
{
    return mode == addr_mode_indx || mode == addr_mode_indy || mode == addr_mode_ind;
}

// The instruction's control-flow class, from its cell's control flags (exclusive: at most one is set).
static zp_flow flow_from_cell(uint16_t cell)
{
    if (cell & op_branch) return zp_flow_branch;
    if (cell & op_jump)   return zp_flow_jump;
    if (cell & op_call)   return zp_flow_call;
    if (cell & op_return) return zp_flow_return;
    return zp_flow_normal;
}

// The attribution of one operand to a ZPAUTO variable: the referenced binding's identity (def cursor + the
// scope it was declared in) and how this instruction touches it. All-none / vref_none when the operand names
// no bound symbol.
typedef struct operand_ref {
    cursor   def;
    uint32_t scope;
    uint8_t  rw;
} operand_ref;

// If the operand's leading identifier resolves (with shadowing) to a bound symbol, return its identity and rw
// class; otherwise an all-none ref. The identity is mapped to a concrete vreg LATER (zeropage_resolve_vregs),
// once the whole ZPAUTO registry is populated, so a use before the declaration still attributes. The rw class
// comes from the cell for a DIRECT access (the variable IS the operand); an INDIRECT access reads the variable
// as a pointer to dereference, whatever the instruction does to the pointed-to data. `operand_pos` is where
// the operand expression begins, or RC_INDEX_NONE for a no-operand / immediate instruction (never a variable).
static operand_ref attribute_operand(baron *b, cursor at, uint32_t scope, addr_mode mode, uint16_t cell,
                                     uint32_t operand_pos)
{
    operand_ref none = {.def = cursor_none(), .scope = RC_INDEX_NONE, .rw = vref_none};
    if (operand_pos == RC_INDEX_NONE) {
        return none;
    }
    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lx = lexer_next(src, operand_pos, operand_tokens);
    if (lx.token.type != lexeme_type_identifier) {
        return none;   // a literal address or a register
    }
    symbol_ref ref = scopes_resolve_symbol_def(&b->scopes, scope, lx.token.identifier.name);
    if (cursor_is_none(ref.def)) {
        return none;   // resolves to nothing (undefined / not a bare symbol)
    }
    uint8_t rw = is_indirect_mode(mode)
                     ? (uint8_t) vref_read
                     : (uint8_t) (((cell & op_read) ? vref_read : 0) | ((cell & op_write) ? vref_write : 0));
    return (operand_ref) {.def = ref.def, .scope = ref.scope, .rw = rw};
}

// Record one assembled instruction into the zero-page IR (final active pass, feature on), for the CFG +
// liveness passes: its address + size, control-flow class + resolved target (branch/jump/call to a plain
// abs/rel address; an indirect/computed target stays RC_INDEX_NONE), and its variable touch (if any).
static void record_insn(baron *b, cursor at, uint32_t scope, parse_flags flags, addr_mode mode,
                        uint16_t cell, int_argument arg, uint32_t operand_base, uint32_t pc)
{
    if (!flags.final || !flags.active || !zeropage_is_enabled(&b->zeropage)) {
        return;
    }
    zp_flow  flow   = flow_from_cell(cell);
    uint32_t target = RC_INDEX_NONE;
    if ((flow == zp_flow_branch || flow == zp_flow_jump || flow == zp_flow_call)
        && arg.type == int_argument_type_known
        && (mode == addr_mode_rel || mode == addr_mode_abs)) {
        target = (uint32_t) (arg.value & 0xFFFF);
    }

    operand_ref op = attribute_operand(b, at, scope, mode, cell, operand_base);

    // The operand byte lands right after the opcode byte we are about to emit: the overlay's current code
    // length is the opcode's offset, so the operand is at +1. Recorded so the allocation patch can find it.
    uint32_t overlay        = b->current_overlay;
    uint32_t operand_offset = overlays_code(&b->overlays, overlay).num + 1;

    zeropage_add_insn(&b->zeropage, (zp_insn) {
        .pc             = pc,
        .size           = (uint16_t) (1 + mode_operand_bytes(mode)),
        .flow           = (uint8_t) flow,
        .rw             = op.rw,
        .vreg           = RC_INDEX_NONE,   // resolved from (var_scope, var_def) post-pass
        .var_scope      = op.scope,
        .var_def        = op.def,
        .target         = target,
        .overlay        = overlay,
        .operand_offset = operand_offset,
        .at             = at,
    });
}

struct parse_result opcode_parse(baron *b, mnemonic m, cursor at,
                                 uint32_t scope, parse_flags flags, rc_arena scratch)
{
    uint32_t source = at.source;
    uint32_t overlay = b->current_overlay;   // the overlay we emit into now (assembler-wide state)
    rc_str src = source_files_text(&b->source_files, source);
    uint32_t start = at.pos;              // just past the mnemonic
    uint32_t insn_pc = overlays_pc(&b->overlays, overlay);   // this instruction's address (before it emits)
    addr_mode mode;
    int_argument arg = {.type = int_argument_type_known};
    uint32_t operand_base = RC_INDEX_NONE;   // where a memory operand's expression begins (for VAR observation)
    uint32_t  after;                       // past the operand shell, before the separator

    lexer_result peek = lexer_next(src, start, operand_tokens);

    if (peek.token.type == lexeme_type_terminator || peek.token.type == lexeme_type_close_brace) {
        // No operand: implied, or accumulator for the shift / read-modify-write mnemonics. A '}' immediately
        // after the mnemonic counts as end-of-statement here (it closes the enclosing scope); we leave it for
        // require_separator, which recognises it as closing the statement.
        if (opcode_def(m, addr_mode_imp) != 0) {
            mode = addr_mode_imp;
        }
        else if (opcode_def(m, addr_mode_acc) != 0) {
            mode = addr_mode_acc;
        }
        else {
            return syntax_error(b, error_type_missing_operand, cursor_at(at, start));
        }
        after = start;                     // leave the terminator for require_separator
    }
    else if (peek.token.type == lexeme_type_hash) {
        expr_result e = eval(b, cursor_at(at, peek.next), scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }
        arg = int_argument_make(e.value, flags.final, peek.next);
        mode = addr_mode_imm;
        after = e.next;
    }
    else if (peek.token.type == lexeme_type_open_paren) {
        operand_base = peek.next;   // the pointer expression inside the parentheses
        expr_result e = eval(b, cursor_at(at, peek.next), scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }
        arg = int_argument_make(e.value, flags.final, peek.next);

        // Closing shell: "(expr,X)" -> indexed-indirect; "(expr),Y" -> indirect-indexed;
        // "(expr)" -> indirect (JMP's ind16, or a CMOS zero-page indirect).
        lexer_result a = lexer_next(src, e.next, operand_tokens);
        if (a.token.type == lexeme_type_comma) {
            lexer_result reg = lexer_next(src, a.next, operand_reg_tokens);   // "(expr,X)": X expected
            if (!is_register(reg.token, reg_x)) {
                return syntax_error(b, error_type_bad_index_register, cursor_at(at, a.next));
            }
            lexer_result cp = lexer_next(src, reg.next, operand_tokens);
            if (cp.token.type != lexeme_type_close_paren) {
                return syntax_error(b, error_type_expected_close_paren, cursor_at(at, reg.next));
            }
            mode = addr_mode_indx;
            after = cp.next;
        }
        else if (a.token.type == lexeme_type_close_paren) {
            lexer_result tail = lexer_next(src, a.next, operand_tokens);
            if (tail.token.type == lexeme_type_comma) {
                lexer_result reg = lexer_next(src, tail.next, operand_reg_tokens);   // "(expr),Y": Y expected
                if (!is_register(reg.token, reg_y)) {
                    return syntax_error(b, error_type_bad_index_register, cursor_at(at, tail.next));
                }
                mode  = addr_mode_indy;
                after = reg.next;
            }
            else {
                mode = (opcode_def(m, addr_mode_ind16) != 0) ? addr_mode_ind16 : addr_mode_ind;
                after = a.next;
            }
        }
        else {
            return syntax_error(b, error_type_expected_close_paren, cursor_at(at, e.next));
        }
    }
    else {
        // Plain operand. First, a bare accumulator 'A' on a shift / rmw mnemonic - consulted with the
        // register-aware table, so `ASL A` reads as accumulator mode even when a symbol `a` exists, yet a
        // real symbol operand (`ASL data`) falls straight through to the expression parser below. It counts
        // as the accumulator only when nothing but a statement terminator (or a scope-closing '}') follows.
        bool handled = false;
        if (opcode_def(m, addr_mode_acc) != 0) {
            lexer_result areg = lexer_next(src, start, operand_reg_tokens);
            if (is_register(areg.token, reg_a)) {
                lexer_result after_a = lexer_next(src, areg.next, operand_tokens);
                if (after_a.token.type == lexeme_type_terminator
                    || after_a.token.type == lexeme_type_close_brace) {
                    mode = addr_mode_acc;
                    after = areg.next;
                    handled = true;
                }
            }
        }
        if (!handled) {
            operand_base = start;   // the zero-page / absolute operand expression
            expr_result e = eval(b, cursor_at(at, start), scope, scratch);
            if (e.error != expr_error_none) {
                return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
            }
            arg = int_argument_make(e.value, flags.final, start);

            index_result ix = consume_index(src, e.next);
            if (ix.error != error_type_none) {
                return syntax_error(b, ix.error, cursor_at(at, ix.error_at));
            }

            // A relative branch takes a bare target; everything else is zero-page/absolute.
            if (opcode_def(m, addr_mode_rel) != 0 && ix.reg == index_none) {
                mode = addr_mode_rel;
            }
            else {
                mode = resolve_direct(m, ix.reg, arg.type == int_argument_type_known, arg.value);
            }
            after = ix.next;
        }
    }

    // A dead IF branch is parsed for structure only: the operand has been consumed, so just step over
    // the separator. Everything below - the value error, encoding, emission, range checks and the
    // forward-reference flag - is the active path.
    if (!flags.active) {
        return require_separator(b, cursor_at(at, after));
    }

    // A value that can never be an address is recoverable: record it and emit a best-effort 0 operand,
    // so the instruction keeps its size and the layout still settles.
    if (arg.type == int_argument_type_error) {
        semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));
    }

    uint16_t cell = opcode_def(m, mode);
    if (cell == 0 || (cell & cmos) != 0) {            // NMOS target: a CMOS-only encoding is unavailable
        // No encoding for this operand shape: record it and emit nothing (a stable zero-byte best effort).
        semantic_error(b, flags, error_type_bad_addressing_mode, cursor_at(at, start));
        return require_separator(b, cursor_at(at, after));
    }

    // Record this instruction into the ZP IR (final active pass, feature on) for the CFG + liveness passes.
    record_insn(b, at, scope, flags, mode, cell, arg, operand_base, insn_pc);

    overlays_emit_u8(&b->overlays, overlay, (uint8_t)(cell & 0xFF));

    uint32_t width = mode_operand_bytes(mode);
    if (width == 1) {
        if (mode == addr_mode_rel) {
            uint8_t off = 0;
            if (arg.type == int_argument_type_known) {
                // From the address after the instruction (the offset byte we are about to emit).
                int64_t delta = arg.value - (int64_t)(overlays_pc(&b->overlays, overlay) + 1);
                if (delta < -128 || delta > 127) {
                    semantic_error(b, flags, error_type_branch_out_of_range, cursor_at(at, start));
                }
                off = (uint8_t)(int8_t)delta;   // best-effort: the low byte of the (out-of-range) delta
            }
            overlays_emit_u8(&b->overlays, overlay, off);
        }
        else {
            int64_t lo = (mode == addr_mode_imm) ? -128 : 0;   // immediates may be written signed (#-1)
            if (arg.type == int_argument_type_known && (arg.value < lo || arg.value > 0xFF)) {
                semantic_error(b, flags, error_type_value_out_of_range, cursor_at(at, start));
            }
            overlays_emit_u8(&b->overlays, overlay, (uint8_t)(arg.value & 0xFF));
        }
    }
    else if (width == 2) {
        if (arg.type == int_argument_type_known && (arg.value < 0 || arg.value > 0xFFFF)) {
            semantic_error(b, flags, error_type_value_out_of_range, cursor_at(at, start));
        }
        // NMOS hardware bug: an indirect JMP through a vector whose low byte is at $xxFF fetches the high
        // byte from $xx00, not the next page. Legal but almost always a mistake, so warn (not an error).
        if (mode == addr_mode_ind16 && arg.type == int_argument_type_known && (arg.value & 0xFF) == 0xFF) {
            semantic_warning(b, flags, error_type_jmp_indirect_page_cross, cursor_at(at, start), severity_warning);
        }
        overlays_emit_u16(&b->overlays, overlay, (uint16_t)(arg.value & 0xFFFF));
    }

    // The separator follows; carry forward whether the operand was a forward reference.
    parse_result r = require_separator(b, cursor_at(at, after));
    r.unresolved = (arg.type == int_argument_type_unresolved);
    return r;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(opcodes, bytes_and_jsr_fix)
{
    RC_CHECK((uint32_t)(opcode_def(mnemonic_lda, addr_mode_imm) & 0xFF), ==, 0xA9u);
    RC_CHECK((uint32_t)(opcode_def(mnemonic_lda, addr_mode_zp)  & 0xFF), ==, 0xA5u);
    RC_CHECK((uint32_t)(opcode_def(mnemonic_lda, addr_mode_abs) & 0xFF), ==, 0xADu);
    RC_CHECK((uint32_t)(opcode_def(mnemonic_jsr, addr_mode_abs) & 0xFF), ==, 0x20u);   // was 0xC8 in baron-c

    // An unsupported mode is a zero cell. A plain cell is both CPUs; the cmos flag marks
    // CMOS-only (so unavailable on the NMOS target).
    RC_CHECK((uint32_t)opcode_def(mnemonic_jmp, addr_mode_zp), ==, 0u);
    RC_CHECK_TRUE((opcode_def(mnemonic_lda, addr_mode_zp)  & cmos) == 0);   // both CPUs
    RC_CHECK_TRUE((opcode_def(mnemonic_lda, addr_mode_ind) & cmos) != 0);   // (zp) is CMOS-only
}

RC_TEST(opcodes, call_analysis_class)
{
    // Per-(mnemonic, mode) classification: the accumulator form touches no memory operand,
    // the zero-page form is a read-modify-write.
    RC_CHECK_TRUE((opcode_def(mnemonic_asl, addr_mode_acc) & op_class_mask) == 0);
    RC_CHECK_TRUE((opcode_def(mnemonic_asl, addr_mode_zp)  & op_class_mask) == (op_read | op_write));

    // Immediate is no memory operand; a real address is a read.
    RC_CHECK_TRUE((opcode_def(mnemonic_lda, addr_mode_imm) & op_class_mask) == 0);
    RC_CHECK_TRUE((opcode_def(mnemonic_lda, addr_mode_zp)  & op_class_mask) == op_read);

    RC_CHECK_TRUE((opcode_def(mnemonic_sta, addr_mode_zp)  & op_class_mask) == op_write);
    RC_CHECK_TRUE((opcode_def(mnemonic_jsr, addr_mode_abs) & op_class_mask) == op_call);
    RC_CHECK_TRUE((opcode_def(mnemonic_rts, addr_mode_imp) & op_class_mask) == op_return);
    RC_CHECK_TRUE((opcode_def(mnemonic_beq, addr_mode_rel) & op_class_mask) == op_branch);
    RC_CHECK_TRUE((opcode_def(mnemonic_jmp, addr_mode_abs) & op_class_mask) == op_jump);
    RC_CHECK_TRUE((opcode_def(mnemonic_tax, addr_mode_imp) & op_class_mask) == 0);
}

#endif // BARON_TESTS
