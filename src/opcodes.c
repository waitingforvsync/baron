#include "opcodes.h"

#include "assemble_internal.h"   // parse_result, int_argument, int_argument_make, require_separator, parse_fail
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

// The opcode-operand token table: '#' (immediate), the parens (indirect), and the registers
// A / X / Y. A bare register is recognised here rather than guessed from an identifier, and a
// longer identifier still wins (so a symbol "xyz" is not the X register).
static const token operand_token_entries[] = {
    {RC_STR("#"), {.type = lexeme_type_hash}},
    {RC_STR("("), {.type = lexeme_type_open_paren}},
    {RC_STR(")"), {.type = lexeme_type_close_paren}},
    {RC_STR("A"), {.type = lexeme_type_register, .reg = {.which = reg_a}}},
    {RC_STR("X"), {.type = lexeme_type_register, .reg = {.which = reg_x}}},
    {RC_STR("Y"), {.type = lexeme_type_register, .reg = {.which = reg_y}}},
};
static const token_table operand_tokens = RC_VIEW(operand_token_entries);

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
    assemble_error error;
    uint32_t       error_at;
} index_result;

static index_result consume_index(rc_str source, uint32_t cursor)
{
    lexer_result a = lexer_next(source, cursor, operand_tokens);
    if (a.token.type != lexeme_type_comma) {
        return (index_result) {.reg = index_none, .next = cursor};
    }

    lexer_result reg = lexer_next(source, a.next, operand_tokens);
    if (is_register(reg.token, reg_x)) {
        return (index_result) {.reg = index_x, .next = reg.next};
    }

    if (is_register(reg.token, reg_y)) {
        return (index_result) {.reg = index_y, .next = reg.next};
    }

    return (index_result) {
        .reg = index_none,
        .next = cursor,
        .error = assemble_error_bad_index_register,
        .error_at = a.next
    };
}

struct parse_result opcode_parse(baron *b, mnemonic m, cursor at,
                                 uint32_t scope, parse_flags flags, rc_arena scratch)
{
    uint32_t source = at.source;
    uint32_t overlay = b->current_overlay;   // the overlay we emit into now (assembler-wide state)
    rc_str src = source_files_text(&b->source_files, source);
    uint32_t start = at.pos;              // just past the mnemonic
    addr_mode mode;
    int_argument arg = {.type = int_argument_type_known};
    uint32_t  after;                       // past the operand shell, before the separator

    lexer_result peek = lexer_next(src, start, operand_tokens);

    if (peek.token.type == lexeme_type_terminator) {
        // No operand: implied, or accumulator for the shift / read-modify-write mnemonics.
        if (opcode_def(m, addr_mode_imp) != 0) {
            mode = addr_mode_imp;
        }
        else if (opcode_def(m, addr_mode_acc) != 0) {
            mode = addr_mode_acc;
        }
        else {
            return parse_fail(assemble_error_missing_operand, start);
        }
        after = start;                     // leave the terminator for require_separator
    }
    else if (peek.token.type == lexeme_type_hash) {
        expr_result e = expression_parse(src, peek.next, &b->scopes, scope, &scratch);
        if (e.error != expr_error_none) {
            return parse_fail(assemble_error_expression, e.error_at);
        }
        arg = int_argument_make(e.value, flags.final, peek.next);
        mode = addr_mode_imm;
        after = e.next;
    }
    else if (peek.token.type == lexeme_type_open_paren) {
        expr_result e = expression_parse(src, peek.next, &b->scopes, scope, &scratch);
        if (e.error != expr_error_none) {
            return parse_fail(assemble_error_expression, e.error_at);
        }
        arg = int_argument_make(e.value, flags.final, peek.next);

        // Closing shell: "(expr,X)" -> indexed-indirect; "(expr),Y" -> indirect-indexed;
        // "(expr)" -> indirect (JMP's ind16, or a CMOS zero-page indirect).
        lexer_result a = lexer_next(src, e.next, operand_tokens);
        if (a.token.type == lexeme_type_comma) {
            lexer_result reg = lexer_next(src, a.next, operand_tokens);
            if (!is_register(reg.token, reg_x)) {
                return parse_fail(assemble_error_bad_index_register, a.next);
            }
            lexer_result cp = lexer_next(src, reg.next, operand_tokens);
            if (cp.token.type != lexeme_type_close_paren) {
                return parse_fail(assemble_error_expected_close_paren, reg.next);
            }
            mode = addr_mode_indx;
            after = cp.next;
        }
        else if (a.token.type == lexeme_type_close_paren) {
            lexer_result tail = lexer_next(src, a.next, operand_tokens);
            if (tail.token.type == lexeme_type_comma) {
                lexer_result reg = lexer_next(src, tail.next, operand_tokens);
                if (!is_register(reg.token, reg_y)) {
                    return parse_fail(assemble_error_bad_index_register, tail.next);
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
            return parse_fail(assemble_error_expected_close_paren, e.next);
        }
    }
    else {
        // Plain operand. First, a bare accumulator 'A' on a shift / rmw mnemonic.
        bool handled = false;
        if (is_register(peek.token, reg_a) && opcode_def(m, addr_mode_acc) != 0) {
            lexer_result after_a = lexer_next(src, peek.next, operand_tokens);
            if (after_a.token.type == lexeme_type_terminator) {
                mode = addr_mode_acc;
                after = peek.next;
                handled = true;
            }
        }
        if (!handled) {
            expr_result e = expression_parse(src, start, &b->scopes, scope, &scratch);
            if (e.error != expr_error_none) {
                return parse_fail(assemble_error_expression, e.error_at);
            }
            arg = int_argument_make(e.value, flags.final, start);

            index_result ix = consume_index(src, e.next);
            if (ix.error != assemble_error_none) {
                return parse_fail(ix.error, ix.error_at);
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
        return require_separator(src, after);
    }

    if (arg.type == int_argument_type_error) {
        return parse_fail(arg.error, arg.error_at);
    }

    uint16_t cell = opcode_def(m, mode);
    if (cell == 0 || (cell & cmos) != 0) {            // NMOS target: a CMOS-only encoding is unavailable
        return parse_fail(assemble_error_bad_addressing_mode, start);
    }
    overlays_emit_u8(&b->overlays, overlay, (uint8_t)(cell & 0xFF));

    uint32_t width = mode_operand_bytes(mode);
    if (width == 1) {
        if (mode == addr_mode_rel) {
            uint8_t off = 0;
            if (arg.type == int_argument_type_known) {
                // From the address after the instruction (the offset byte we are about to emit).
                int64_t delta = arg.value - (int64_t)(overlays_pc(&b->overlays, overlay) + 1);
                if (flags.final && (delta < -128 || delta > 127)) {
                    return parse_fail(assemble_error_branch_out_of_range, start);
                }
                off = (uint8_t)(int8_t)delta;
            }
            overlays_emit_u8(&b->overlays, overlay, off);
        }
        else {
            int64_t lo = (mode == addr_mode_imm) ? -128 : 0;   // immediates may be written signed (#-1)
            if (arg.type == int_argument_type_known && flags.final && (arg.value < lo || arg.value > 0xFF)) {
                return parse_fail(assemble_error_value_out_of_range, start);
            }
            overlays_emit_u8(&b->overlays, overlay, (uint8_t)(arg.value & 0xFF));
        }
    }
    else if (width == 2) {
        if (arg.type == int_argument_type_known && flags.final && (arg.value < 0 || arg.value > 0xFFFF)) {
            return parse_fail(assemble_error_value_out_of_range, start);
        }
        overlays_emit_u16(&b->overlays, overlay, (uint16_t)(arg.value & 0xFFFF));
    }

    // The separator follows; carry forward whether the operand was a forward reference.
    parse_result r = require_separator(src, after);
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
